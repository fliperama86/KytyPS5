#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/cache/readbackDiagnostics.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/replay/frameCapture.h"
#include "kernel/memory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <tuple>
#include <vector>

namespace Libs::Graphics {

GpuResourceManager::GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_scheduler(scheduler), m_buffer_cache(graphics, scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, scheduler, m_page_manager, m_buffer_cache) {
	// Step 1 of docs/sync-points-design.md: everything the guest committed before this object
	// existed -- a replay restores gigabytes of it -- is imported at the first preparation.
	m_import_pending.store(m_buffer_cache.PrologueTableEnabled(), std::memory_order_relaxed);
}

GpuResourceManager::~GpuResourceManager() = default;

bool GpuResourceManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else if (GuestGpu::IsGpuThread()) {
		// Item 1b (docs/performance-roadmap.md): a read fault on a GPU-dirty page is what turns an
		// SRT evaluation into a device drain. Always counted; --gpu-readback-diagnostics also
		// times it and records what was being evaluated.
		ReadbackDiag::CountFault();
		if (ReadbackDiag::Enabled()) {
			ReadbackDiag::BeginFault(fault_vaddr);
			m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
			ReadbackDiag::EndFault();
		} else {
			m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
		}
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool GpuResourceManager::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool GpuResourceManager::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void GpuResourceManager::MapMemory(uint64_t vaddr, uint64_t size) {
	{
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Add(vaddr, size);
		++m_mapped_generation;
		// Buffers may already cover the new mapping; the next preparation must revisit it.
		m_buffer_cache.InvalidateBda(vaddr, size);
		// Step 1 of docs/sync-points-design.md: the range has to reach the prologue page table as
		// imported host memory. The import itself is Vulkan work on a recording command buffer,
		// so it waits for the GPU thread rather than blocking this one.
		if (m_buffer_cache.PrologueTableEnabled()) {
			m_import_pending.store(true, std::memory_order_release);
		}
	}
	// Frame capture (docs/frame-replay.md, phase E): a guest map moves both generations, and a
	// replay of one frame never maps anything.
	Replay::RecordChurnEvent(Replay::ChurnEventKind::Map, vaddr, size);
}

void GpuResourceManager::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		if (m_scheduler.Active()) {
			const auto tick = m_scheduler.CurrentTick();
			m_scheduler.Finish();
			m_scheduler.WaitPriorityOperations(tick);
		}
		m_buffer_cache.InvalidateMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		// Step 1: the import has to be gone before the guest decommits the pages behind it. The
		// wait above has already drained the device, so nothing can be reading it; what is left
		// is to clear the entries that pointed there and let the objects go.
		m_buffer_cache.ReleaseGuestRange(vaddr, size);
		// A release drops whole imports, so a mapping the range only clipped has to be covered
		// again; the next preparation re-imports whatever is still mapped.
		if (m_buffer_cache.PrologueTableEnabled()) {
			m_import_pending.store(true, std::memory_order_release);
		}
		m_mapped_ranges.Subtract(vaddr, size);
		++m_mapped_generation;
		// Nothing can be uploaded to an unmapped range, and it must not linger in the set.
		m_buffer_cache.ForgetBdaRange(vaddr, size);
		Replay::RecordChurnEvent(Replay::ChurnEventKind::Unmap, vaddr, size);
	};
	if (m_gpu == nullptr) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

// Step 1 of docs/sync-points-design.md. The mapped set is the truth: every part of it the imports
// do not cover yet is imported here and its prologue entries filled, whatever order the guest's
// map calls and the GPU thread's start-up happened in. An import that fails leaves its pages with
// today's behaviour, a zero entry and the fault path.
void GpuResourceManager::ImportPendingRanges() {
	if (!m_import_pending.load(std::memory_order_acquire) || !m_scheduler.Active()) {
		return;
	}
	m_import_pending.store(false, std::memory_order_relaxed);
	// The kernel's committed ranges, not the GPU's mapped set: a range committed before the GPU
	// thread existed never reached MapMemory, and the prologue has to be able to read it too.
	// SnapshotVirtualRanges and SnapshotGuestBackingViews take their own lock and never the
	// memory-operation mutex, so a guest thread waiting on this thread inside a memory syscall
	// cannot deadlock them -- the frame capture reads the ranges from here for the same reason.
	// Never a coalesced span of several ranges: an import that crosses two guest mappings is
	// refused by the driver however contiguous the addresses look.
	std::vector<RangeSet::Range> ranges;
	for (const auto& range: Libs::LibKernel::Memory::SnapshotVirtualRanges()) {
		if (range.is_committed && range.start != 0 && range.size != 0) {
			ranges.push_back({range.start, range.size});
		}
	}
	std::sort(ranges.begin(), ranges.end(),
	          [](const RangeSet::Range& a, const RangeSet::Range& b) {
		          return a.address < b.address;
	          });
	const auto before = ReadBdaPrologueCounters();
	// One import for all of direct, flexible and pooled memory: they are views of the guest's
	// backing store, and the driver takes that single 13.5 GiB allocation where it refuses even a
	// few hundred MiB of separate views (docs/sync-points-design.md, step 1).
	uint64_t alias_base = 0;
	uint64_t alias_size = 0;
	if (!m_buffer_cache.BackingAliasImported() &&
	    Libs::LibKernel::Memory::GetGuestBackingAlias(&alias_base, &alias_size)) {
		m_buffer_cache.ImportBackingAlias(alias_base, alias_size);
	}
	if (m_buffer_cache.BackingAliasImported()) {
		for (const auto& view: Libs::LibKernel::Memory::SnapshotGuestBackingViews()) {
			m_buffer_cache.MapBackingView(view.vaddr, view.size, view.backing_offset);
		}
	}

	for (const auto& range: ranges) {
		m_buffer_cache.ImportGuestRange(range.address, range.size);
	}
	const auto after = ReadBdaPrologueCounters();
	if (after.imports != before.imports) {
		std::printf("gpu-prologue-table: imported %llu guest ranges, %.1f MiB, in %.0f ms "
		            "(%llu alive, %.1f MiB; %llu failed)\n",
		            static_cast<unsigned long long>(after.imports - before.imports),
		            static_cast<double>(after.import_bytes - before.import_bytes) /
		                (1024.0 * 1024.0),
		            static_cast<double>(after.import_ns - before.import_ns) / 1.0e6,
		            static_cast<unsigned long long>(after.import_ranges),
		            static_cast<double>(after.import_bytes) / (1024.0 * 1024.0),
		            static_cast<unsigned long long>(after.import_failures));
		const auto detail = m_buffer_cache.DescribeGuestImports();
		std::printf("gpu-prologue-table: %s\n", detail.c_str());
		std::fflush(stdout);
	}
}

void GpuResourceManager::PrepareBda() {
	// Both are a predictable branch with --gpu-prologue-table off.
	ImportPendingRanges();
	m_buffer_cache.FlushPrologueTable();
	std::shared_lock lock(m_mapped_ranges_mutex);
	const auto buffer_generation = m_buffer_cache.BdaGeneration();
	const auto mapped_generation = m_mapped_generation;
	if (!BdaScanRequired(buffer_generation, mapped_generation)) {
		m_fault_process_pending = true;
		// Frame replay (docs/frame-replay.md, phase E): every preparation is recorded, scanning
		// or not, because the number of scans per preparation is what a replay must reproduce.
		Replay::RecordPrepareEvent(Replay::PrepareSample {});
		return;
	}
	ScanBda(buffer_generation, mapped_generation);
}

// Design P (docs/bda-sync-design.md), the submission boundary: wait for the re-protection helper
// to have drained, then scan once so every page it protected is uploaded again and clean before
// the submission's first draw. The scan is forced rather than driven by a generation bump,
// because a landing does not otherwise need a scan of its own -- a draw of this submission may
// only read what the guest wrote before it was submitted, and anything written after a page's
// upload is later than that. Called with the scheduler already recording.
void GpuResourceManager::AsyncProtectBoundary() {
	if (!m_buffer_cache.AsyncProtectEnabled()) {
		return;
	}
	m_buffer_cache.DrainAsyncProtect();
	const auto started = std::chrono::steady_clock::now();
	{
		std::shared_lock lock(m_mapped_ranges_mutex);
		ScanBda(m_buffer_cache.BdaGeneration(), m_mapped_generation, false);
	}
	RecordAsyncProtectBoundaryScan(
	    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                              std::chrono::steady_clock::now() - started)
	                              .count()));
}

// The scan itself, with m_mapped_ranges_mutex held. PrepareBda reaches it when a generation moved;
// the submission boundary forces it.
void GpuResourceManager::ScanBda(uint64_t buffer_generation, uint64_t mapped_generation,
                                 bool record) {
	// The scan is timed only while a capture or a replay is watching, so a normal run pays one
	// relaxed load and no clock reads. A boundary scan is not recorded: the stream it would go
	// into is the game's own preparation timeline, which a replay is measured against, and the
	// boundary's scans have their own counters.
	const bool watched = record && Replay::PrepareEventsWatched();
	const auto started = watched ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
	const auto protect_calls_before = watched ? PageProtectThreadCallCount() : 0;
	const auto protect_pages_before = watched ? PageProtectThreadPageCount() : 0;
	KYTY_PROFILER_BLOCK("GpuResourceManager::SynchronizeBdaBuffers");
	// Take the dirty set before the scan. A range added afterwards also advances the
	// generation captured above, so the next call picks it up.
	const auto dirty = m_buffer_cache.TakeBdaDirtyRanges();
	uint32_t   dirty_ranges = 0;
	uint32_t   synchronized = 0;
	uint64_t   dirty_bytes  = 0;
	dirty.ForEach([&](uint64_t start, uint64_t end) {
		dirty_ranges++;
		dirty_bytes += end - start;
		m_mapped_ranges.ForEachIntersection(start, end - start, [&](RangeSet::Range range) {
			synchronized++;
			m_buffer_cache.SynchronizeBuffersInRange(range.address, range.size);
		});
	});
	// Publish the generations captured before the scan. A concurrent buffer invalidation
	// may have advanced the live generation while scanning and must force the next call
	// to scan again.
	m_last_bda_buffer_generation = buffer_generation;
	m_last_bda_mapped_generation = mapped_generation;
	m_fault_process_pending = true;
	if (watched) {
		Replay::PrepareSample sample;
		sample.scanned      = true;
		sample.dirty_ranges = dirty_ranges;
		sample.synchronized = synchronized;
		sample.dirty_bytes  = dirty_bytes;
		sample.scan_ns      = static_cast<uint32_t>(
		         std::min<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                          std::chrono::steady_clock::now() - started)
		                          .count(),
		                      UINT32_MAX));
		// The re-protection the scan's uploads caused, on this thread only: every
		// SynchronizeBuffersInRange that uploads a page has the tracker protect it again, which is
		// a kernel call and, with guest threads running, a TLB shootdown across every one of them.
		// Design P moves those calls to a helper thread, where they do not count here.
		sample.protect_calls =
		    static_cast<uint32_t>(PageProtectThreadCallCount() - protect_calls_before);
		sample.protect_pages =
		    static_cast<uint32_t>(PageProtectThreadPageCount() - protect_pages_before);
		Replay::RecordPrepareEvent(sample);
	}
}

bool GpuResourceManager::BdaScanRequired(uint64_t buffer_generation,
                                         uint64_t mapped_generation) const noexcept {
	return buffer_generation != m_last_bda_buffer_generation ||
	       mapped_generation != m_last_bda_mapped_generation;
}

void GpuResourceManager::RunGarbageCollector() {
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
		// Same schedule and mechanism as the fault buffer: a mismatch a shader reported becomes
		// visible to the program cache one or more frames later, with no GPU drain.
		m_buffer_cache.ProcessDescriptorFeedback();
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

} // namespace Libs::Graphics
