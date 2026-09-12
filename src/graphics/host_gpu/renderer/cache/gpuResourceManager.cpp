#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/replay/frameCapture.h"

#include <algorithm>
#include <chrono>
namespace Libs::Graphics {

GpuResourceManager::GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_scheduler(scheduler), m_buffer_cache(graphics, scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, scheduler, m_page_manager, m_buffer_cache) {}

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

void GpuResourceManager::PrepareBda() {
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
