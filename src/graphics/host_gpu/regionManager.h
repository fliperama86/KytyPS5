#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_

#include "common/assert.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace Libs::Graphics {

class TrackingSpinLock final {
public:
	void lock() noexcept {
		const auto thread = CurrentThread();
		if (m_owner.load(std::memory_order_relaxed) == thread) {
			EXIT("recursive region tracking lock\n");
		}
		while (m_lock.test_and_set(std::memory_order_acquire)) {
			if (m_owner.load(std::memory_order_relaxed) == thread) {
				EXIT("recursive region tracking lock while contended\n");
			}
			std::atomic_signal_fence(std::memory_order_seq_cst);
		}
		m_owner.store(thread, std::memory_order_relaxed);
	}
	void unlock() noexcept {
		if (m_owner.load(std::memory_order_relaxed) != CurrentThread()) {
			EXIT("region tracking lock released by non-owner\n");
		}
		m_owner.store(0, std::memory_order_relaxed);
		m_lock.clear(std::memory_order_release);
	}

private:
	static uint32_t CurrentThread() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return GetCurrentThreadId();
#elif defined(__APPLE__)
		// mach thread port is a nonzero per-thread id (0 is the "no owner" sentinel).
		return static_cast<uint32_t>(pthread_mach_thread_np(pthread_self()));
#elif defined(__linux__)
		static thread_local const uint32_t tid = static_cast<uint32_t>(::syscall(SYS_gettid));
		return tid;
#else
		EXIT("region tracking thread identity is unsupported on this platform\n");
#endif
	}

	std::atomic_flag     m_lock = ATOMIC_FLAG_INIT;
	std::atomic_uint32_t m_owner {0};
};

static_assert(std::atomic_uint32_t::is_always_lock_free);

// Design P (docs/bda-sync-design.md): with asynchronous re-protection on, a region carries two
// extra page sets beside the CPU- and GPU-dirty ones.
//
//   pending (state U, "uploaded, unprotected"): the scan uploaded the page and cleared its
//       CPU-dirty bit but left it writable, so the render thread made no NtProtectVirtualMemory
//       call. A helper thread protects it later.
//   landed: the helper has protected the page. The page may have been written between the upload
//       and the protection taking effect without faulting, so the next upload that covers it
//       uploads it once more and it becomes P (protected, clean).
//
// The invariant the whole scheme rests on is m_writable == m_cpu_dirty | pending: the
// protection applied to a page says "writable if the guest may write it without faulting", and a
// U page may. UpdateCpuProtection therefore drives protection from that union, and every state
// change keeps the sets disjoint where it matters: pending & cpu_dirty, landed & cpu_dirty and
// pending & landed are all empty.
class RegionManager final {
public:
	// The design-P page sets live behind a pointer so a region keeps its size and its layout when
	// the setting is off, which is the configuration every other game runs in.
	struct AsyncState {
		RegionBits pending;
		RegionBits landed;
	};

	RegionManager(PageManager& page_manager, uint64_t cpu_addr, bool async_protect = false)
	    : m_page_manager(page_manager), m_cpu_addr(cpu_addr),
	      m_async(async_protect ? std::make_unique<AsyncState>() : nullptr) {
		if (m_cpu_addr % TRACKER_REGION_SIZE != 0) {
			EXIT("invalid region tracking manager construction\n");
		}
		m_cpu_dirty.Fill();
		m_writable.Fill();
		m_readable.Fill();
	}

	KYTY_CLASS_NO_COPY(RegionManager);

	[[nodiscard]] uint64_t GetCpuAddr() const { return m_cpu_addr; }
	template <DirtySource source>
	[[nodiscard]] bool IsModified(uint64_t offset, uint64_t size) const {
		const auto [start, end] = GetPageRange(m_cpu_addr + offset, size);
		const auto& bits        = GetBits<source>();
		return RegionBits(bits, start, end).Any();
	}

	template <DirtySource source, bool enable>
	void ChangeState(uint64_t vaddr, uint64_t size) {
		const auto [start, end] = GetPageRange(vaddr, size);
		if constexpr (source == DirtySource::Cpu && enable) {
			if (RegionBits(m_gpu_dirty, start, end).Any()) {
				EXIT("CPU dirty state conflicts with GPU dirty state\n");
			}
		}
		if constexpr (source == DirtySource::Gpu && enable) {
			if (RegionBits(m_cpu_dirty, start, end).Any()) {
				EXIT("GPU dirty state conflicts with CPU dirty state\n");
			}
		}
		auto& bits = GetBits<source>();
		if constexpr (enable) {
			bits.SetRange(start, end);
		} else {
			bits.UnsetRange(start, end);
		}
		if constexpr (source == DirtySource::Cpu) {
			if (m_async != nullptr) {
				// Whichever way the CPU-dirty bit moves, the page leaves the asynchronous states:
				// a page marked dirty is already writable and needs no protection change and no
				// second upload, and a page cleared through this path is protected right here.
				m_async->pending.UnsetRange(start, end);
				m_async->landed.UnsetRange(start, end);
			}
			UpdateCpuProtection<!enable>();
		} else {
			if (m_async != nullptr && enable) {
				// The GPU owns these pages and will write them back; nothing may upload guest
				// memory over them, so drop the pending second upload. The pending-protect bits
				// stay: the helper still owes the page its write watcher, and applying it while
				// the GPU access watcher holds the page at no-access costs no kernel call.
				m_async->landed.UnsetRange(start, end);
			}
			UpdateGpuProtection<enable>();
		}
	}

	// Design P adds four out-of-band parameters, all inert with the setting off. `defer_protect`
	// is passed only by the CPU upload path: with it the pages this call clears stay writable and
	// go to the helper's queue instead of being re-protected here, so the caller makes no kernel
	// call at all. `stats` counts the calls it does make, `landed_pages` the pages re-uploaded
	// because the helper protected them since, and `queue_protect` says the region now owes the
	// helper work.
	template <DirtySource source, bool clear, typename Func>
	void ForEachModifiedRange(uint64_t vaddr, uint64_t size, Func&& func,
	                          bool defer_protect = false, ProtectStats* stats = nullptr,
	                          uint64_t* landed_pages = nullptr, bool* queue_protect = nullptr) {
		const auto [start, end] = GetPageRange(vaddr, size);
		RegionBits mask(GetBits<source>(), start, end);
		if constexpr (clear) {
			GetBits<source>().UnsetRange(start, end);
		}
		if constexpr (source == DirtySource::Cpu && clear) {
			if (m_async != nullptr) {
				if (defer_protect && mask.Any()) {
					// State U: the bits move from CPU-dirty to pending, so the union that drives
					// protection does not change and UpdateCpuProtection makes no kernel call.
					m_async->pending |= mask;
					if (queue_protect != nullptr) {
						*queue_protect = true;
					}
				}
				RegionBits landed(m_async->landed, start, end);
				if (landed.Any()) {
					m_async->landed.UnsetRange(start, end);
					if (landed_pages != nullptr) {
						*landed_pages += landed.Count();
					}
					mask |= landed;
				}
			}
			UpdateCpuProtection<true>(stats);
			ForEachRange(mask, std::forward<Func>(func));
			return;
		}
		if constexpr (source == DirtySource::Gpu && clear) {
			UpdateGpuProtection<false>();
		}
		ForEachRange(mask, std::forward<Func>(func));
	}

	// The helper thread's half of design P, called with `lock` held. Protects every pending page
	// of the region in as few kernel calls as the page manager coalesces them into -- a run may
	// span already-protected pages, whose protection it re-applies unchanged -- moves them to the
	// landed set unless the GPU has claimed them meanwhile, and reports the landed runs so the
	// caller can put them back in the BDA dirty set. Returns the number of landed pages.
	template <typename Func>
	uint64_t LandPendingProtect(ProtectStats* stats, Func&& func) {
		if (m_async == nullptr || m_async->pending.None()) {
			return 0;
		}
		auto landed = m_async->pending;
		m_async->pending.Clear();
		// A page the GPU has claimed since its upload is downloaded, not uploaded: leaving it out
		// of the landed set keeps the tracker's rule that nothing overwrites GPU-owned bytes.
		landed.AndNot(m_gpu_dirty);
		m_async->landed |= landed;
		UpdateCpuProtection<true>(stats);
		ForEachRange(landed, std::forward<Func>(func));
		return landed.Count();
	}

	// Whether this region is already on the helper's queue. The producer sets it and the helper
	// clears it, both with `lock` held, so a region is queued at most once per batch.
	std::atomic_bool queued {false};

	TrackingSpinLock lock;

private:
	template <bool track>
	void UpdateCpuProtection(ProtectStats* stats = nullptr) {
		if (m_async == nullptr) [[likely]] {
			auto mask  = m_cpu_dirty ^ m_writable;
			m_writable = m_cpu_dirty;
			if (mask.None()) {
				return;
			}
			m_page_manager.UpdatePageWatchersForRegion<track>(m_cpu_addr, mask, stats);
			return;
		}
		// Design P: a page stays writable when the guest may write it without faulting, which
		// means CPU-dirty or not protected yet (state U). Kept apart from the branch above so the
		// setting off costs no extra copy of the region's bit sets on a path a bind runs.
		auto desired = m_cpu_dirty;
		desired |= m_async->pending;
		auto mask  = desired ^ m_writable;
		m_writable = desired;
		if (mask.None()) {
			return;
		}
		m_page_manager.UpdatePageWatchersForRegion<track>(m_cpu_addr, mask, stats);
	}

	template <bool track>
	void UpdateGpuProtection() {
		auto readable = ~m_gpu_dirty;
		auto mask     = readable ^ m_readable;
		m_readable    = readable;
		if (mask.None()) {
			return;
		}
		if constexpr (track) {
			m_page_manager.UpdatePageWatchersForRegion<true, true>(m_cpu_addr, mask);
		} else {
			m_page_manager.UpdatePageWatchersForRegion<false, true>(m_cpu_addr, mask);
		}
	}

	template <DirtySource source>
	RegionBits& GetBits() {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	template <DirtySource source>
	const RegionBits& GetBits() const {
		if constexpr (source == DirtySource::Cpu) {
			return m_cpu_dirty;
		} else {
			return m_gpu_dirty;
		}
	}

	[[nodiscard]] std::pair<size_t, size_t> GetPageRange(uint64_t vaddr, uint64_t size) const {
		if (size == 0 || vaddr < m_cpu_addr || vaddr >= m_cpu_addr + TRACKER_REGION_SIZE ||
		    size > m_cpu_addr + TRACKER_REGION_SIZE - vaddr) {
			EXIT("range lies outside its tracking region\n");
		}
		const auto offset = vaddr - m_cpu_addr;
		return {static_cast<size_t>(offset / TRACKER_PAGE_SIZE),
		        static_cast<size_t>((offset + size + TRACKER_PAGE_SIZE - 1) / TRACKER_PAGE_SIZE)};
	}

	template <typename Func>
	void ForEachRange(const RegionBits& bits, Func&& func) const {
		for (const auto [start, end]: bits) {
			func(m_cpu_addr + start * TRACKER_PAGE_SIZE, (end - start) * TRACKER_PAGE_SIZE);
		}
	}

	PageManager& m_page_manager;
	uint64_t     m_cpu_addr = 0;
	RegionBits   m_cpu_dirty;
	RegionBits   m_gpu_dirty;
	RegionBits   m_writable;
	RegionBits   m_readable;
	// Design P: null when the setting is off, which is the only thing the off path tests.
	std::unique_ptr<AsyncState> m_async;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_REGIONMANAGER_H_
