#include "graphics/host_gpu/memoryTracker.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "common/threads.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>

namespace Libs::Graphics {

static_assert(std::atomic<void*>::is_always_lock_free);

namespace {

// Design P's counters (docs/bda-sync-design.md). Relaxed adds on paths that already take a lock.
std::atomic_uint64_t g_upload_protect_calls {0};
std::atomic_uint64_t g_upload_protect_pages {0};
std::atomic_uint64_t g_written_protect_calls {0};
std::atomic_uint64_t g_written_protect_pages {0};
std::atomic_uint64_t g_helper_protect_calls {0};
std::atomic_uint64_t g_helper_protect_pages {0};
std::atomic_uint64_t g_helper_batches {0};
std::atomic_uint64_t g_landed_pages {0};
std::atomic_uint64_t g_extra_uploads {0};
std::atomic_uint64_t g_drains {0};
std::atomic_uint64_t g_waits {0};
std::atomic_uint64_t g_wait_ns {0};
std::atomic_uint64_t g_boundary_scans {0};
std::atomic_uint64_t g_boundary_scan_ns {0};

} // namespace

AsyncProtectCounters ReadAsyncProtectCounters() noexcept {
	AsyncProtectCounters counters;
	counters.upload_protect_calls = g_upload_protect_calls.load(std::memory_order_relaxed);
	counters.upload_protect_pages = g_upload_protect_pages.load(std::memory_order_relaxed);
	counters.written_protect_calls = g_written_protect_calls.load(std::memory_order_relaxed);
	counters.written_protect_pages = g_written_protect_pages.load(std::memory_order_relaxed);
	counters.helper_protect_calls = g_helper_protect_calls.load(std::memory_order_relaxed);
	counters.helper_protect_pages = g_helper_protect_pages.load(std::memory_order_relaxed);
	counters.helper_batches       = g_helper_batches.load(std::memory_order_relaxed);
	counters.landed_pages         = g_landed_pages.load(std::memory_order_relaxed);
	counters.extra_uploads        = g_extra_uploads.load(std::memory_order_relaxed);
	counters.drains               = g_drains.load(std::memory_order_relaxed);
	counters.waits                = g_waits.load(std::memory_order_relaxed);
	counters.wait_ns              = g_wait_ns.load(std::memory_order_relaxed);
	counters.boundary_scans       = g_boundary_scans.load(std::memory_order_relaxed);
	counters.boundary_scan_ns     = g_boundary_scan_ns.load(std::memory_order_relaxed);
	return counters;
}

void ResetAsyncProtectCounters() noexcept {
	g_upload_protect_calls.store(0, std::memory_order_relaxed);
	g_upload_protect_pages.store(0, std::memory_order_relaxed);
	g_written_protect_calls.store(0, std::memory_order_relaxed);
	g_written_protect_pages.store(0, std::memory_order_relaxed);
	g_helper_protect_calls.store(0, std::memory_order_relaxed);
	g_helper_protect_pages.store(0, std::memory_order_relaxed);
	g_helper_batches.store(0, std::memory_order_relaxed);
	g_landed_pages.store(0, std::memory_order_relaxed);
	g_extra_uploads.store(0, std::memory_order_relaxed);
	g_drains.store(0, std::memory_order_relaxed);
	g_waits.store(0, std::memory_order_relaxed);
	g_wait_ns.store(0, std::memory_order_relaxed);
	g_boundary_scans.store(0, std::memory_order_relaxed);
	g_boundary_scan_ns.store(0, std::memory_order_relaxed);
}

void RecordAsyncProtectBoundaryScan(uint64_t ns) noexcept {
	g_boundary_scans.fetch_add(1, std::memory_order_relaxed);
	g_boundary_scan_ns.fetch_add(ns, std::memory_order_relaxed);
}

// The helper thread of design P. It owns nothing: the pages it protects live in the regions the
// tracker owns, and the only thing it holds is a queue of the regions that have pending pages.
//
// Announcing and queueing are two steps on purpose. `m_outstanding` is raised with the region's
// own lock held, the moment a page becomes unprotected-and-clean, and only falls when that
// region has been landed; the queue entry follows once the producer has released the lock. A
// drain therefore cannot slip between the two and conclude that there is nothing to wait for.
class AsyncProtectHelper final {
public:
	AsyncProtectHelper(Common::ThreadAffinityGroup group, AsyncProtectSink sink)
	    : m_sink(std::move(sink)), m_group(group) {
		m_thread = std::thread([this] { Run(); });
	}

	~AsyncProtectHelper() { Stop(); }

	KYTY_CLASS_NO_COPY(AsyncProtectHelper);

	void Stop() noexcept {
		{
			std::lock_guard lock(m_mutex);
			if (m_stop) {
				return;
			}
			m_stop = true;
		}
		m_work.notify_all();
		if (m_thread.joinable()) {
			m_thread.join();
		}
	}

	void Announce() noexcept { m_outstanding.fetch_add(1, std::memory_order_release); }

	void Queue(RegionManager* const* regions, size_t count) {
		{
			std::lock_guard lock(m_mutex);
			for (size_t i = 0; i < count; i++) {
				m_queue.push_back(regions[i]);
			}
		}
		m_work.notify_one();
	}

	void Drain() {
		g_drains.fetch_add(1, std::memory_order_relaxed);
		if (m_outstanding.load(std::memory_order_acquire) == 0) {
			std::unique_lock lock(m_mutex);
			if (!m_busy && m_queue.empty() &&
			    m_outstanding.load(std::memory_order_acquire) == 0) {
				return;
			}
		}
		KYTY_PROFILER_BLOCK("AsyncProtect::Drain");
		const auto started = std::chrono::steady_clock::now();
		{
			std::unique_lock lock(m_mutex);
			m_idle.wait(lock, [this] {
				return m_stop || (!m_busy && m_queue.empty() &&
				                  m_outstanding.load(std::memory_order_acquire) == 0);
			});
		}
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
		                         std::chrono::steady_clock::now() - started)
		                         .count();
		g_waits.fetch_add(1, std::memory_order_relaxed);
		g_wait_ns.fetch_add(static_cast<uint64_t>(elapsed), std::memory_order_relaxed);
	}

private:
	void Run() {
		Common::ApplyThreadAffinity(m_group, "Thread_BdaProtect");
		KYTY_PROFILER_THREAD("Thread_BdaProtect");
		std::vector<RegionManager*> batch;
		std::vector<GuestRange>     landed;
		for (;;) {
			{
				std::unique_lock lock(m_mutex);
				m_work.wait(lock, [this] { return m_stop || !m_queue.empty(); });
				if (m_queue.empty()) {
					if (m_stop) {
						return;
					}
					continue;
				}
				batch.assign(m_queue.begin(), m_queue.end());
				m_queue.clear();
				m_busy = true;
			}
			landed.clear();
			ProtectStats stats;
			uint64_t     pages = 0;
			for (auto* region: batch) {
				std::scoped_lock region_lock(region->lock);
				region->queued.store(false, std::memory_order_release);
				pages += region->LandPendingProtect(
				    &stats, [&landed](uint64_t address, uint64_t size) noexcept {
					    landed.push_back({address, size});
				    });
			}
			if (!landed.empty()) {
				// One generation bump for the whole batch, and the landed runs go back in the
				// BDA dirty set so the next scan uploads them a second time.
				m_sink(landed.data(), landed.size());
			}
			g_helper_batches.fetch_add(1, std::memory_order_relaxed);
			g_helper_protect_calls.fetch_add(stats.calls, std::memory_order_relaxed);
			g_helper_protect_pages.fetch_add(stats.pages, std::memory_order_relaxed);
			g_landed_pages.fetch_add(pages, std::memory_order_relaxed);
			{
				std::lock_guard lock(m_mutex);
				m_outstanding.fetch_sub(batch.size(), std::memory_order_release);
				m_busy = false;
			}
			m_idle.notify_all();
		}
	}

	AsyncProtectSink                   m_sink;
	Common::ThreadAffinityGroup        m_group;
	std::mutex                         m_mutex;
	std::condition_variable            m_work;
	std::condition_variable            m_idle;
	std::deque<RegionManager*>         m_queue;
	std::atomic_uint64_t               m_outstanding {0};
	bool                               m_busy = false;
	bool                               m_stop = false;
	std::thread                        m_thread;
};

MemoryTracker::MemoryTracker(PageManager& page_manager): m_page_manager(page_manager) {
	m_regions = std::make_unique<std::atomic<RegionManager*>[]>(REGION_COUNT);
	for (size_t i = 0; i < REGION_COUNT; i++) {
		m_regions[i].store(nullptr, std::memory_order_relaxed);
	}
}

MemoryTracker::~MemoryTracker() {
	StopAsyncProtect();
}

void MemoryTracker::EnableAsyncProtect(Common::ThreadAffinityGroup group, AsyncProtectSink sink) {
	if (m_async != nullptr) {
		EXIT("memory tracker asynchronous protection enabled twice\n");
	}
	if (!m_region_storage.empty()) {
		EXIT("memory tracker asynchronous protection enabled after the first region\n");
	}
	m_async         = std::make_unique<AsyncProtectHelper>(group, std::move(sink));
	m_async_protect = true;
}

void MemoryTracker::StopAsyncProtect() noexcept {
	if (m_async == nullptr) {
		return;
	}
	m_async->Stop();
}

void MemoryTracker::DrainAsyncProtect() {
	if (!m_async_protect) {
		return;
	}
	m_async->Drain();
}

void MemoryTracker::AnnounceAsyncProtect() noexcept {
	m_async->Announce();
}

void MemoryTracker::QueueAsyncProtect(RegionManager* const* regions, size_t count) {
	m_async->Queue(regions, count);
}

void MemoryTracker::RecordUploadProtect(const ProtectStats& stats, uint64_t landed_pages,
                                       bool deferred) noexcept {
	if (stats.calls != 0) {
		auto& calls = deferred ? g_upload_protect_calls : g_written_protect_calls;
		auto& pages = deferred ? g_upload_protect_pages : g_written_protect_pages;
		calls.fetch_add(stats.calls, std::memory_order_relaxed);
		pages.fetch_add(stats.pages, std::memory_order_relaxed);
	}
	if (landed_pages != 0) {
		g_extra_uploads.fetch_add(landed_pages, std::memory_order_relaxed);
	}
}

#if KYTY_BUILD == KYTY_BUILD_DEBUG
void MemoryTracker::ValidateGpuDirtyPages(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                                          const char* operation) const noexcept {
	if (!GuestRange {vaddr, size}.Valid() || (vaddr & (TRACKER_PAGE_SIZE - 1)) != 0 ||
	    (size & (TRACKER_PAGE_SIZE - 1)) != 0) {
		EXIT("MemoryTracker: invalid dirty-page validation range\n");
	}
	for (auto page = vaddr; page < vaddr + size; page += TRACKER_PAGE_SIZE) {
		if (!dirty.Intersects(page, TRACKER_PAGE_SIZE)) {
			EXIT("MemoryTracker: GPU-dirty tracker page has no dirty bytes, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}

void MemoryTracker::ValidateGpuDirtyOwnership(const RangeSet& dirty, uint64_t vaddr, uint64_t size,
                                              const char* operation) {
	ValidateRange(vaddr, size);
	const auto begin = vaddr & ~(TRACKER_PAGE_SIZE - 1);
	const auto end   = (vaddr + size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
	for (auto page = begin; page < end; page += TRACKER_PAGE_SIZE) {
		const bool has_dirty_bytes = dirty.Intersects(page, TRACKER_PAGE_SIZE);
		if (IsRegionGpuModified(page, TRACKER_PAGE_SIZE) != has_dirty_bytes) {
			EXIT("MemoryTracker: tracker and byte ownership disagree, operation=%s "
			     "addr=0x%016" PRIx64 "\n",
			     operation, page);
		}
	}
}
#endif

void MemoryTracker::ValidateRange(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("invalid memory tracker range\n");
	}
}

RegionManager* MemoryTracker::GetOrCreateRegion(uint64_t index) {
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	std::lock_guard lock(m_region_mutex);
	if (auto* manager = m_regions[index].load(std::memory_order_acquire); manager != nullptr) {
		return manager;
	}
	auto  manager =
	    std::make_unique<RegionManager>(m_page_manager, index * TRACKER_REGION_SIZE, m_async_protect);
	auto* ptr     = manager.get();
	m_region_storage.push_back(std::move(manager));
	m_regions[index].store(ptr, std::memory_order_release);
	return ptr;
}

bool MemoryTracker::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	return Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		return manager->IsModified<DirtySource::Cpu>(offset, bytes);
	});
}

bool MemoryTracker::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	return Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		return manager->IsModified<DirtySource::Gpu>(offset, bytes);
	});
}

void MemoryTracker::MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
	});
}

void MemoryTracker::MarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	Iterate<true>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Gpu, true>(manager->GetCpuAddr() + offset, bytes);
	});
}

void MemoryTracker::UnmarkRegionAsGpuModified(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		std::scoped_lock lock(manager->lock);
		manager->ChangeState<DirtySource::Gpu, false>(manager->GetCpuAddr() + offset, bytes);
	});
}

void MemoryTracker::UntrackMemory(uint64_t vaddr, uint64_t size) {
	CheckNotInUploadCallback();
	std::vector<RegionManager*> managers;
	managers.reserve((vaddr % TRACKER_REGION_SIZE + size + TRACKER_REGION_SIZE - 1) /
	                 TRACKER_REGION_SIZE);
	Iterate<false>(vaddr, size, [&](RegionManager* manager, uint64_t, uint64_t) {
		managers.push_back(manager);
	});

	std::vector<std::unique_lock<TrackingSpinLock>> locks;
	locks.reserve(managers.size());
	for (auto* manager: managers) {
		locks.emplace_back(manager->lock);
	}
	if (Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		    return manager->IsModified<DirtySource::Gpu>(offset, bytes);
	    })) {
		EXIT("cannot untrack GPU-dirty memory\n");
	}
	Iterate<false>(vaddr, size, [](RegionManager* manager, uint64_t offset, uint64_t bytes) {
		manager->ChangeState<DirtySource::Cpu, true>(manager->GetCpuAddr() + offset, bytes);
	});
}

} // namespace Libs::Graphics
