#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_

#include "common/assert.h"
#include "common/profiler.h"
#include "common/threads.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/regionManager.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// What design P (docs/bda-sync-design.md) did, per process, since the last reset. The frame
// replay prints and reports these; nothing else reads them.
struct AsyncProtectCounters {
	uint64_t upload_protect_calls = 0; // kernel calls the deferred upload path made: must be 0
	uint64_t upload_protect_pages = 0;
	// A written range is claimed by the GPU in the same call and protected at no-access either
	// way, so it is not deferred and its protections are counted apart.
	uint64_t written_protect_calls = 0;
	uint64_t written_protect_pages = 0;
	uint64_t helper_protect_calls = 0; // kernel calls the helper made, after coalescing
	uint64_t helper_protect_pages = 0;
	uint64_t helper_batches       = 0;
	uint64_t landed_pages         = 0;
	uint64_t extra_uploads        = 0; // pages a later scan uploaded a second time
	uint64_t drains               = 0; // submission boundaries that asked for a drain
	uint64_t waits                = 0; // of those, the ones that had to wait
	uint64_t wait_ns              = 0;
	uint64_t boundary_scans       = 0; // forced BDA scans at those boundaries
	uint64_t boundary_scan_ns     = 0;
};

[[nodiscard]] AsyncProtectCounters ReadAsyncProtectCounters() noexcept;
void                               ResetAsyncProtectCounters() noexcept;
// What the forced scan at a submission boundary cost, counted by the resource manager.
void                               RecordAsyncProtectBoundaryScan(uint64_t ns) noexcept;

// The landed runs of one helper batch, handed to the buffer cache so it can put them back in the
// BDA dirty set and move the generation once for the whole batch.
using AsyncProtectSink = std::function<void(const GuestRange*, size_t)>;

class AsyncProtectHelper;

class MemoryTracker final {
public:
	explicit MemoryTracker(PageManager& page_manager);
	~MemoryTracker();

	KYTY_CLASS_NO_COPY(MemoryTracker);

	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size);
	void               MarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	void               UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size);
	void               UntrackMemory(uint64_t vaddr, uint64_t size);
	// Removes protection from a range and flushes GPU-owned data when required.
	template <typename Flush>
	void InvalidateRegion(uint64_t vaddr, uint64_t size, Flush&& on_flush) noexcept {
		static_assert(std::is_invocable_v<Flush&>);
		CheckNotInUploadCallback();

		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			const bool should_flush = [&] {
				// Perform both the GPU modification check and CPU state change with the lock in
				// case the GPU thread is racing to mark the page modified. If a flush is needed,
				// on_flush performs the CPU state change.
				std::scoped_lock lock(manager->lock);
				if (manager->IsModified<DirtySource::Gpu>(offset, bytes)) {
					return true;
				}
				manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
				return false;
			}();
			if (should_flush) {
				on_flush();
			}
		});
	}
#if KYTY_BUILD == KYTY_BUILD_DEBUG
	void ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                           const char* operation) const noexcept;
	void ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
	                               const char* operation);
#else
	void ValidateGpuDirtyPages(const RangeSet&, uint64_t, uint64_t, const char*) const noexcept {}
	void ValidateGpuDirtyOwnership(const RangeSet&, uint64_t, uint64_t, const char*) {}
#endif

	template <bool clear, typename Preflight, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Preflight&& preflight, Func&& func) {
		static_assert(std::is_nothrow_invocable_v<Preflight&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<Func&, uint64_t, uint64_t>);
		CheckNotInUploadCallback();
		std::vector<RegionManager*> managers;
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t, uint64_t) {
			managers.push_back(manager);
		});
		std::vector<std::unique_lock<TrackingSpinLock>> locks;
		locks.reserve(managers.size());
		for (auto* manager: managers) {
			locks.emplace_back(manager->lock);
		}
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			const auto address = manager->GetCpuAddr() + offset;
			manager->template ForEachModifiedRange<DirtySource::Gpu, false>(address, bytes,
			                                                                preflight);
		});
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			manager->template ForEachModifiedRange<DirtySource::Gpu, false>(
			    manager->GetCpuAddr() + offset, bytes, func);
		});
		if constexpr (clear) {
			Iterate<false>(vaddr, size,
			               [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               const auto address = manager->GetCpuAddr() + offset;
				               manager->template ForEachModifiedRange<DirtySource::Gpu, true>(
				                   address, bytes, [](uint64_t, uint64_t) noexcept {});
			               });
		}
	}

	template <bool clear, typename Func>
	void ForEachDownloadRange(uint64_t vaddr, uint64_t size, Func&& func) {
		ForEachDownloadRange<clear>(
		    vaddr, size, [](uint64_t, uint64_t) noexcept {}, std::forward<Func>(func));
	}

	// Every CPU-modified range in [vaddr, vaddr + size), without clearing the dirty state or
	// touching page protection. The frame capture records the set; the game path never reads it.
	template <typename Func>
	void ForEachCpuModifiedRange(uint64_t vaddr, uint64_t size, Func&& func) {
		static_assert(std::is_nothrow_invocable_v<Func&, uint64_t, uint64_t>);
		CheckNotInUploadCallback();
		Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
			std::scoped_lock lock(manager->lock);
			manager->template ForEachModifiedRange<DirtySource::Cpu, false>(
			    manager->GetCpuAddr() + offset, bytes, func);
		});
	}

	template <typename RangeFunc, typename UploadFunc>
	void ForEachUploadRange(uint64_t vaddr, uint64_t size, bool is_written, RangeFunc&& range_func,
	                        UploadFunc&& upload_func) {
		static_assert(std::is_nothrow_invocable_v<RangeFunc&, uint64_t, uint64_t>);
		static_assert(std::is_nothrow_invocable_v<UploadFunc&>);
		CheckNotInUploadCallback();
		Iterate<true>(vaddr, size, [](RegionManager*, uint64_t, uint64_t) {});
		const auto* previous_upload_owner = std::exchange(s_upload_owner, this);
		// Design P (docs/bda-sync-design.md): a read upload leaves every page it takes writable
		// and hands it to the helper thread, so that loop makes no NtProtectVirtualMemory call.
		// A written range is claimed by the GPU right after, which protects the same pages at
		// no-access anyway, so deferring it would buy nothing and would leave the guest free to
		// write bytes the GPU is about to own. The two loops are written out separately so the
		// setting off runs the same instructions it ran before the design existed: this is the
		// path every bind of every draw takes.
		ProtectStats stats;
		uint64_t     landed_pages = 0;
		const bool   defer        = m_async_protect && !is_written;
		if (!m_async_protect) [[likely]] {
			Iterate<false>(vaddr, size,
			               [&](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               {
					               // Step 0b of docs/bda-sync-design.md: what a BDA scan waits for
					               // on the region lock, which the guest's fault handler holds
					               // while it re-marks pages.
					               KYTY_PROFILER_BLOCK("MemoryTracker::Lock");
					               manager->lock.lock();
				               }
				               manager->ForEachModifiedRange<DirtySource::Cpu, true>(
				                   manager->GetCpuAddr() + offset, bytes, range_func);
				               if (!is_written) {
					               manager->lock.unlock();
				               }
			               });
		} else {
			auto& queued = QueuedRegions();
			queued.clear();
			Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t offset,
			                                uint64_t bytes) {
				{
					KYTY_PROFILER_BLOCK("MemoryTracker::Lock");
					manager->lock.lock();
				}
				bool queue_protect = false;
				manager->ForEachModifiedRange<DirtySource::Cpu, true>(
				    manager->GetCpuAddr() + offset, bytes, range_func, defer, &stats,
				    &landed_pages, &queue_protect);
				if (queue_protect && !manager->queued.exchange(true, std::memory_order_acq_rel)) {
					// Announced with the region lock still held, so a drain cannot see an empty
					// queue while a page of this region is still writable.
					AnnounceAsyncProtect();
					queued.push_back(manager);
				}
				if (!is_written) {
					manager->lock.unlock();
				}
			});
		}
		upload_func();
		if (is_written) {
			Iterate<false>(vaddr, size,
			               [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
				               manager->template ChangeState<DirtySource::Gpu, true>(
				                   manager->GetCpuAddr() + offset, bytes);
				               manager->lock.unlock();
			               });
		}
		s_upload_owner = previous_upload_owner;
		if (m_async_protect) {
			RecordUploadProtect(stats, landed_pages, defer);
			auto& queued = QueuedRegions();
			if (!queued.empty()) {
				QueueAsyncProtect(queued.data(), queued.size());
				queued.clear();
			}
		}
	}

	// Design P. Enabling it creates the helper thread; it is never turned off again while the
	// tracker lives, and every region created afterwards carries the deferred-protection states.
	void EnableAsyncProtect(Common::ThreadAffinityGroup group, AsyncProtectSink sink);
	void StopAsyncProtect() noexcept;
	// Waits until the helper's queue is empty and its last batch has landed. Called at every
	// submission boundary; a no-op and one relaxed load with the setting off.
	void               DrainAsyncProtect();
	[[nodiscard]] bool AsyncProtectEnabled() const noexcept { return m_async_protect; }

private:
	static std::vector<RegionManager*>& QueuedRegions() {
		static thread_local std::vector<RegionManager*> regions;
		return regions;
	}

	void AnnounceAsyncProtect() noexcept;
	void QueueAsyncProtect(RegionManager* const* regions, size_t count);
	static void RecordUploadProtect(const ProtectStats& stats, uint64_t landed_pages,
	                                bool deferred) noexcept;

	static constexpr size_t REGION_COUNT = TRACKER_ADDRESS_SIZE / TRACKER_REGION_SIZE;
	inline static thread_local const MemoryTracker* s_upload_owner = nullptr;

	void CheckNotInUploadCallback() const noexcept {
		if (s_upload_owner == this) {
			EXIT("memory tracker re-entered from upload callback\n");
		}
	}

	template <bool create, typename Func>
	bool Iterate(uint64_t vaddr, uint64_t size, Func&& func) {
		ValidateRange(vaddr, size);
		using Result = std::invoke_result_t<Func, RegionManager*, uint64_t, uint64_t>;
		constexpr bool returns_bool = std::is_same_v<Result, bool>;
		uint64_t       remaining    = size;
		uint64_t       index        = vaddr / TRACKER_REGION_SIZE;
		uint64_t       offset       = vaddr % TRACKER_REGION_SIZE;
		while (remaining != 0) {
			const auto bytes   = std::min(TRACKER_REGION_SIZE - offset, remaining);
			auto*      manager = m_regions[index].load(std::memory_order_acquire);
			if (manager == nullptr && create) {
				manager = GetOrCreateRegion(index);
			}
			if (manager != nullptr) {
				if constexpr (returns_bool) {
					if (func(manager, offset, bytes)) {
						return true;
					}
				} else {
					func(manager, offset, bytes);
				}
			}
			remaining -= bytes;
			offset = 0;
			index++;
		}
		return false;
	}

	static void    ValidateRange(uint64_t vaddr, uint64_t size);
	RegionManager* GetOrCreateRegion(uint64_t index);

	std::unique_ptr<std::atomic<RegionManager*>[]> m_regions;
	std::vector<std::unique_ptr<RegionManager>>    m_region_storage;
	std::mutex                                     m_region_mutex;
	PageManager&                                   m_page_manager;
	std::unique_ptr<AsyncProtectHelper>            m_async;
	bool                                           m_async_protect = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYTRACKER_H_
