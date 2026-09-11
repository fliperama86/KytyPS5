#include "common/guestPageWatch.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace Common::GuestPageWatch {

namespace {

constexpr uint64_t REGION_SHIFT = 22; // 4 MiB, the granularity the memory tracker already uses
constexpr uint64_t REGION_SIZE  = uint64_t {1} << REGION_SHIFT;
constexpr uint64_t REGION_PAGES = REGION_SIZE >> PAGE_SHIFT;
constexpr uint64_t REGION_WORDS = REGION_PAGES / 64u;
constexpr uint64_t REGION_COUNT = ADDRESS_LIMIT >> REGION_SHIFT;

struct Block {
	// One generation per page plus one armed bit per page. Both live in the same block, so a
	// region nothing ever armed costs one null pointer.
	std::atomic<uint32_t> generation[REGION_PAGES] {};
	std::atomic<uint64_t> armed[REGION_WORDS] {};
};

struct Table {
	std::unique_ptr<std::atomic<Block*>[]> blocks =
	    std::make_unique<std::atomic<Block*>[]>(REGION_COUNT);
	std::vector<std::unique_ptr<Block>> storage;
	std::mutex                          create_mutex;
	// Serializes the armed bit and the page protection so they always move together. Arm's fast
	// path, which only reads a bit that is already set, does not take it.
	std::mutex watch_mutex;
};

Table& GetTable() {
	static Table table;
	return table;
}

std::atomic<PageQuery>   g_can_watch {nullptr};
std::atomic<ProtectPage> g_protect {nullptr};
std::atomic<bool>        g_enabled {true};
std::atomic<bool>        g_reporting {false};
std::atomic<uint32_t>    g_frame_epoch {1};

std::atomic<uint64_t> g_hits {0};
std::atomic<uint64_t> g_misses {0};
std::atomic<uint64_t> g_inserts {0};
std::atomic<uint64_t> g_hot_skips {0};
std::atomic<uint64_t> g_invalidations {0};
std::atomic<uint64_t> g_faults {0};
std::atomic<uint64_t> g_armed_pages {0};
std::atomic<uint64_t> g_frames {0};

Block* FindBlock(uint64_t page) noexcept {
	const auto region = page >> (REGION_SHIFT - PAGE_SHIFT);
	if (region >= REGION_COUNT) {
		return nullptr;
	}
	return GetTable().blocks[region].load(std::memory_order_acquire);
}

Block* GetOrCreateBlock(uint64_t page) {
	const auto region = page >> (REGION_SHIFT - PAGE_SHIFT);
	if (region >= REGION_COUNT) {
		return nullptr;
	}
	auto& table = GetTable();
	if (auto* existing = table.blocks[region].load(std::memory_order_acquire);
	    existing != nullptr) {
		return existing;
	}
	std::lock_guard lock(table.create_mutex);
	if (auto* existing = table.blocks[region].load(std::memory_order_relaxed); existing != nullptr) {
		return existing;
	}
	table.storage.push_back(std::make_unique<Block>());
	auto* created = table.storage.back().get();
	table.blocks[region].store(created, std::memory_order_release);
	return created;
}

// Clears the armed bit, bumps the generation and restores write access, in that order: a reader
// that still sees the bit set has not yet seen the bump, and one that sees the old generation has
// not lost the protection. The caller holds watch_mutex.
void DropWatchLocked(Block& block, uint64_t page, uint64_t index) noexcept {
	const auto word = index / 64u;
	const auto bit  = uint64_t {1} << (index % 64u);
	if ((block.armed[word].fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0) {
		return;
	}
	block.generation[index].fetch_add(1, std::memory_order_release);
	g_armed_pages.fetch_sub(1, std::memory_order_relaxed);
	g_invalidations.fetch_add(1, std::memory_order_relaxed);
	if (auto* protect = g_protect.load(std::memory_order_acquire); protect != nullptr) {
		(void)protect(page << PAGE_SHIFT, false);
	}
}

bool DropWatch(uint64_t page) noexcept {
	auto* block = FindBlock(page);
	if (block == nullptr) {
		return false;
	}
	const auto index = page % REGION_PAGES;
	const auto word  = index / 64u;
	const auto bit   = uint64_t {1} << (index % 64u);
	if ((block->armed[word].load(std::memory_order_acquire) & bit) == 0) {
		return false;
	}
	std::lock_guard lock(GetTable().watch_mutex);
	const bool watched = (block->armed[word].load(std::memory_order_relaxed) & bit) != 0;
	DropWatchLocked(*block, page, index);
	return watched;
}

void Report() {
	using Clock = std::chrono::steady_clock;
	static Clock::time_point last        = Clock::now();
	static Counters          taken       = {};
	static uint64_t          last_frames = 0;
	const auto               now         = Clock::now();
	if (now - last < std::chrono::seconds {1}) {
		return;
	}
	last               = now;
	const auto total   = Snapshot();
	const auto frames  = g_frames.load(std::memory_order_relaxed);
	std::printf("srt-cache: frames=%llu hits=%llu misses=%llu inserts=%llu hot=%llu "
	            "invalidations=%llu watch-faults=%llu armed=%llu\n",
	            static_cast<unsigned long long>(frames - last_frames),
	            static_cast<unsigned long long>(total.hits - taken.hits),
	            static_cast<unsigned long long>(total.misses - taken.misses),
	            static_cast<unsigned long long>(total.inserts - taken.inserts),
	            static_cast<unsigned long long>(total.hot_skips - taken.hot_skips),
	            static_cast<unsigned long long>(total.invalidations - taken.invalidations),
	            static_cast<unsigned long long>(total.faults - taken.faults),
	            static_cast<unsigned long long>(total.armed_pages));
	std::fflush(stdout);
	taken       = total;
	last_frames = frames;
}

} // namespace

void InstallProtect(PageQuery can_watch, ProtectPage protect) {
	if (protect == nullptr) {
		DisarmAll();
		g_protect.store(nullptr, std::memory_order_release);
		g_can_watch.store(nullptr, std::memory_order_release);
		return;
	}
	g_can_watch.store(can_watch, std::memory_order_release);
	g_protect.store(protect, std::memory_order_release);
}

void SetEnabled(bool enabled) {
	g_enabled.store(enabled, std::memory_order_relaxed);
	if (!enabled) {
		DisarmAll();
	}
}

bool Enabled() noexcept {
	return g_enabled.load(std::memory_order_relaxed);
}

bool Active() noexcept {
	return Enabled() && g_protect.load(std::memory_order_acquire) != nullptr;
}

void SetReporting(bool reporting) {
	g_reporting.store(reporting, std::memory_order_relaxed);
}

bool Reporting() noexcept {
	return g_reporting.load(std::memory_order_relaxed);
}

Armed Arm(uint64_t page) noexcept {
	if (page >= PAGE_COUNT || !Enabled() || g_protect.load(std::memory_order_acquire) == nullptr) {
		return {};
	}
	const auto index = page % REGION_PAGES;
	const auto word  = index / 64u;
	const auto bit   = uint64_t {1} << (index % 64u);
	auto*      block = FindBlock(page);
	if (block != nullptr && (block->armed[word].load(std::memory_order_acquire) & bit) != 0) {
		// Already watched. An Invalidate racing with this sample bumps the generation before it
		// drops the protection, so a stale sample is detected by the caller's second look.
		return {block->generation[index].load(std::memory_order_acquire), true};
	}
	// Asked before the lock: the answer needs the graphics mapping lock, and nothing may take
	// that while holding watch_mutex.
	if (auto* can_watch = g_can_watch.load(std::memory_order_acquire);
	    can_watch != nullptr && !can_watch(page << PAGE_SHIFT)) {
		return {};
	}
	block = GetOrCreateBlock(page);
	if (block == nullptr) {
		return {};
	}
	std::lock_guard lock(GetTable().watch_mutex);
	if ((block->armed[word].load(std::memory_order_relaxed) & bit) == 0) {
		auto* protect = g_protect.load(std::memory_order_acquire);
		if (protect == nullptr || !protect(page << PAGE_SHIFT, true)) {
			return {};
		}
		block->armed[word].fetch_or(bit, std::memory_order_acq_rel);
		g_armed_pages.fetch_add(1, std::memory_order_relaxed);
	}
	// Sampled after the page is protected: a store that beat the protection also beat the read
	// the caller is about to make, and a store after it faults and bumps.
	return {block->generation[index].load(std::memory_order_acquire), true};
}

bool IsArmed(uint64_t page) noexcept {
	auto* block = FindBlock(page);
	if (block == nullptr) {
		return false;
	}
	const auto index = page % REGION_PAGES;
	return (block->armed[index / 64u].load(std::memory_order_acquire) &
	        (uint64_t {1} << (index % 64u))) != 0;
}

uint32_t Generation(uint64_t page) noexcept {
	auto* block = FindBlock(page);
	if (block == nullptr) {
		return 0;
	}
	return block->generation[page % REGION_PAGES].load(std::memory_order_acquire);
}

void Invalidate(uint64_t address, uint64_t size) noexcept {
	if (size == 0 || address >= ADDRESS_LIMIT) {
		return;
	}
	const auto first = address >> PAGE_SHIFT;
	const auto last  = (address + std::min(size, ADDRESS_LIMIT - address) - 1) >> PAGE_SHIFT;
	for (auto page = first; page <= last;) {
		const auto index       = page % REGION_PAGES;
		const auto region_base = page - index;
		const auto chunk_last  = std::min(last, region_base + REGION_PAGES - 1);
		auto*      block       = FindBlock(page);
		if (block == nullptr) {
			page = chunk_last + 1;
			continue;
		}
		const auto last_index = chunk_last - region_base;
		for (auto word = index / 64u; word <= last_index / 64u; word++) {
			auto bits = block->armed[word].load(std::memory_order_acquire);
			// Keep only the bits this range covers, then walk the ones that are set.
			const auto word_first = word * 64u;
			if (index > word_first) {
				bits &= ~uint64_t {0} << (index - word_first);
			}
			if (last_index < word_first + 63u) {
				bits &= ~uint64_t {0} >> (63u - (last_index - word_first));
			}
			while (bits != 0) {
				const auto offset = static_cast<uint64_t>(std::countr_zero(bits));
				bits &= bits - 1;
				(void)DropWatch(region_base + word_first + offset);
			}
		}
		page = chunk_last + 1;
	}
}

bool InvalidateOnFault(uint64_t address) noexcept {
	if (address >= ADDRESS_LIMIT) {
		return false;
	}
	const bool watched = DropWatch(address >> PAGE_SHIFT);
	if (watched) {
		g_faults.fetch_add(1, std::memory_order_relaxed);
	}
	return watched;
}

void DisarmAll() noexcept {
	auto& table = GetTable();
	for (uint64_t region = 0; region < REGION_COUNT; region++) {
		auto* block = table.blocks[region].load(std::memory_order_acquire);
		if (block == nullptr) {
			continue;
		}
		for (uint64_t word = 0; word < REGION_WORDS; word++) {
			auto bits = block->armed[word].load(std::memory_order_acquire);
			while (bits != 0) {
				const auto offset = static_cast<uint64_t>(std::countr_zero(bits));
				bits &= bits - 1;
				(void)DropWatch(region * REGION_PAGES + word * 64u + offset);
			}
		}
	}
}

void MarkFrame() noexcept {
	g_frame_epoch.fetch_add(1, std::memory_order_release);
	g_frames.fetch_add(1, std::memory_order_relaxed);
	if (Reporting()) {
		Report();
	}
}

uint32_t FrameEpoch() noexcept {
	return g_frame_epoch.load(std::memory_order_acquire);
}

void CountHit() noexcept {
	g_hits.fetch_add(1, std::memory_order_relaxed);
}

void CountMiss() noexcept {
	g_misses.fetch_add(1, std::memory_order_relaxed);
}

void CountInsert() noexcept {
	g_inserts.fetch_add(1, std::memory_order_relaxed);
}

void CountHotSkip() noexcept {
	g_hot_skips.fetch_add(1, std::memory_order_relaxed);
}

Counters Snapshot() noexcept {
	return {.hits          = g_hits.load(std::memory_order_relaxed),
	        .misses        = g_misses.load(std::memory_order_relaxed),
	        .inserts       = g_inserts.load(std::memory_order_relaxed),
	        .hot_skips     = g_hot_skips.load(std::memory_order_relaxed),
	        .invalidations = g_invalidations.load(std::memory_order_relaxed),
	        .faults        = g_faults.load(std::memory_order_relaxed),
	        .armed_pages   = g_armed_pages.load(std::memory_order_relaxed)};
}

} // namespace Common::GuestPageWatch
