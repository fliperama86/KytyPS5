#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"

#include "common/assert.h"
#include "common/guestPageWatch.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
namespace Libs::Graphics {

GpuResourceManager::GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_scheduler(scheduler), m_buffer_cache(graphics, scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, scheduler, m_page_manager, m_buffer_cache) {}

GpuResourceManager::~GpuResourceManager() = default;

bool GpuResourceManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	// Answered before the mapped-range gate: the watch is the only owner of a page it protected
	// on its own, and leaving such a fault unhandled kills the process.
	const bool watched = access == PageFaultAccess::Write &&
	                     Common::GuestPageWatch::InvalidateOnFault(fault_vaddr);
	if (!IsMapped(fault_vaddr, fault_size)) {
		return watched;
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
	Common::GuestPageWatch::Invalidate(vaddr, size);
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

// Only a mapped page may be watched. The address space silently skips the unmapped parts of a
// protection request, so an unmapped page would be recorded as watched and never fault; a page
// outside the mapped set also has no owner to answer its fault.
bool GpuResourceManager::CanWatchPage(uint64_t page_address) const noexcept {
	return IsMapped(page_address, Common::GuestPageWatch::PAGE_SIZE);
}

bool GpuResourceManager::WatchPage(uint64_t page_address, bool arm) noexcept {
	constexpr uint64_t page_size = Common::GuestPageWatch::PAGE_SIZE;
	if (arm) {
		m_page_manager.UpdatePageWatchers<true>(page_address, page_size);
	} else {
		m_page_manager.UpdatePageWatchers<false>(page_address, page_size);
	}
	return true;
}

void GpuResourceManager::MapMemory(uint64_t vaddr, uint64_t size) {
	// A fresh mapping holds different bytes; nothing may still be watching the old ones.
	Common::GuestPageWatch::Invalidate(vaddr, size);
	{
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Add(vaddr, size);
		++m_mapped_generation;
		// Buffers may already cover the new mapping; the next preparation must revisit it.
		m_buffer_cache.InvalidateBda(vaddr, size);
	}
}

void GpuResourceManager::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	const auto unmap = [this, vaddr, size] {
		// Before the mapping goes away, and before m_mapped_ranges_mutex is taken: arming needs
		// that lock, so the watch must never be dropped while it is held.
		Common::GuestPageWatch::Invalidate(vaddr, size);
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
		return;
	}
	KYTY_PROFILER_BLOCK("GpuResourceManager::SynchronizeBdaBuffers");
	// Take the dirty set before the scan. A range added afterwards also advances the
	// generation captured above, so the next call picks it up.
	const auto dirty = m_buffer_cache.TakeBdaDirtyRanges();
	dirty.ForEach([this](uint64_t start, uint64_t end) {
		m_mapped_ranges.ForEachIntersection(start, end - start, [this](RangeSet::Range range) {
			m_buffer_cache.SynchronizeBuffersInRange(range.address, range.size);
		});
	});
	// Publish the generations captured before the scan. A concurrent buffer invalidation
	// may have advanced the live generation while scanning and must force the next call
	// to scan again.
	m_last_bda_buffer_generation = buffer_generation;
	m_last_bda_mapped_generation = mapped_generation;
	m_fault_process_pending = true;
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
	}
	m_texture_cache.ProcessDownloadImages();
	m_texture_cache.RunGarbageCollector();
	m_buffer_cache.RunGarbageCollector();
}

} // namespace Libs::Graphics
