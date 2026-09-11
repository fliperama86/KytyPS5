#ifndef KYTY_COMMON_GUEST_PAGE_WATCH_H_
#define KYTY_COMMON_GUEST_PAGE_WATCH_H_

#include <cstdint>

// Per-4 KiB-page write generations for the guest virtual address space.
//
// A consumer that wants to remember something it derived from guest memory arms the pages it read
// and records their generations. Every host path that can change those bytes behind the reader's
// back bumps the generation, so a later comparison tells the reader whether its copy is still
// valid. Arming write-protects the page through the graphics page manager, so a plain guest store
// also faults into Invalidate.
//
// The tables are sparse: one block per 4 MiB region, created only when a page in it is armed, so
// nothing is paid for the parts of the 1 TiB guest range the consumer never reads.
//
// This header is deliberately free of every other Kyty dependency: SrtWalker.cpp is compiled into
// standalone test binaries that link nothing else.
namespace Common::GuestPageWatch {

inline constexpr uint64_t PAGE_SHIFT = 12;
inline constexpr uint64_t PAGE_SIZE  = uint64_t {1} << PAGE_SHIFT;
// Matches TRACKER_ADDRESS_SIZE in regionDefinitions.h and GPU_ADDRESS_LIMIT in memory.cpp: every
// guest range the memory tracker accepts lies below it.
inline constexpr uint64_t ADDRESS_LIMIT = uint64_t {1} << 40u;
inline constexpr uint64_t PAGE_COUNT    = ADDRESS_LIMIT >> PAGE_SHIFT;

// Installed by the graphics layer. CanWatch answers whether a page may be watched at all; it is
// asked outside every lock this module holds, so it is free to take the graphics locks. Protect
// write-protects (arm) or restores write access (disarm) to one page and must take no lock that
// any guest-memory notification path already holds.
using PageQuery   = bool (*)(uint64_t page_address);
using ProtectPage = bool (*)(uint64_t page_address, bool arm);

// Installing a backend also clears any watch the previous backend left behind, so the page
// manager never outlives the watches taken against it. Pass nullptr to detach.
void InstallProtect(PageQuery can_watch, ProtectPage protect);

void SetEnabled(bool enabled);
bool Enabled() noexcept;
// Enabled and backed by a page manager that can actually protect a page. Nothing may be cached
// against a generation otherwise, because a write would go unnoticed.
bool Active() noexcept;
void SetReporting(bool reporting);
bool Reporting() noexcept;

struct Armed {
	uint32_t generation = 0;
	bool     armed      = false;
};

// Write-protects the page if it is not watched already and returns its current generation.
Armed Arm(uint64_t page) noexcept;
bool  IsArmed(uint64_t page) noexcept;

// Bumps the generation of every armed page the range touches and drops their watch. Pages that
// were never armed are skipped without touching memory, so a large range costs one load per 64
// pages. Call this from every host path that writes guest memory without faulting, and from every
// path that drops a page's protection.
void Invalidate(uint64_t address, uint64_t size) noexcept;

// Invalidate for one faulting address. Returns true when the page was watched, which is exactly
// the case where the watch, and not the graphics caches, is what made the store fault.
bool InvalidateOnFault(uint64_t address) noexcept;

// Drops every watch. The graphics page manager fatals if it is destroyed with live page state.
void DisarmAll() noexcept;

uint32_t Generation(uint64_t page) noexcept;

// Advanced once per presented frame; cache entries are stamped with it so a reset is free.
void     MarkFrame() noexcept;
uint32_t FrameEpoch() noexcept;

// Counters, reported once per second while Reporting(). Kept off the hot path by the caller,
// which only counts while Reporting(). All monotonic; the report prints deltas.
void CountHit() noexcept;
void CountMiss() noexcept;
void CountInsert() noexcept;
void CountHotSkip() noexcept;

struct Counters {
	uint64_t hits          = 0;
	uint64_t misses        = 0;
	uint64_t inserts       = 0;
	uint64_t hot_skips     = 0;
	uint64_t invalidations = 0;
	uint64_t faults        = 0;
	uint64_t armed_pages   = 0;
};

Counters Snapshot() noexcept;

} // namespace Common::GuestPageWatch

#endif /* KYTY_COMMON_GUEST_PAGE_WATCH_H_ */
