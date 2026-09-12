#include "graphics/replay/frameReplay.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/readbackDiagnostics.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/replay/frameCapture.h"
#include "graphics/replay/frameCaptureReader.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/agc.h"
#include "loader/runtimeLinker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::Graphics::Replay {

namespace VirtualMemory = Common::VirtualMemory;
namespace Memory        = LibKernel::Memory;

namespace {

using Clock = std::chrono::steady_clock;

// A WAIT_REG_MEM that never completes re-queues its submission forever; the replay gives a
// measured loop this long before it prints the wait and gives up. The warm-up loop compiles
// every pipeline the frame touches, which is minutes on a cold shader cache, so it gets its own
// budget.
constexpr uint32_t WAIT_TIMEOUT_MS   = 2000;
constexpr uint32_t WARMUP_TIMEOUT_MS = 600000;
// The presented flip lands on the next vblank; a few frames of slack is generous.
constexpr uint32_t FLIP_TIMEOUT_MS = 2000;

constexpr uint32_t PROT_CPU_READ  = 0x01;
constexpr uint32_t PROT_CPU_WRITE = 0x02;
constexpr uint32_t PROT_CPU_EXEC  = 0x04;
constexpr uint32_t PROT_GPU_READ  = 0x10;
constexpr uint32_t PROT_GPU_WRITE = 0x20;

// The kernel's VirtualRangeType, which RangeRecord::type carries as a plain number.
constexpr uint32_t RANGE_TYPE_RESERVED      = 0;
constexpr uint32_t RANGE_TYPE_POOL_RESERVED = 1;
constexpr uint32_t RANGE_TYPE_RUNTIME       = 7;

struct BlockedWait {
	uint64_t address   = 0;
	uint64_t value     = 0;
	uint64_t reference = 0;
	uint64_t mask      = 0;
	uint32_t function  = 0;
	uint32_t width     = 0;
	bool     valid     = false;
};

std::atomic_bool g_wait_diagnostics {false};
std::mutex       g_wait_mutex;
BlockedWait      g_last_wait;

double MillisSince(Clock::time_point start) {
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The same decode KernelMapDirectMemory applies to a guest protection word.
VirtualMemory::Mode ModeFromProt(uint32_t prot) {
	const bool host_write = (prot & (PROT_CPU_WRITE | PROT_GPU_WRITE)) != 0;
	const bool host_read  = host_write || (prot & (PROT_CPU_READ | PROT_GPU_READ)) != 0;
	if ((prot & PROT_CPU_EXEC) != 0) {
		return host_write  ? VirtualMemory::Mode::ExecuteReadWrite
		       : host_read ? VirtualMemory::Mode::ExecuteRead
		                   : VirtualMemory::Mode::Execute;
	}
	if (host_write) {
		return VirtualMemory::Mode::ReadWrite;
	}
	return host_read ? VirtualMemory::Mode::Read : VirtualMemory::Mode::NoAccess;
}

bool IsCommittedRangeType(uint32_t type) {
	return type != RANGE_TYPE_RESERVED && type != RANGE_TYPE_POOL_RESERVED &&
	       type <= RANGE_TYPE_RUNTIME;
}

struct RestoredRange {
	uint64_t start = 0;
	uint64_t size  = 0;
	uint32_t prot  = 0;
};

const RestoredRange* FindRange(const std::vector<RestoredRange>& ranges, uint64_t vaddr) {
	auto position = std::upper_bound(
	    ranges.begin(), ranges.end(), vaddr,
	    [](uint64_t value, const RestoredRange& range) { return value < range.start; });
	if (position == ranges.begin()) {
		return nullptr;
	}
	--position;
	if (vaddr >= position->start && vaddr - position->start < position->size) {
		return &*position;
	}
	return nullptr;
}

// Writes one restored page. Direct, flexible and pooled ranges go through the kernel's backing
// store, which ignores the guest protection; a private-committed range is written in place, with
// the host protection lifted for the copy when the guest mapping is not writable.
bool WritePage(const std::vector<RestoredRange>& ranges, uint64_t vaddr, const uint8_t* data) {
	if (Memory::TryWriteBacking(vaddr, data, kPageSize)) {
		return true;
	}
	const auto* range = FindRange(ranges, vaddr);
	if (range == nullptr) {
		return false;
	}
	const auto mode = ModeFromProt(range->prot);
	const bool writable =
	    mode == VirtualMemory::Mode::ReadWrite || mode == VirtualMemory::Mode::ExecuteReadWrite ||
	    mode == VirtualMemory::Mode::Write || mode == VirtualMemory::Mode::ExecuteWrite;
	if (!writable &&
	    !Memory::ProtectGuestHostMemory(vaddr, kPageSize, VirtualMemory::Mode::ReadWrite)) {
		return false;
	}
	std::memcpy(reinterpret_cast<void*>(vaddr), data, kPageSize); // NOLINT
	if (!writable) {
		(void)Memory::ProtectGuestHostMemory(vaddr, kPageSize, mode);
	}
	return true;
}

bool IsHostReadable(uint32_t prot) {
	const auto mode = ModeFromProt(prot);
	return mode != VirtualMemory::Mode::NoAccess && mode != VirtualMemory::Mode::Execute;
}

// Rebuilding the shader map, a capture format v1 workaround.
//
// The render path resolves every shader through ShaderMap, which the game fills from
// sceAgcCreateShader; a replay runs no guest code, so the map is empty and the first draw or
// dispatch exits with "is missing from ShaderMap". Every field of an entry, though, comes from the
// guest AGC shader header, and sceAgcCreateShader rewrote that header in place before the capture:
// its offsets are already absolute pointers and the restore puts them back at the same guest
// addresses. So the replay scans the restored readable ranges for the header signature and
// re-registers what it finds. A capture format v2 should record the map instead of inferring it.
uint64_t RestoreRecordedShaderMap(const std::vector<ShaderRecord>& records) {
	uint64_t registered = 0;
	for (const auto& record: records) {
		if (record.code_address == 0) {
			continue;
		}
		ShaderMappedData map;
		map.type            = static_cast<Prospero::ShaderBinaryType>(record.type);
		map.user_data       = reinterpret_cast<ShaderUserData*>(record.user_data);       // NOLINT
		map.input_semantics = reinterpret_cast<ShaderSemantic*>(record.input_semantics); // NOLINT
		map.num_input_semantics = record.num_input_semantics;
		map.code_size_bytes     = record.code_size_bytes;
		map.scratch_size_dwords = record.scratch_size_dwords;
		ShaderMapUserData(record.code_address, map);
		registered++;
	}
	return registered;
}

uint64_t RestoreShaderMap(const std::vector<RestoredRange>& ranges) {
	// Shader::file_header 0x34333231 ("1234") followed by Shader::version 0x00000018, the pair
	// sceAgcCreateShader insists on.
	constexpr uint64_t SIGNATURE           = 0x0000001834333231ull;
	constexpr uint32_t MAX_CODE_BYTES      = 16u * 1024u * 1024u;
	constexpr uint32_t MAX_INPUT_SEMANTICS = 64;

	auto in_guest_memory = [&ranges](const void* pointer, uint64_t size) {
		const auto address = reinterpret_cast<uint64_t>(pointer);
		if (address == 0) {
			return false;
		}
		const auto* range = FindRange(ranges, address);
		return range != nullptr && size <= range->size - (address - range->start);
	};

	uint64_t registered = 0;
	for (const auto& range: ranges) {
		if (!IsHostReadable(range.prot) || range.size < sizeof(Shader)) {
			continue;
		}
		const auto* words = reinterpret_cast<const uint64_t*>(range.start); // NOLINT
		const auto  last  = (range.size - sizeof(Shader)) / sizeof(uint64_t);
		for (uint64_t index = 0; index <= last; index++) {
			if (words[index] != SIGNATURE) {
				continue;
			}
			const auto* header = reinterpret_cast<const Shader*>(words + index); // NOLINT
			const auto  base   = reinterpret_cast<uint64_t>(header->code);
			if ((base & 0xffff0000000000ffull) != 0 || header->shader_size == 0 ||
			    header->shader_size > MAX_CODE_BYTES ||
			    header->shader_size % sizeof(uint32_t) != 0 ||
			    header->type > static_cast<uint8_t>(Prospero::ShaderBinaryType::kFs) ||
			    header->num_input_semantics > MAX_INPUT_SEMANTICS ||
			    !in_guest_memory(const_cast<void*>(header->code), header->shader_size)) {
				continue;
			}
			if (header->user_data != nullptr &&
			    !in_guest_memory(header->user_data, sizeof(ShaderUserData))) {
				continue;
			}
			if (header->input_semantics != nullptr &&
			    !in_guest_memory(header->input_semantics,
			                     header->num_input_semantics * sizeof(ShaderSemantic))) {
				continue;
			}

			ShaderMappedData map;
			map.type                = static_cast<Prospero::ShaderBinaryType>(header->type);
			map.user_data           = header->user_data;
			map.input_semantics     = header->input_semantics;
			map.num_input_semantics = header->num_input_semantics;
			map.code_size_bytes     = header->shader_size;
			map.scratch_size_dwords = header->scratch_size_dw_per_thread;
			ShaderMapUserData(base, map);
			registered++;
		}
	}
	return registered;
}

struct Stats {
	double min    = 0.0;
	double median = 0.0;
	double max    = 0.0;
	double jitter = 0.0;
	double total  = 0.0;
};

Stats Summarize(const std::vector<double>& samples, size_t skip) {
	Stats stats;
	for (const auto sample: samples) {
		stats.total += sample;
	}
	if (samples.size() <= skip) {
		return stats;
	}
	std::vector<double> measured(samples.begin() + static_cast<std::ptrdiff_t>(skip),
	                             samples.end());
	std::sort(measured.begin(), measured.end());
	stats.min    = measured.front();
	stats.max    = measured.back();
	stats.median = measured.size() % 2 == 1
	                   ? measured[measured.size() / 2]
	                   : 0.5 * (measured[measured.size() / 2 - 1] + measured[measured.size() / 2]);
	stats.jitter = stats.median > 0.0 ? (stats.max - stats.min) / stats.median : 0.0;
	return stats;
}

void PrintStats(const char* label, const Stats& stats) {
	::printf("  %-14s min %8.3f  median %8.3f  max %8.3f  jitter %6.2f%%\n", label, stats.min,
	         stats.median, stats.max, stats.jitter * 100.0);
}

std::string JsonStats(const Stats& stats) {
	char buffer[256];
	std::snprintf(buffer, sizeof(buffer),
	              "{\"min_ms\":%.4f,\"median_ms\":%.4f,\"max_ms\":%.4f,\"jitter\":%.5f,"
	              "\"total_ms\":%.4f}",
	              stats.min, stats.median, stats.max, stats.jitter, stats.total);
	return buffer;
}

std::string JsonSamples(const std::vector<double>& samples) {
	std::string text = "[";
	for (size_t i = 0; i < samples.size(); i++) {
		char buffer[32];
		std::snprintf(buffer, sizeof(buffer), "%s%.4f", i == 0 ? "" : ",", samples[i]);
		text += buffer;
	}
	text += "]";
	return text;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
// A replay drives the render path over restored memory, so a bad descriptor faults on the host
// instead of failing a check. The process would otherwise die silently with no output at all; this
// prints what faulted where and leaves a non-zero exit code. It runs only after every vectored
// handler has declined the exception, so the GPU page tracker is untouched, and it is installed
// only by a replay.
LONG WINAPI ReplayCrashFilter(EXCEPTION_POINTERS* pointers) {
	const auto* record  = pointers->ExceptionRecord;
	const auto  address = reinterpret_cast<uint64_t>(record->ExceptionAddress);

	::printf("replay: host exception 0x%08lx at 0x%016llx", record->ExceptionCode,
	         static_cast<unsigned long long>(address));
	if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
		const auto* what = record->ExceptionInformation[0] == 0   ? "reading"
		                   : record->ExceptionInformation[0] == 1 ? "writing"
		                                                          : "executing";
		::printf(", %s 0x%016llx", what,
		         static_cast<unsigned long long>(record->ExceptionInformation[1]));
	}
	HMODULE module = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                       reinterpret_cast<LPCSTR>(address), &module) != 0) {
		char name[MAX_PATH] {};
		GetModuleFileNameA(module, name, sizeof(name) - 1);
		::printf(" (%s+0x%llx)", name,
		         static_cast<unsigned long long>(address - reinterpret_cast<uint64_t>(module)));
	}
	::printf("\n");
	::fflush(stdout);
	::_Exit(4);
}
#endif

void Fail(const std::string& message) {
	::printf("replay: %s\n", message.c_str());
	LOGF_COLOR(Log::Color::BrightRed, "replay: %s\n", message.c_str());
}

// A guest range the memory tracker will accept (regionDefinitions.h, GuestRange::Valid). A
// recorded event always satisfies it, but a hand-edited stream must not take the process down.
constexpr uint64_t GUEST_ADDRESS_LIMIT = 1ull << 40u;

bool IsMarkableRange(uint64_t vaddr, uint64_t size) noexcept {
	return vaddr != 0 && size != 0 && vaddr < GUEST_ADDRESS_LIMIT &&
	       size <= GUEST_ADDRESS_LIMIT - vaddr;
}

// The marker thread spins on the GPU thread's progress counter, which moves tens of thousands of
// times per loop, so latency matters more than the core it costs; it yields occasionally anyway
// so it cannot starve the machine if the counter stalls.
void SpinPause() noexcept {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	YieldProcessor();
#else
	std::this_thread::yield();
#endif
}

// Imitating the guest's writes to the memory a BDA scan is about to copy (step 0 of
// docs/bda-sync-design.md).
//
// In the game the bytes a scan's memcpy reads were written by a guest thread on the other CCD
// moments earlier, so every cache line is a cross-CCD transfer; in a replay nothing writes them
// between loops and the lines are cold in DRAM. This thread puts that freshness back: around every
// dirty mark the replay applies, it walks the marked range one 64-byte line at a time and performs
// a volatile load and a store of the same value, which leaves the bytes unchanged and the line
// dirty in its own core's cache. --replay-writer, off by default.
//
// The handoff is synchronous: the caller publishes the ranges, spins until the writer is done and
// only then goes on, so the rewrite always precedes the BDA scan the mark provokes. The wait is
// timed on the caller's side and reported apart from ms/loop, because it is the harness's cost and
// not the render path's.
//
// The rewrite runs *after* the mark, not before it, and it has to. Before the mark the range's
// pages are still write-protected by the GPU tracker, so the first store faults; the host fault
// handler resolves that through BufferCache::InvalidateMemory, which for a GPU-dirty range reads
// it back with SendCommandSync -- and the thread it would have to wait for is the GPU thread,
// which is the one spinning here. Applying the mark first clears the GPU-dirty bits and leaves
// every page of the range writable, so the rewrite touches memory the way a guest thread does and
// cannot deadlock. Either order puts the write between the last upload and the next one, which is
// what the experiment is about (docs/frame-replay.md, "Writer imitation").
class MemoryWriter final {
public:
	MemoryWriter(Common::ThreadAffinityGroup group, const std::vector<RestoredRange>* ranges)
	    : m_ranges(ranges) {
		m_thread = std::thread([this, group] {
			Common::ApplyThreadAffinity(group, "ReplayWriter");
			uint64_t served = 0;
			for (;;) {
				// Quit first: the destructor sets the flag and then bumps the ticket to wake this
				// thread, and by then the last job's ranges are nobody's to touch.
				if (m_quit.load(std::memory_order_relaxed)) {
					return;
				}
				const auto ticket = m_request.load(std::memory_order_acquire);
				if (ticket == served) {
					SpinPause();
					continue;
				}
				Rewrite();
				served = ticket;
				m_done.store(served, std::memory_order_release);
			}
		});
	}

	~MemoryWriter() {
		m_quit.store(true, std::memory_order_relaxed);
		m_request.fetch_add(1, std::memory_order_release);
		if (m_thread.joinable()) {
			m_thread.join();
		}
	}

	KYTY_CLASS_NO_COPY(MemoryWriter);

	// Called from whichever thread is about to apply the marks, one at a time: the GPU thread for
	// the pre-event batch and for every inline mark, the feeder for the late ones.
	void Touch(const DirtyEventRecord* events, size_t count) {
		if (count == 0) {
			return;
		}
		m_events = events;
		m_count  = count;
		const auto ticket = m_request.load(std::memory_order_relaxed) + 1;
		const auto start  = Clock::now();
		m_request.store(ticket, std::memory_order_release);
		while (m_done.load(std::memory_order_acquire) != ticket) {
			SpinPause();
		}
		m_wait_ns.fetch_add(static_cast<uint64_t>(
		                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
		                                                                             start)
		                            .count()),
		                    std::memory_order_relaxed);
	}

	void Reset() noexcept {
		ResetWait();
		m_bytes.store(0, std::memory_order_relaxed);
		m_skipped_pages.store(0, std::memory_order_relaxed);
	}

	void ResetWait() noexcept { m_wait_ns.store(0, std::memory_order_relaxed); }

	[[nodiscard]] uint64_t WaitNs() const noexcept {
		return m_wait_ns.load(std::memory_order_relaxed);
	}
	[[nodiscard]] uint64_t Bytes() const noexcept { return m_bytes.load(std::memory_order_relaxed); }
	[[nodiscard]] uint64_t SkippedPages() const noexcept {
		return m_skipped_pages.load(std::memory_order_relaxed);
	}

private:
	// A page the writer may touch: restored, committed and mapped read-write by the guest. A page
	// in a range the capture left reserved, or one the guest mapped read-only, is skipped and
	// counted; a store there would fault on something the host fault handler cannot resolve.
	static bool IsRewritablePage(const RestoredRange* range) noexcept {
		if (range == nullptr) {
			return false;
		}
		const auto mode = ModeFromProt(range->prot);
		return mode == VirtualMemory::Mode::ReadWrite ||
		       mode == VirtualMemory::Mode::ExecuteReadWrite;
	}

	void Rewrite() {
		constexpr uint64_t LINE = 64;
		uint64_t           bytes   = 0;
		uint64_t           skipped = 0;
		for (size_t i = 0; i < m_count; i++) {
			const auto& event = m_events[i];
			// A recorded mark is usually one byte wide -- the host fault handler invalidates the
			// faulting address, not the guest's store -- while the mark it applies dirties the
			// whole 4 KiB tracker page and the scan that follows copies that page. The imitation
			// has to cover what the scan copies, so the range is widened to tracker pages.
			uint64_t   address = event.vaddr & ~(TRACKER_PAGE_SIZE - 1);
			const auto end =
			    (event.vaddr + event.size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
			while (address < end) {
				const auto page     = address & ~(kPageSize - 1);
				const auto page_end = std::min(page + kPageSize, end);
				if (!IsRewritablePage(FindRange(*m_ranges, page))) {
					skipped++;
					address = page_end;
					continue;
				}
				// One load and one store per 64-byte line, of the value already there: the bytes
				// do not change, and the line ends up dirty in this core's cache.
				for (auto line = address & ~(LINE - 1); line < page_end; line += LINE) {
					auto* word = reinterpret_cast<volatile uint64_t*>(line); // NOLINT
					const auto value = *word;
					*word            = value;
				}
				bytes += page_end - address;
				address = page_end;
			}
		}
		m_bytes.fetch_add(bytes, std::memory_order_relaxed);
		m_skipped_pages.fetch_add(skipped, std::memory_order_relaxed);
	}

	const std::vector<RestoredRange>* m_ranges = nullptr;
	const DirtyEventRecord*           m_events = nullptr;
	size_t                            m_count  = 0;
	std::thread                       m_thread;
	std::atomic_uint64_t              m_request {0};
	std::atomic_uint64_t              m_done {0};
	std::atomic_bool                  m_quit {false};
	std::atomic_uint64_t              m_wait_ns {0};
	std::atomic_uint64_t              m_bytes {0};
	std::atomic_uint64_t              m_skipped_pages {0};
};

// Replays the frame's CPU writes *when* they happened (docs/frame-replay.md, phase D).
//
// In the game the guest dirties pages throughout the frame, interleaved with the GPU thread's
// draws, and every mark advances the BDA generation, so PrepareBda rescans hundreds of times a
// frame. A replay that re-marks the whole recorded set in one batch before the frame bumps the
// generation a couple of dozen times and misses most of that cost. This thread walks the recorded
// events in arrival order, waits until the GPU thread's progress clock has reached the value the
// event carries, and marks the range the way the game's invalidation path does.
//
// A loop that outruns the events is not an error: Finish() stops the waiting, marks whatever is
// left at once and reports how many were late, which is what the replay report prints.
class DirtyEventMarker final {
public:
	DirtyEventMarker(BufferCache& cache, MemoryWriter* writer): m_cache(cache), m_writer(writer) {
		m_thread = std::thread([this] { Run(); });
	}

	~DirtyEventMarker() {
		{
			std::lock_guard lock(m_mutex);
			m_quit = true;
		}
		m_start.notify_one();
		if (m_thread.joinable()) {
			m_thread.join();
		}
	}

	KYTY_CLASS_NO_COPY(DirtyEventMarker);

	// `events` are the frame's timed marks; they outlive the run, so the thread borrows them.
	void Start(const std::vector<DirtyEventRecord>* events) {
		{
			std::lock_guard lock(m_mutex);
			m_abandon.store(false, std::memory_order_relaxed);
			m_events  = events;
			m_late    = 0;
			m_running = true;
		}
		m_start.notify_one();
	}

	[[nodiscard]] uint64_t Finish() {
		m_abandon.store(true, std::memory_order_relaxed);
		std::unique_lock lock(m_mutex);
		m_finished.wait(lock, [this] { return !m_running; });
		return m_late;
	}

private:
	void Run() {
		for (;;) {
			std::unique_lock lock(m_mutex);
			m_start.wait(lock, [this] { return m_running || m_quit; });
			if (m_quit) {
				return;
			}
			lock.unlock();

			uint64_t    late   = 0;
			const auto* events = m_events;
			for (const auto& event: *events) {
				uint32_t spins = 0;
				while (GuestGpu::Progress() < event.progress) {
					if (m_abandon.load(std::memory_order_relaxed)) {
						late++;
						break;
					}
					if ((++spins & 0x3ffu) == 0) {
						std::this_thread::yield();
					} else {
						SpinPause();
					}
				}
				// The game path: InvalidateMemory, not a bare mark, because a range the GPU still
				// owns has to be flushed back before its CPU state changes -- marking it outright
				// from another thread trips the tracker's "CPU dirty state conflicts with GPU
				// dirty state" check.
				m_cache.InvalidateMemory(event.vaddr, event.size);
				// The rewrite follows the mark; see MemoryWriter for why it cannot precede it.
				if (m_writer != nullptr) {
					m_writer->Touch(&event, 1);
				}
			}

			lock.lock();
			m_late    = late;
			m_running = false;
			lock.unlock();
			m_finished.notify_one();
		}
	}

	BufferCache&                         m_cache;
	MemoryWriter*                        m_writer = nullptr;
	const std::vector<DirtyEventRecord>* m_events = nullptr;
	std::thread                          m_thread;
	std::mutex                    m_mutex;
	std::condition_variable       m_start;
	std::condition_variable       m_finished;
	std::atomic_bool              m_abandon {false};
	uint64_t                      m_late    = 0;
	bool                          m_running = false;
	bool                          m_quit    = false;
};

// Applying the frame's CPU writes inline, on the GPU thread (format version 5).
//
// The marker thread of phase D waits for the progress clock to pass a value and then marks from
// another thread, so a burst of marks recorded at one tick is applied at once and, worse, may land
// after the GPU thread has already passed the preparation it was meant to precede. From version 5
// the clock ticks twice per draw and dispatch and the GPU thread itself applies, at every tick, the
// marks recorded at that tick, before it goes on -- which is where the game's own writes reached
// the cache. The hook runs on the GPU thread only; the feeder sets the frame up before its first
// submission and reads the cursor after the queues are drained, so the queue's own synchronisation
// orders the two.
struct InlineMarking {
	BufferCache*                         cache  = nullptr;
	const std::vector<DirtyEventRecord>* events = nullptr;
	MemoryWriter*                        writer = nullptr;
	std::atomic_size_t                   cursor {0};
};

InlineMarking g_inline_marking;

void InlineProgressHook(uint32_t progress) {
	auto&       state  = g_inline_marking;
	const auto* events = state.events;
	if (events == nullptr) {
		return;
	}
	auto cursor = state.cursor.load(std::memory_order_relaxed);
	while (cursor < events->size() && (*events)[cursor].progress <= progress) {
		const auto& event = (*events)[cursor];
		// Publish the cursor before the call: InvalidateMemory is the game's own path and may
		// drain the device, and nothing may apply this event twice.
		state.cursor.store(++cursor, std::memory_order_relaxed);
		state.cache->InvalidateMemory(event.vaddr, event.size);
		// Then the writer rewrites the range, so the scan this mark provokes copies lines that are
		// dirty in another core's cache (docs/bda-sync-design.md, step 0).
		if (state.writer != nullptr) {
			state.writer->Touch(&event, 1);
		}
	}
}

// What the replay's own BDA preparations did, counted through the same sink the capture writes
// prepare-events.bin from. This is how a replay reports its scan count without a Tracy capture.
struct PrepareCounters {
	std::atomic_uint64_t prepares {0};
	std::atomic_uint64_t scans {0};
	std::atomic_uint64_t dirty_ranges {0};
	std::atomic_uint64_t synchronized {0};
	std::atomic_uint64_t dirty_bytes {0};
	std::atomic_uint64_t scan_ns {0};
	std::atomic_uint64_t protect_calls {0};
	std::atomic_uint64_t protect_pages {0};

	void Reset() noexcept {
		prepares.store(0, std::memory_order_relaxed);
		scans.store(0, std::memory_order_relaxed);
		dirty_ranges.store(0, std::memory_order_relaxed);
		synchronized.store(0, std::memory_order_relaxed);
		dirty_bytes.store(0, std::memory_order_relaxed);
		scan_ns.store(0, std::memory_order_relaxed);
		protect_calls.store(0, std::memory_order_relaxed);
		protect_pages.store(0, std::memory_order_relaxed);
	}
};

PrepareCounters g_prepare_counters;

void CountPrepareEvent(const Replay::PrepareSample& sample) {
	auto& counters = g_prepare_counters;
	counters.prepares.fetch_add(1, std::memory_order_relaxed);
	if (!sample.scanned) {
		return;
	}
	counters.scans.fetch_add(1, std::memory_order_relaxed);
	counters.dirty_ranges.fetch_add(sample.dirty_ranges, std::memory_order_relaxed);
	counters.synchronized.fetch_add(sample.synchronized, std::memory_order_relaxed);
	counters.dirty_bytes.fetch_add(sample.dirty_bytes, std::memory_order_relaxed);
	counters.scan_ns.fetch_add(sample.scan_ns, std::memory_order_relaxed);
	counters.protect_calls.fetch_add(sample.protect_calls, std::memory_order_relaxed);
	counters.protect_pages.fetch_add(sample.protect_pages, std::memory_order_relaxed);
}

// The guest's job-system workers busy-wait in guest code while the render thread works: twelve
// core equivalents of spinning, on the CPUs the affinity policy gives the guest
// (docs/investigations/cpu-profile-2026-09-10.md). A replay runs no guest code, so the machine is
// idle around it and every kernel call the render thread makes -- a page re-protection above all --
// is cheaper than it is in the game. These threads put that load back without pretending to do the
// guest's work: one relaxed load of a shared flag and a pause per iteration, no syscalls, no
// writes. --replay-spin-threads, off by default.
class SpinThreads final {
public:
	explicit SpinThreads(uint32_t count) {
		m_threads.reserve(count);
		for (uint32_t i = 0; i < count; i++) {
			m_threads.emplace_back([this] {
				Common::ApplyThreadAffinity(Common::ThreadAffinityGroup::Guest, "ReplaySpin");
				while (!m_stop.load(std::memory_order_relaxed)) {
					SpinPause();
				}
			});
		}
	}

	~SpinThreads() {
		m_stop.store(true, std::memory_order_relaxed);
		for (auto& thread: m_threads) {
			if (thread.joinable()) {
				thread.join();
			}
		}
	}

	KYTY_CLASS_NO_COPY(SpinThreads);

private:
	std::atomic_bool         m_stop {false};
	std::vector<std::thread> m_threads;
};

// What one frame's preparations amounted to, in the capture or in one replayed loop.
struct PrepareSummary {
	uint64_t prepares      = 0;
	uint64_t scans         = 0;
	uint64_t dirty_ranges  = 0;
	uint64_t synchronized  = 0;
	uint64_t dirty_bytes   = 0;
	uint64_t scan_ns       = 0;
	uint64_t protect_calls = 0;
	uint64_t protect_pages = 0;
};

// One frame of a capture: the slice of submissions.bin between two Done records, the frame's own
// CPU-dirty marks, and what the recorder said about it. A version 1 to 3 capture has exactly one.
struct ReplayFrame {
	uint64_t number   = 0; // GuestGpu frame number in the captured run
	uint32_t progress = 0; // draws plus dispatches the capture recorded for it
	size_t   first    = 0; // index of its first submission record
	size_t   count    = 0; // how many, the Done record excluded
	size_t   graphics = 0;
	size_t   compute  = 0;
	size_t   flips    = 0;
	// The marks that arrived before its first draw, applied with the batch on the GPU thread, and
	// the rest, replayed against the progress clock. The timed ones are sorted by progress: the
	// stream is in arrival order, which is the same thing up to a race on the clock.
	std::vector<DirtyEventRecord> pre_events;
	std::vector<DirtyEventRecord> timed_events;
	// What the capture recorded this frame's BDA preparations doing (format version 5): the
	// ground truth the replay's own counts are compared with.
	PrepareSummary recorded_prepares;
};

// `<path>` becomes `<path stem>-f<n><extension>`, so one loop of M frames writes M images.
std::filesystem::path FrameImagePath(const std::filesystem::path& image, size_t frame_index) {
	auto path = image;
	path.replace_filename(image.stem().string() + "-f" + std::to_string(frame_index + 1) +
	                      image.extension().string());
	return path;
}

// The raw image plus the sidecar that says how to read it (docs/frame-replay.md).
bool WritePresentedImage(const std::filesystem::path& path, const PresentedImage& readback) {
	std::ofstream raw(path, std::ios::binary | std::ios::trunc);
	if (!raw.is_open()) {
		return false;
	}
	raw.write(reinterpret_cast<const char*>(readback.pixels.data()),
	          static_cast<std::streamsize>(readback.pixels.size()));
	raw.close();
	auto sidecar_path = path;
	sidecar_path += ".json";
	std::ofstream sidecar(sidecar_path, std::ios::binary | std::ios::trunc);
	if (!sidecar.is_open()) {
		return false;
	}
	sidecar << "{\"width\":" << readback.width << ",\"height\":" << readback.height
	        << ",\"format\":\"" << readback.format
	        << "\",\"bytes_per_pixel\":" << readback.bytes_per_pixel
	        << ",\"stride\":" << readback.width * readback.bytes_per_pixel << "}\n";
	sidecar.close();
	return true;
}

// One contiguous run of recorded dirty pages, so a loop re-marks them with a handful of calls.
std::vector<RestoredRange> CoalescePages(std::vector<uint64_t> pages) {
	std::sort(pages.begin(), pages.end());
	pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
	std::vector<RestoredRange> ranges;
	for (const auto page: pages) {
		if (!ranges.empty() && ranges.back().start + ranges.back().size == page) {
			ranges.back().size += kPageSize;
			continue;
		}
		ranges.push_back({page, kPageSize, 0});
	}
	return ranges;
}

} // namespace

bool IsWaitDiagnosticsEnabled() noexcept {
	return g_wait_diagnostics.load(std::memory_order_relaxed);
}

void RecordBlockedWait(uint64_t address, uint64_t value, uint64_t reference, uint64_t mask,
                       uint32_t function, uint32_t width) {
	std::lock_guard lock(g_wait_mutex);
	g_last_wait = {address, value, reference, mask, function, width, true};
}

std::string DescribeLastBlockedWait() {
	std::lock_guard lock(g_wait_mutex);
	if (!g_last_wait.valid) {
		return "no WAIT_REG_MEM was recorded";
	}
	char buffer[256];
	std::snprintf(buffer, sizeof(buffer),
	              "WAIT_REG_MEM addr=0x%016llx value=0x%016llx ref=0x%016llx mask=0x%016llx "
	              "func=%u width=%u",
	              static_cast<unsigned long long>(g_last_wait.address),
	              static_cast<unsigned long long>(g_last_wait.value),
	              static_cast<unsigned long long>(g_last_wait.reference),
	              static_cast<unsigned long long>(g_last_wait.mask), g_last_wait.function,
	              g_last_wait.width);
	return buffer;
}

int RunReplay(const std::filesystem::path& dir, uint32_t loops, uint32_t frames_wanted,
              bool dirty_set_once, uint32_t spin_threads, Config::ReplayWriter writer_mode,
              const std::filesystem::path& image) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SetUnhandledExceptionFilter(ReplayCrashFilter);
#endif

	// The GPU page tracker protects guest pages and resolves the faults through the host fault
	// handler the ELF loader installs. A replay loads no ELF, so it installs the same handler
	// itself; without it the first tracked access on the render thread kills the process.
	Loader::InstallHostFaultHandler();

	CaptureReader reader;
	std::string   error;

	if (!reader.Open(dir, &error)) {
		Fail(error);
		return 1;
	}

	auto* renderer = GetRenderContext();
	if (renderer == nullptr) {
		Fail("the graphics lifecycle is not up");
		return 1;
	}
	auto& gpu = renderer->GetGpu();

	::printf("replay: %s\n", dir.string().c_str());
	::printf("  frame          %llu\n", static_cast<unsigned long long>(reader.Manifest().frame));

	// 1. Ranges.
	std::vector<RangeRecord> records;
	if (!reader.ReadRanges(&records, &error)) {
		Fail(error);
		return 1;
	}
	const auto                 restore_start = Clock::now();
	std::vector<RestoredRange> restored;
	restored.reserve(records.size());
	uint64_t skipped_ranges = 0;
	uint64_t committed_size = 0;
	for (const auto& record: records) {
		char name[sizeof(record.name) + 1] {};
		std::memcpy(name, record.name, sizeof(record.name));
		if (!Memory::RestoreRange(record.vaddr, record.size, record.prot, record.type, name)) {
			skipped_ranges++;
			LOGF_COLOR(Log::Color::BrightYellow,
			           "replay: could not restore range 0x%016llx size 0x%llx type %u prot %u\n",
			           static_cast<unsigned long long>(record.vaddr),
			           static_cast<unsigned long long>(record.size), record.type, record.prot);
			continue;
		}
		if (IsCommittedRangeType(record.type)) {
			restored.push_back({record.vaddr, record.size, record.prot});
			committed_size += record.size;
		}
	}
	std::sort(restored.begin(), restored.end(),
	          [](const RestoredRange& left, const RestoredRange& right) {
		          return left.start < right.start;
	          });
	const auto ranges_ms = MillisSince(restore_start);

	// 2. Pages.
	const auto pages_start   = Clock::now();
	uint64_t   pages_written = 0;
	uint64_t   pages_failed  = 0;
	uint64_t   page_count    = 0;
	const auto ok            = reader.ForEachPage(
	    [&](uint64_t vaddr, const uint8_t* data) {
		    if (WritePage(restored, vaddr, data)) {
			    pages_written++;
		    } else {
			    pages_failed++;
			    if (pages_failed <= 16) {
				    LOGF_COLOR(Log::Color::BrightYellow,
				               "replay: page 0x%016llx has no restored range\n",
				               static_cast<unsigned long long>(vaddr));
			    }
		    }
		    return true;
	    },
	    &page_count, &error);
	if (!ok) {
		Fail(error);
		return 1;
	}
	const auto pages_ms = MillisSince(pages_start);

	::printf("  ranges         %zu restored, %llu skipped, %.1f MiB committed, %.0f ms\n",
	         records.size() - static_cast<size_t>(skipped_ranges),
	         static_cast<unsigned long long>(skipped_ranges),
	         static_cast<double>(committed_size) / (1024.0 * 1024.0), ranges_ms);
	::printf("  memory         %llu pages, %.1f MiB, %.0f ms%s\n",
	         static_cast<unsigned long long>(pages_written),
	         static_cast<double>(pages_written * kPageSize) / (1024.0 * 1024.0), pages_ms,
	         pages_failed != 0 ? " (some pages unmapped)" : "");
	if (pages_failed != 0) {
		::printf("  WARNING        %llu pages had no restored range\n",
		         static_cast<unsigned long long>(pages_failed));
	}

	// 2b. PRT apertures. A partly resident image is a committed head followed by a reserved hole,
	// and BufferCache reads it through Memory::TryReadPrtBacking, which refuses outside an
	// aperture. Restore them before anything can touch such an image.
	std::vector<PrtApertureRecord> apertures;
	bool                           apertures_present = false;
	if (!reader.ReadPrtApertures(&apertures, &apertures_present, &error)) {
		Fail(error);
		return 1;
	}
	uint32_t apertures_restored = 0;
	for (const auto& aperture: apertures) {
		if (Memory::KernelSetPrtAperture(aperture.index,
		                                 reinterpret_cast<void*>(aperture.address), // NOLINT
		                                 aperture.size) != 0) {
			Fail("could not restore PRT aperture " + std::to_string(aperture.index));
			return 1;
		}
		apertures_restored++;
	}
	if (!apertures_present) {
		::printf("  prt apertures  none recorded (format version %u); a partly resident image "
		         "cannot be read\n",
		         reader.Manifest().format_version);
	} else {
		::printf("  prt apertures  %u restored\n", apertures_restored);
	}

	// 2c. Shader map. Guest code created every shader before the capture; version 2 records the
	// map, version 1 leaves it to be recovered from the headers the game left in guest memory.
	std::vector<ShaderRecord> shader_records;
	bool                      shaders_present = false;
	if (!reader.ReadShaders(&shader_records, &shaders_present, &error)) {
		Fail(error);
		return 1;
	}
	const auto shaders_start = Clock::now();
	const auto shaders       = shaders_present && !shader_records.empty()
	                               ? RestoreRecordedShaderMap(shader_records)
	                               : RestoreShaderMap(restored);
	::printf("  shaders        %llu registrations %s, %.0f ms\n",
	         static_cast<unsigned long long>(shaders),
	         shaders_present && !shader_records.empty() ? "restored" : "recovered by scan",
	         MillisSince(shaders_start));
	if (shaders == 0) {
		::printf("  WARNING        the shader map is empty; every draw will fail\n");
	}

	// 3a. Register files, on the GPU thread so the lazy compute processors are created there.
	std::vector<CaptureRegisterFile> register_files;
	if (!reader.ReadRegisterFiles(&register_files, &error)) {
		Fail(error);
		return 1;
	}
	const auto expected_register_size = GuestGpu::RegisterFileSize();
	for (const auto& file: register_files) {
		if (file.data.size() != expected_register_size) {
			Fail("registers.bin queue " + std::to_string(file.queue_id) + " has " +
			     std::to_string(file.data.size()) + " bytes, this build's register file is " +
			     std::to_string(expected_register_size) +
			     " bytes (HW::Context + HW::UserConfig + HW::Shader); the capture was taken by a "
			     "different build");
			return 1;
		}
		if (file.queue_id >= GuestGpu::RegisterFileQueueCount()) {
			Fail("registers.bin has queue id " + std::to_string(file.queue_id) +
			     ", out of range for this build");
			return 1;
		}
	}
	bool registers_ok = true;
	gpu.SendCommandSync([&]() {
		for (const auto& file: register_files) {
			registers_ok =
			    gpu.RestoreRegisterFile(file.queue_id, file.data.data(), file.data.size()) &&
			    registers_ok;
		}
	});
	if (!registers_ok) {
		Fail("a recorded register file could not be restored");
		return 1;
	}

	// 3b. Video-out registrations.
	std::vector<CaptureVideoOut> video_out;
	if (!reader.ReadVideoOut(&video_out, &error)) {
		Fail(error);
		return 1;
	}
	if (video_out.empty()) {
		Fail("the capture registered no video-out buffers, so nothing can be presented");
		return 1;
	}
	// A port hands out a fixed handle, bus type plus one. The recorded PM4 stream flips on the
	// handle the capture recorded, so the replay has to end up with the same one.
	const int handle = VideoOut::VideoOutReplayOpen(video_out.front().header.handle - 1);
	if (handle < 0) {
		Fail("could not open a video-out port");
		return 1;
	}
	if (handle != video_out.front().header.handle) {
		Fail("the capture flips on video-out handle " +
		     std::to_string(video_out.front().header.handle) + " but this run opened " +
		     std::to_string(handle));
		return 1;
	}
	int flip_index = 0;
	for (size_t i = 0; i < video_out.size(); i++) {
		const auto& registration = video_out[i];
		const int   result       = VideoOut::VideoOutReplayRegisterBuffers(
		    handle, registration.header.set_index, registration.header.index_start,
		    registration.header.count, registration.header.category, registration.attribute.data(),
		    registration.attribute.size(), registration.addresses.data());
		if (result != 0) {
			Fail("VideoOutRegisterBuffers2 rejected recorded group " +
			     std::to_string(registration.header.set_index) + ": " + std::to_string(result));
			return 1;
		}
		if (i == 0) {
			flip_index = registration.header.index_start;
		}
	}

	// 3c. Submissions, split into frames. The dwords stay in these vectors for the whole run:
	// GuestGpu borrows the spans and the GPU thread reads them long after the feeder moved on.
	std::vector<CaptureSubmission> submissions;
	if (!reader.ReadSubmissions(&submissions, &error)) {
		Fail(error);
		return 1;
	}
	// A Done record ends a frame and carries its number and progress count (format version 4);
	// in an older capture those fields are zero and there is exactly one frame.
	std::vector<ReplayFrame> recorded;
	{
		ReplayFrame current;
		current.first = 0;
		for (size_t i = 0; i < submissions.size(); i++) {
			const auto& header = submissions[i].header;
			switch (static_cast<SubmissionKind>(header.kind)) {
				case SubmissionKind::Graphics:
					current.graphics++;
					current.count++;
					break;
				case SubmissionKind::Compute:
					current.compute++;
					current.count++;
					break;
				case SubmissionKind::FlipPreparation:
					current.flips++;
					current.count++;
					break;
				case SubmissionKind::Done:
					current.number   = header.flip_request_id;
					current.progress = header.queue_id;
					recorded.push_back(std::move(current));
					current       = ReplayFrame {};
					current.first = i + 1;
					break;
			}
		}
		if (current.count != 0) {
			::printf("  WARNING        %zu submissions after the last Done record are ignored\n",
			         current.count);
		}
	}
	if (recorded.empty()) {
		Fail("submissions.bin has no Done record, so the frame has no end");
		return 1;
	}
	if (reader.Manifest().frames != 0 && reader.Manifest().frames != recorded.size()) {
		::printf("  WARNING        the manifest says %u frames, submissions.bin has %zu\n",
		         reader.Manifest().frames, recorded.size());
	}
	// The last recorded frame is the one the memory snapshot ends on, so a shorter sequence keeps
	// the tail of the window, not its head: every frame the replay drops is a frame further from
	// the state the restore puts the guest in.
	const size_t frame_count = frames_wanted == 0
	                               ? recorded.size()
	                               : std::min<size_t>(frames_wanted, recorded.size());
	if (frames_wanted > recorded.size()) {
		::printf("  WARNING        --replay-frames %u, but the capture holds %zu; replaying %zu\n",
		         frames_wanted, recorded.size(), recorded.size());
	}
	const size_t frame_base = recorded.size() - frame_count;

	// 3d. Dirty pages. One set for the whole capture, taken at the end of the last frame; it is
	// re-marked before every replayed frame, as it has been since phase B, because almost none of
	// it comes from a CPU write of the frame -- it is memory nothing has uploaded yet.
	std::vector<uint64_t> dirty_pages;
	if (!reader.ReadDirtyPages(&dirty_pages, &error)) {
		Fail(error);
		return 1;
	}
	const auto dirty_ranges = CoalescePages(dirty_pages);

	// 3e. Dirty events (format version 3): the same CPU writes with the moment they arrived, keyed
	// to the GPU thread's progress clock, and from version 4 to the frame they arrived in. Without
	// them the loop can only re-mark everything in one batch, which is why the phase C A/B did not
	// reproduce (docs/frame-replay.md).
	std::vector<DirtyEventRecord> dirty_events;
	bool                          dirty_events_present = false;
	if (!reader.ReadDirtyEvents(&dirty_events, &dirty_events_present, &error)) {
		Fail(error);
		return 1;
	}
	uint64_t skipped_events = 0;
	size_t   timed_total    = 0;
	for (const auto& event: dirty_events) {
		if (!IsMarkableRange(event.vaddr, event.size) || event.frame >= recorded.size()) {
			skipped_events++;
			continue;
		}
		auto& frame = recorded[event.frame];
		(event.progress == 0 ? frame.pre_events : frame.timed_events).push_back(event);
	}
	for (size_t i = 0; i < frame_count; i++) {
		timed_total += recorded[frame_base + i].timed_events.size();
	}
	const bool use_events = dirty_events_present && !dirty_events.empty();
	// Version 5 keys the marks to a clock that ticks twice per draw and dispatch and applies them
	// on the GPU thread; an older capture's values mean something else and keep the marker thread.
	const bool inline_events = use_events && reader.Manifest().format_version >= 5;
	for (auto& frame: recorded) {
		std::stable_sort(frame.timed_events.begin(), frame.timed_events.end(),
		                 [](const DirtyEventRecord& left, const DirtyEventRecord& right) {
			                 return left.progress < right.progress;
		                 });
	}

	// 3f. Prepare events (format version 5): every BDA preparation the captured frames made and
	// whether it scanned. Nothing in the replay consumes it -- it is the ground truth the replay's
	// own preparation counts are reported against.
	std::vector<PrepareEventRecord> prepare_events;
	bool                            prepare_events_present = false;
	if (!reader.ReadPrepareEvents(&prepare_events, &prepare_events_present, &error)) {
		Fail(error);
		return 1;
	}
	for (const auto& event: prepare_events) {
		if (event.frame >= recorded.size()) {
			continue;
		}
		auto& summary = recorded[event.frame].recorded_prepares;
		summary.prepares++;
		if (event.scanned == 0) {
			continue;
		}
		summary.scans++;
		summary.dirty_ranges += event.dirty_ranges;
		summary.synchronized += event.synchronized;
		summary.dirty_bytes += event.dirty_bytes;
		summary.scan_ns += event.scan_ns;
	}

	size_t graphics_count = 0;
	size_t compute_count  = 0;
	size_t flip_count     = 0;
	uint64_t progress_recorded = 0;
	for (size_t i = 0; i < frame_count; i++) {
		graphics_count += recorded[frame_base + i].graphics;
		compute_count += recorded[frame_base + i].compute;
		flip_count += recorded[frame_base + i].flips;
		progress_recorded += recorded[frame_base + i].progress;
	}

	::printf("  registers      %zu command processors, %zu bytes each\n", register_files.size(),
	         expected_register_size);
	::printf("  video-out      %zu registrations, handle %d, flip index %d\n", video_out.size(),
	         handle, flip_index);
	::printf("  frames         %zu recorded (%llu to %llu), %zu replayed per loop (%llu to %llu)\n",
	         recorded.size(), static_cast<unsigned long long>(recorded.front().number),
	         static_cast<unsigned long long>(recorded.back().number), frame_count,
	         static_cast<unsigned long long>(recorded[frame_base].number),
	         static_cast<unsigned long long>(recorded.back().number));
	::printf("  submissions    %zu records over the replayed frames (%zu graphics, %zu compute, "
	         "%zu flips)\n",
	         graphics_count + compute_count + flip_count, graphics_count, compute_count,
	         flip_count);
	::printf("  dirty pages    %zu in %zu ranges, re-marked %s\n", dirty_pages.size(),
	         dirty_ranges.size(), dirty_set_once ? "once at restore" : "before every frame");
	::printf("  spin threads   %u on the guest CPUs\n", spin_threads);
	::printf("  writer         %s\n",
	         writer_mode == Config::ReplayWriter::Guest
	             ? "one thread on the guest CPUs, rewriting each marked range at its mark"
	             : writer_mode == Config::ReplayWriter::Render
	                   ? "one thread on the render CPUs, rewriting each marked range at its mark"
	                   : "none");
	if (prepare_events_present && !prepare_events.empty()) {
		uint64_t recorded_prepares = 0;
		uint64_t recorded_scans    = 0;
		for (size_t i = 0; i < frame_count; i++) {
			recorded_prepares += recorded[frame_base + i].recorded_prepares.prepares;
			recorded_scans += recorded[frame_base + i].recorded_prepares.scans;
		}
		::printf("  prepares       %llu recorded over the replayed frames, %llu of them scanned "
		         "(the ground truth)\n",
		         static_cast<unsigned long long>(recorded_prepares),
		         static_cast<unsigned long long>(recorded_scans));
	} else {
		::printf("  prepares       none recorded (format version %u); there is no ground truth to "
		         "compare the replay's scans with\n",
		         reader.Manifest().format_version);
	}
	if (use_events) {
		::printf("  dirty events   %zu recorded (%zu timed in the replayed frames) over %llu "
		         "draws%s\n",
		         dirty_events.size(), timed_total,
		         static_cast<unsigned long long>(progress_recorded),
		         skipped_events != 0 ? " (some out of range, skipped)" : "");
		::printf("  dirty timing   %s\n",
		         inline_events ? "applied inline on the GPU thread at each progress tick"
		                       : "applied by the marker thread (capture older than version 5)");
	} else if (reader.Manifest().format_version >= 3) {
		::printf("  dirty events   dirty-events.bin is empty; the BDA generation only moves with the "
		         "batch, which under-counts the BDA scan\n");
	} else {
		::printf("  dirty events   none recorded (format version %u); the BDA generation only moves "
		         "with the batch, which under-counts the BDA scan\n",
		         reader.Manifest().format_version);
	}
	::fflush(stdout);

	// 4. Loop. One loop is the recorded sequence of frames in order, which is what makes the
	// guest's ring buffers rotate as they did in the game (docs/frame-replay.md, phase E).
	g_wait_diagnostics.store(true, std::memory_order_relaxed);
	auto&               buffer_cache = renderer->GetBufferCache();
	uint64_t            presented    = 0;
	uint64_t            flips_before = VideoOut::VideoOutReplayFlipCount(handle);
	std::vector<double> loop_ms;
	std::vector<double> gpu_ms;
	loop_ms.reserve(loops);
	gpu_ms.reserve(loops);
	// Per frame, one sample per loop.
	std::vector<std::vector<double>> frame_loop_ms(frame_count);
	std::vector<std::vector<double>> frame_gpu_ms(frame_count);
	std::vector<uint64_t>            frame_late(frame_count, 0);
	std::vector<uint32_t>            frame_progress(frame_count, 0);
	for (size_t i = 0; i < frame_count; i++) {
		frame_loop_ms[i].reserve(loops);
		frame_gpu_ms[i].reserve(loops);
	}
	std::vector<PresentedImage> frame_images;
	const auto                  run_start = Clock::now();

	// The memory writer, if this run wants one: it lives for the whole run and is handed the
	// ranges of every mark, on the thread that is about to apply it.
	std::unique_ptr<MemoryWriter> writer;
	if (writer_mode != Config::ReplayWriter::None) {
		writer = std::make_unique<MemoryWriter>(writer_mode == Config::ReplayWriter::Render
		                                            ? Common::ThreadAffinityGroup::Render
		                                            : Common::ThreadAffinityGroup::Guest,
		                                        &restored);
	}
	std::unique_ptr<DirtyEventMarker> marker;
	if (use_events && !inline_events) {
		marker = std::make_unique<DirtyEventMarker>(buffer_cache, writer.get());
	}
	if (inline_events) {
		g_inline_marking.cache  = &buffer_cache;
		g_inline_marking.writer = writer.get();
		GuestGpu::SetProgressHook(&InlineProgressHook);
	}
	// The replay's own preparations, counted through the sink the capture writes its stream from.
	Replay::SetPrepareSink(&CountPrepareEvent);
	// The guest's spinning job workers, if this run wants them; they live until the run ends.
	std::unique_ptr<SpinThreads> spinners;
	if (spin_threads != 0) {
		spinners = std::make_unique<SpinThreads>(spin_threads);
	}
	if (dirty_set_once && !dirty_ranges.empty()) {
		gpu.SendCommandSync([&]() {
			for (const auto& range: dirty_ranges) {
				buffer_cache.MarkRegionAsCpuModified(range.start, range.size);
			}
		});
	}
	std::vector<std::vector<PrepareSummary>> frame_prepares(frame_count);
	// Per frame, per loop: how many times the render thread drained the device to read GPU-written
	// memory back (SyncGpuCleanBacking -> BufferCache::ReadMemory). Roadmap item 1 removes the
	// indirect-argument ones, which is all of them in this capture.
	std::vector<std::vector<uint64_t>> frame_drains(frame_count);
	std::vector<std::vector<uint64_t>> frame_syncs(frame_count);
	// Per frame, per loop: vkQueueSubmit calls on the graphics timeline (roadmap item 1b,
	// periodic submits), and the read faults the render thread took with the device wait inside
	// them. The last two are the item 1b headline numbers, counted in every build.
	std::vector<std::vector<uint64_t>> frame_submits(frame_count);
	std::vector<std::vector<uint64_t>> frame_faults(frame_count);
	std::vector<std::vector<uint64_t>> frame_fault_wait_ns(frame_count);
	// Per frame, per loop: what the memory writer cost and did. The wait is the render thread's,
	// the rest the writer's; both are the harness's own and are reported apart from ms/loop. The
	// pre-event batch is applied before the frame's clock starts, as it always has been, so its
	// wait is counted on its own: only the in-frame one is inside ms/loop gpu.
	// Per frame, per loop: what design P did (docs/bda-sync-design.md). All zero with
	// --bda-async-protect false.
	std::vector<std::vector<AsyncProtectCounters>> frame_async(frame_count);
	// Per frame, per loop: step 1 of docs/sync-points-design.md. All zero with
	// --gpu-prologue-table false, except the data table's fault pages, which are counted always.
	std::vector<std::vector<BdaPrologueCounters>> frame_prologue(frame_count);
	std::vector<std::vector<uint64_t>> frame_writer_pre_wait_ns(frame_count);
	std::vector<std::vector<uint64_t>> frame_writer_wait_ns(frame_count);
	std::vector<std::vector<uint64_t>> frame_writer_bytes(frame_count);
	std::vector<std::vector<uint64_t>> frame_writer_skipped(frame_count);
	uint64_t late_events = 0;

	for (uint32_t loop = 0; loop < loops; loop++) {
		ReadbackDiag::SetLoop(loop + 1);
		double loop_total = 0.0;
		double gpu_total  = 0.0;
		for (size_t index = 0; index < frame_count; index++) {
			const auto& frame = recorded[frame_base + index];
			// The recorded dirty set, in one batch on the GPU thread and outside the timing, plus
			// everything the guest wrote before this frame's first draw. The events come on top of
			// the batch, not instead of it -- see the phase D section of docs/frame-replay.md.
			// With --replay-dirty-set once the set is marked at restore instead: in the game those
			// pages stay dirty across frames without being re-marked, and re-marking them every
			// frame inflates what each BDA scan has to walk (phase E).
			const bool mark_batch = !dirty_set_once && !dirty_ranges.empty();
			if (writer) {
				writer->Reset();
			}
			if (mark_batch || !frame.pre_events.empty()) {
				gpu.SendCommandSync([&]() {
					if (mark_batch) {
						for (const auto& range: dirty_ranges) {
							buffer_cache.MarkRegionAsCpuModified(range.start, range.size);
						}
					}
					for (const auto& event: frame.pre_events) {
						buffer_cache.InvalidateMemory(event.vaddr, event.size);
					}
					if (writer) {
						writer->Touch(frame.pre_events.data(), frame.pre_events.size());
					}
				});
			}
			if (marker) {
				marker->Start(&frame.timed_events);
			}
			if (inline_events) {
				g_inline_marking.cursor.store(0, std::memory_order_relaxed);
				g_inline_marking.events = &frame.timed_events;
			}
			g_prepare_counters.Reset();
			ResetAsyncProtectCounters();
			ResetBdaPrologueFrameCounters();
			// What the pre-event batch cost, before the counters start again for the frame
			// itself; the bytes and the skipped pages stay cumulative over the loop.
			const auto pre_wait_ns = writer ? writer->WaitNs() : 0;
			if (writer) {
				writer->ResetWait();
			}
			Memory::ResetGpuBackingDrainCount();
			const auto submits_before    = CommandScheduler::TotalSubmits();
			const auto faults_before     = ReadbackDiag::TotalFaults();
			const auto fault_wait_before = ReadbackDiag::TotalWaitNs();

			const auto frame_start = Clock::now();
			for (size_t i = frame.first; i < frame.first + frame.count; i++) {
				const auto& submission = submissions[i];
				switch (static_cast<SubmissionKind>(submission.header.kind)) {
					case SubmissionKind::Graphics:
						gpu.Submit(submission.commands, submission.constants);
						break;
					case SubmissionKind::Compute:
						gpu.SubmitCompute(submission.header.queue_id, submission.commands);
						break;
					case SubmissionKind::FlipPreparation: {
						// The recorded request id belongs to the captured run; the replay reserves
						// its own flip through the path VideoOutSubmitFlip takes.
						const int result = VideoOut::VideoOutReplaySubmitFlip(handle, flip_index);
						if (result != 0) {
							g_wait_diagnostics.store(false, std::memory_order_relaxed);
							Fail("VideoOutSubmitFlip failed in loop " + std::to_string(loop) + ": " +
							     std::to_string(result));
							return 1;
						}
						break;
					}
					case SubmissionKind::Done: break;
				}
			}

			const uint32_t timeout = loop == 0 ? WARMUP_TIMEOUT_MS : WAIT_TIMEOUT_MS;
			if (!gpu.WaitForIdleFor(timeout)) {
				g_wait_diagnostics.store(false, std::memory_order_relaxed);
				if (marker) {
					(void)marker->Finish();
				}
				Fail("the GPU thread did not finish frame " + std::to_string(index + 1) + " (" +
				     std::to_string(frame.number) + ") of loop " + std::to_string(loop + 1) +
				     " within " + std::to_string(timeout) + " ms: " + DescribeLastBlockedWait());
				return 1;
			}
			frame_progress[index] = GuestGpu::Progress();
			if (inline_events) {
				// The GPU thread is idle, so the hook cannot fire again: whatever is left never had
				// its tick reached and is applied here, late, exactly as the marker thread does.
				g_inline_marking.events = nullptr;
				const auto cursor = g_inline_marking.cursor.load(std::memory_order_relaxed);
				for (size_t i = cursor; i < frame.timed_events.size(); i++) {
					buffer_cache.InvalidateMemory(frame.timed_events[i].vaddr,
					                              frame.timed_events[i].size);
					if (writer) {
						writer->Touch(&frame.timed_events[i], 1);
					}
				}
				const auto late = frame.timed_events.size() - cursor;
				frame_late[index] += late;
				late_events += late;
			}
			// The marks still waiting are marked now, inside the frame's timing, and counted: a
			// frame that outran the recorded events is one whose BDA-scan count is too low, and
			// the report says so.
			if (marker) {
				const auto late = marker->Finish();
				frame_late[index] += late;
				late_events += late;
			}
			// Done() resets the progress clock, so it must come after Finish().
			gpu.Done();
			const auto gpu_done = MillisSince(frame_start);
			// The title flips from its own command stream, so there is no recorded buffer index to
			// wait on: a frame ends when every flip it queued on the port has been presented.
			if (!VideoOut::VideoOutReplayWaitFlipsDrained(handle, FLIP_TIMEOUT_MS)) {
				g_wait_diagnostics.store(false, std::memory_order_relaxed);
				Fail("the flip queued by frame " + std::to_string(index + 1) + " of loop " +
				     std::to_string(loop + 1) + " was not presented within " +
				     std::to_string(FLIP_TIMEOUT_MS) + " ms");
				return 1;
			}
			presented += VideoOut::VideoOutReplayFlipCount(handle) - flips_before;
			flips_before = VideoOut::VideoOutReplayFlipCount(handle);
			const auto frame_done = MillisSince(frame_start);
			frame_prepares[index].push_back(
			    {g_prepare_counters.prepares.load(std::memory_order_relaxed),
			     g_prepare_counters.scans.load(std::memory_order_relaxed),
			     g_prepare_counters.dirty_ranges.load(std::memory_order_relaxed),
			     g_prepare_counters.synchronized.load(std::memory_order_relaxed),
			     g_prepare_counters.dirty_bytes.load(std::memory_order_relaxed),
			     g_prepare_counters.scan_ns.load(std::memory_order_relaxed),
			     g_prepare_counters.protect_calls.load(std::memory_order_relaxed),
			     g_prepare_counters.protect_pages.load(std::memory_order_relaxed)});
			frame_async[index].push_back(ReadAsyncProtectCounters());
			frame_prologue[index].push_back(ReadBdaPrologueCounters());
			frame_writer_pre_wait_ns[index].push_back(pre_wait_ns);
			frame_writer_wait_ns[index].push_back(writer ? writer->WaitNs() : 0);
			frame_writer_bytes[index].push_back(writer ? writer->Bytes() : 0);
			frame_writer_skipped[index].push_back(writer ? writer->SkippedPages() : 0);
			frame_drains[index].push_back(Memory::GpuBackingDrainCount());
			frame_syncs[index].push_back(Memory::GpuBackingSyncCount());
			frame_submits[index].push_back(CommandScheduler::TotalSubmits() - submits_before);
			frame_faults[index].push_back(ReadbackDiag::TotalFaults() - faults_before);
			frame_fault_wait_ns[index].push_back(ReadbackDiag::TotalWaitNs() - fault_wait_before);
			frame_gpu_ms[index].push_back(gpu_done);
			frame_loop_ms[index].push_back(frame_done);
			gpu_total += gpu_done;
			loop_total += frame_done;

			// The per-frame images come from the last loop, after its timings are taken, so the
			// readback costs the measurement nothing.
			if (!image.empty() && loop + 1 == loops) {
				auto*          presenter = WindowGetPresenter();
				PresentedImage readback;
				if (presenter != nullptr && presenter->ReadLastPresentedFrame(&readback)) {
					frame_images.push_back(std::move(readback));
				} else {
					frame_images.emplace_back();
				}
			}
		}
		loop_ms.push_back(loop_total);
		gpu_ms.push_back(gpu_total);
	}
	g_wait_diagnostics.store(false, std::memory_order_relaxed);
	spinners.reset();
	GuestGpu::SetProgressHook(nullptr);
	g_inline_marking.writer = nullptr;
	Replay::SetPrepareSink(nullptr);
	g_inline_marking.events = nullptr;
	const auto run_ms = MillisSince(run_start);

	// The measured loops' preparations, averaged per frame: the number to compare with the
	// capture's ground truth and with the game's Tracy zone.
	std::vector<PrepareSummary> prepare_mean(frame_count);
	for (size_t i = 0; i < frame_count; i++) {
		const auto& samples = frame_prepares[i];
		const auto  skipped = samples.size() > 1 ? 1u : 0u;
		const auto  counted = samples.size() - skipped;
		if (counted == 0) {
			continue;
		}
		PrepareSummary total;
		for (size_t loop = skipped; loop < samples.size(); loop++) {
			total.prepares += samples[loop].prepares;
			total.scans += samples[loop].scans;
			total.dirty_ranges += samples[loop].dirty_ranges;
			total.synchronized += samples[loop].synchronized;
			total.dirty_bytes += samples[loop].dirty_bytes;
			total.scan_ns += samples[loop].scan_ns;
			total.protect_calls += samples[loop].protect_calls;
			total.protect_pages += samples[loop].protect_pages;
		}
		prepare_mean[i] = {total.prepares / counted,      total.scans / counted,
		                   total.dirty_ranges / counted,  total.synchronized / counted,
		                   total.dirty_bytes / counted,   total.scan_ns / counted,
		                   total.protect_calls / counted, total.protect_pages / counted};
	}

	// The same average for the device drains, and their total over one loop.
	std::vector<uint64_t> drain_mean(frame_count, 0);
	std::vector<uint64_t> sync_mean(frame_count, 0);
	uint64_t              drains_per_loop = 0;
	uint64_t              syncs_per_loop  = 0;
	std::vector<uint64_t> submit_mean(frame_count, 0);
	uint64_t              submits_per_loop    = 0;
	uint64_t              faults_per_loop     = 0;
	uint64_t              fault_wait_per_loop = 0;
	const auto            mean_of = [](const std::vector<uint64_t>& samples) -> uint64_t {
		const auto skipped = samples.size() > 1 ? 1u : 0u;
		const auto counted = samples.size() - skipped;
		if (counted == 0) {
			return 0;
		}
		uint64_t total = 0;
		for (size_t loop = skipped; loop < samples.size(); loop++) {
			total += samples[loop];
		}
		return total / counted;
	};
	AsyncProtectCounters async_per_loop;
	BdaPrologueCounters  prologue_per_loop;
	uint64_t             writer_pre_wait_ns_per_loop = 0;
	uint64_t writer_wait_ns_per_loop  = 0;
	uint64_t writer_bytes_per_loop    = 0;
	uint64_t writer_skipped_per_loop  = 0;
	for (size_t i = 0; i < frame_count; i++) {
		drain_mean[i] = mean_of(frame_drains[i]);
		sync_mean[i]  = mean_of(frame_syncs[i]);
		drains_per_loop += drain_mean[i];
		syncs_per_loop += sync_mean[i];
		submit_mean[i] = mean_of(frame_submits[i]);
		submits_per_loop += submit_mean[i];
		faults_per_loop += mean_of(frame_faults[i]);
		fault_wait_per_loop += mean_of(frame_fault_wait_ns[i]);
		{
			const auto& samples = frame_async[i];
			const auto  field   = [&](uint64_t AsyncProtectCounters::*member) {
                std::vector<uint64_t> values;
                values.reserve(samples.size());
                for (const auto& sample: samples) {
                    values.push_back(sample.*member);
                }
                return mean_of(values);
			};
			async_per_loop.upload_protect_calls += field(&AsyncProtectCounters::upload_protect_calls);
			async_per_loop.upload_protect_pages += field(&AsyncProtectCounters::upload_protect_pages);
			async_per_loop.written_protect_calls +=
			    field(&AsyncProtectCounters::written_protect_calls);
			async_per_loop.written_protect_pages +=
			    field(&AsyncProtectCounters::written_protect_pages);
			async_per_loop.helper_protect_calls += field(&AsyncProtectCounters::helper_protect_calls);
			async_per_loop.helper_protect_pages += field(&AsyncProtectCounters::helper_protect_pages);
			async_per_loop.helper_batches += field(&AsyncProtectCounters::helper_batches);
			async_per_loop.landed_pages += field(&AsyncProtectCounters::landed_pages);
			async_per_loop.extra_uploads += field(&AsyncProtectCounters::extra_uploads);
			async_per_loop.drains += field(&AsyncProtectCounters::drains);
			async_per_loop.waits += field(&AsyncProtectCounters::waits);
			async_per_loop.wait_ns += field(&AsyncProtectCounters::wait_ns);
			async_per_loop.boundary_scans += field(&AsyncProtectCounters::boundary_scans);
			async_per_loop.boundary_scan_ns += field(&AsyncProtectCounters::boundary_scan_ns);
		}
		{
			const auto& samples = frame_prologue[i];
			const auto  field   = [&](uint64_t BdaPrologueCounters::*member) {
                std::vector<uint64_t> values;
                values.reserve(samples.size());
                for (const auto& sample: samples) {
                    values.push_back(sample.*member);
                }
                return mean_of(values);
			};
			prologue_per_loop.entry_writes += field(&BdaPrologueCounters::entry_writes);
			prologue_per_loop.data_fault_pages += field(&BdaPrologueCounters::data_fault_pages);
			prologue_per_loop.prologue_fault_pages +=
			    field(&BdaPrologueCounters::prologue_fault_pages);
		}
		writer_pre_wait_ns_per_loop += mean_of(frame_writer_pre_wait_ns[i]);
		writer_wait_ns_per_loop += mean_of(frame_writer_wait_ns[i]);
		writer_bytes_per_loop += mean_of(frame_writer_bytes[i]);
		writer_skipped_per_loop += mean_of(frame_writer_skipped[i]);
	}

	// Loop 1's own misses: the first touch of a page after the restore is what the prologue table
	// is meant to remove, and it is exactly the loop the statistics exclude.
	BdaPrologueCounters prologue_first_loop;
	for (size_t i = 0; i < frame_count; i++) {
		if (!frame_prologue[i].empty()) {
			prologue_first_loop.entry_writes += frame_prologue[i].front().entry_writes;
			prologue_first_loop.data_fault_pages += frame_prologue[i].front().data_fault_pages;
			prologue_first_loop.prologue_fault_pages +=
			    frame_prologue[i].front().prologue_fault_pages;
		}
	}
	const auto prologue_totals = ReadBdaPrologueCounters();

	// 5. Report. The first loop is a warm-up: pipelines, descriptor sets and history buffers are
	// all cold, so it is excluded from the statistics (docs/frame-replay.md, limits).
	const size_t skip       = loop_ms.size() > 1 ? 1 : 0;
	const auto   loop_stats = Summarize(loop_ms, skip);
	const auto   gpu_stats  = Summarize(gpu_ms, skip);
	std::vector<Stats> frame_loop_stats;
	std::vector<Stats> frame_gpu_stats;
	frame_loop_stats.reserve(frame_count);
	frame_gpu_stats.reserve(frame_count);
	for (size_t i = 0; i < frame_count; i++) {
		frame_loop_stats.push_back(Summarize(frame_loop_ms[i], skip));
		frame_gpu_stats.push_back(Summarize(frame_gpu_ms[i], skip));
	}

	::printf("  loops          %zu (%zu measured, loop 1 excluded), %llu flips presented\n",
	         loop_ms.size(), loop_ms.size() - skip, static_cast<unsigned long long>(presented));
	if (presented == 0) {
		::printf("  WARNING        no frame was presented; ms/loop equals ms/loop gpu\n");
	}
	PrintStats("ms/loop", loop_stats);
	PrintStats("ms/loop gpu", gpu_stats);
	if (writer) {
		::printf("  writer         %.0f us a loop waiting in the frame (+%.0f us in the pre-event "
		         "batch, outside the timing), %.1f MiB rewritten, %llu pages skipped\n",
		         static_cast<double>(writer_wait_ns_per_loop) / 1000.0,
		         static_cast<double>(writer_pre_wait_ns_per_loop) / 1000.0,
		         static_cast<double>(writer_bytes_per_loop) / (1024.0 * 1024.0),
		         static_cast<unsigned long long>(writer_skipped_per_loop));
	}
	if (Config::BdaAsyncProtectEnabled()) {
		::printf("  async protect  render-thread protect calls in the deferred upload path %llu a "
		         "loop (%llu more in written uploads, which the GPU claims anyway); "
		         "helper %llu calls over %llu pages in %llu batches\n",
		         static_cast<unsigned long long>(async_per_loop.upload_protect_calls),
		         static_cast<unsigned long long>(async_per_loop.written_protect_calls),
		         static_cast<unsigned long long>(async_per_loop.helper_protect_calls),
		         static_cast<unsigned long long>(async_per_loop.helper_protect_pages),
		         static_cast<unsigned long long>(async_per_loop.helper_batches));
		::printf("                 %llu pages landed, %llu uploaded a second time; %llu of %llu "
		         "boundary drains waited, %.0f us in all\n",
		         static_cast<unsigned long long>(async_per_loop.landed_pages),
		         static_cast<unsigned long long>(async_per_loop.extra_uploads),
		         static_cast<unsigned long long>(async_per_loop.waits),
		         static_cast<unsigned long long>(async_per_loop.drains),
		         static_cast<double>(async_per_loop.wait_ns) / 1000.0);
		::printf("                 %llu forced scans at those boundaries, %.0f us a loop\n",
		         static_cast<unsigned long long>(async_per_loop.boundary_scans),
		         static_cast<double>(async_per_loop.boundary_scan_ns) / 1000.0);
	}
	::printf("  bda faults     data table %llu pages a loop (%llu in loop 1)\n",
	         static_cast<unsigned long long>(prologue_per_loop.data_fault_pages),
	         static_cast<unsigned long long>(prologue_first_loop.data_fault_pages));
	if (Config::GpuPrologueTableEnabled()) {
		::printf("  prologue table %llu misses a loop (%llu in loop 1), %llu entries written a "
		         "loop\n",
		         static_cast<unsigned long long>(prologue_per_loop.prologue_fault_pages),
		         static_cast<unsigned long long>(prologue_first_loop.prologue_fault_pages),
		         static_cast<unsigned long long>(prologue_per_loop.entry_writes));
		::printf("  guest imports  %llu ranges alive, %.1f MiB, %.0f ms to import; %llu released, "
		         "%llu failed\n",
		         static_cast<unsigned long long>(prologue_totals.import_ranges),
		         static_cast<double>(prologue_totals.import_bytes) / (1024.0 * 1024.0),
		         static_cast<double>(prologue_totals.import_ns) / 1.0e6,
		         static_cast<unsigned long long>(prologue_totals.import_releases),
		         static_cast<unsigned long long>(prologue_totals.import_failures));
	}
	::printf("  drains         %llu a loop of %llu GPU-range syncs (readbacks on the render thread)\n",
	         static_cast<unsigned long long>(drains_per_loop),
	         static_cast<unsigned long long>(syncs_per_loop));
	::printf("  submits        %llu a loop (vkQueueSubmit, graphics timeline); read faults %llu a "
	         "loop, %.2f ms of device wait in them\n",
	         static_cast<unsigned long long>(submits_per_loop),
	         static_cast<unsigned long long>(faults_per_loop),
	         static_cast<double>(fault_wait_per_loop) / 1.0e6);
	for (size_t i = 0; i < frame_count; i++) {
		::printf("  frame %-2zu %-5llu gpu median %8.3f  min %8.3f  max %8.3f | loop median %8.3f",
		         i + 1, static_cast<unsigned long long>(recorded[frame_base + i].number),
		         frame_gpu_stats[i].median, frame_gpu_stats[i].min, frame_gpu_stats[i].max,
		         frame_loop_stats[i].median);
		::printf(" | drains %llu of %llu syncs", static_cast<unsigned long long>(drain_mean[i]),
		         static_cast<unsigned long long>(sync_mean[i]));
		if (use_events) {
			::printf(" | events %zu (%llu late) | progress %u of %u",
			         recorded[frame_base + i].timed_events.size(),
			         static_cast<unsigned long long>(frame_late[i]), frame_progress[i],
			         recorded[frame_base + i].progress);
		}
		::printf("\n");
	}
	for (size_t i = 0; i < frame_count; i++) {
		const auto& replayed = prepare_mean[i];
		const auto& captured = recorded[frame_base + i].recorded_prepares;
		::printf("  frame %-2zu      prepares %llu (%llu recorded), scans %llu (%llu recorded)",
		         i + 1, static_cast<unsigned long long>(replayed.prepares),
		         static_cast<unsigned long long>(captured.prepares),
		         static_cast<unsigned long long>(replayed.scans),
		         static_cast<unsigned long long>(captured.scans));
		if (replayed.scans != 0) {
			const auto scans = static_cast<double>(replayed.scans);
			::printf(", %.1f protects (%.1f pages), %.1f ranges %.1f buffers %.0f KiB %.1f us a scan",
			         static_cast<double>(replayed.protect_calls) / scans,
			         static_cast<double>(replayed.protect_pages) / scans,
			         static_cast<double>(replayed.dirty_ranges) / scans,
			         static_cast<double>(replayed.synchronized) / scans,
			         static_cast<double>(replayed.dirty_bytes) / scans / 1024.0,
			         static_cast<double>(replayed.scan_ns) / scans / 1000.0);
		}
		if (captured.scans != 0) {
			const auto scans = static_cast<double>(captured.scans);
			::printf(" (recorded %.1f %.1f %.0f KiB %.1f us)",
			         static_cast<double>(captured.dirty_ranges) / scans,
			         static_cast<double>(captured.synchronized) / scans,
			         static_cast<double>(captured.dirty_bytes) / scans / 1024.0,
			         static_cast<double>(captured.scan_ns) / scans / 1000.0);
		}
		::printf("\n");
	}
	if (use_events) {
		::printf("  dirty events   %llu late of %zu timed (%.2f%%)\n",
		         static_cast<unsigned long long>(late_events), timed_total * loop_ms.size(),
		         timed_total == 0 || loop_ms.empty()
		             ? 0.0
		             : 100.0 * static_cast<double>(late_events) /
		                   static_cast<double>(timed_total * loop_ms.size()));
	}
	::printf("  total          %.3f s\n", run_ms / 1000.0);
	::fflush(stdout);

	const auto    report_path = dir / "replay-report.json";
	std::ofstream report(report_path, std::ios::binary | std::ios::trunc);
	if (report.is_open()) {
		uint32_t progress_replayed = 0;
		for (const auto value: frame_progress) {
			progress_replayed += value;
		}
		report << "{\n";
		report << "  \"capture\": \"" << dir.generic_string() << "\",\n";
		report << "  \"frame\": " << reader.Manifest().frame << ",\n";
		report << "  \"loops\": " << loop_ms.size() << ",\n";
		report << "  \"warmup_loops\": " << skip << ",\n";
		report << "  \"recorded_frames\": " << recorded.size() << ",\n";
		report << "  \"frames\": " << frame_count << ",\n";
		report << "  \"frame_numbers\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << recorded[frame_base + i].number;
		}
		report << "],\n";
		report << "  \"restore_ranges_ms\": " << ranges_ms << ",\n";
		report << "  \"restore_pages_ms\": " << pages_ms << ",\n";
		report << "  \"restore_pages\": " << pages_written << ",\n";
		report << "  \"restore_bytes\": " << pages_written * kPageSize << ",\n";
		report << "  \"submissions\": " << graphics_count + compute_count + flip_count << ",\n";
		report << "  \"dirty_pages\": " << dirty_pages.size() << ",\n";
		report << "  \"dirty_events\": " << dirty_events.size() << ",\n";
		report << "  \"dirty_events_timed\": " << timed_total << ",\n";
		report << "  \"dirty_events_late\": " << late_events << ",\n";
		report << "  \"progress_events_captured\": " << progress_recorded << ",\n";
		report << "  \"progress_events_replayed\": " << progress_replayed << ",\n";
		// Per frame, one entry per replayed frame: the samples, their statistics and what the
		// frame's CPU-write replay did.
		report << "  \"frame_gpu_ms\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << JsonSamples(frame_gpu_ms[i]);
		}
		report << "],\n";
		report << "  \"frame_loop_ms\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << JsonSamples(frame_loop_ms[i]);
		}
		report << "],\n";
		report << "  \"frame_gpu_summary\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << JsonStats(frame_gpu_stats[i]);
		}
		report << "],\n";
		report << "  \"frame_summary\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << JsonStats(frame_loop_stats[i]);
		}
		report << "],\n";
		report << "  \"bda_async_protect\": "
		       << (Config::BdaAsyncProtectEnabled() ? "true" : "false") << ",\n";
		report << "  \"async_upload_protect_calls\": " << async_per_loop.upload_protect_calls
		       << ",\n";
		report << "  \"async_upload_protect_pages\": " << async_per_loop.upload_protect_pages
		       << ",\n";
		report << "  \"async_written_protect_calls\": " << async_per_loop.written_protect_calls
		       << ",\n";
		report << "  \"async_written_protect_pages\": " << async_per_loop.written_protect_pages
		       << ",\n";
		report << "  \"async_helper_protect_calls\": " << async_per_loop.helper_protect_calls
		       << ",\n";
		report << "  \"async_helper_protect_pages\": " << async_per_loop.helper_protect_pages
		       << ",\n";
		report << "  \"async_helper_batches\": " << async_per_loop.helper_batches << ",\n";
		report << "  \"async_landed_pages\": " << async_per_loop.landed_pages << ",\n";
		report << "  \"async_extra_uploads\": " << async_per_loop.extra_uploads << ",\n";
		report << "  \"async_boundary_drains\": " << async_per_loop.drains << ",\n";
		report << "  \"async_boundary_waits\": " << async_per_loop.waits << ",\n";
		report << "  \"async_boundary_wait_us\": " << async_per_loop.wait_ns / 1000 << ",\n";
		report << "  \"async_boundary_scans\": " << async_per_loop.boundary_scans << ",\n";
		report << "  \"async_boundary_scan_us\": " << async_per_loop.boundary_scan_ns / 1000
		       << ",\n";
		report << "  \"gpu_prologue_table\": "
		       << (Config::GpuPrologueTableEnabled() ? "true" : "false") << ",\n";
		report << "  \"bda_data_fault_pages_per_loop\": " << prologue_per_loop.data_fault_pages
		       << ",\n";
		report << "  \"bda_prologue_fault_pages_per_loop\": "
		       << prologue_per_loop.prologue_fault_pages << ",\n";
		report << "  \"bda_data_fault_pages_loop1\": " << prologue_first_loop.data_fault_pages
		       << ",\n";
		report << "  \"bda_prologue_fault_pages_loop1\": "
		       << prologue_first_loop.prologue_fault_pages << ",\n";
		report << "  \"bda_prologue_entry_writes_per_loop\": " << prologue_per_loop.entry_writes
		       << ",\n";
		report << "  \"guest_import_ranges\": " << prologue_totals.import_ranges << ",\n";
		report << "  \"guest_import_bytes\": " << prologue_totals.import_bytes << ",\n";
		report << "  \"guest_import_us\": " << prologue_totals.import_ns / 1000 << ",\n";
		report << "  \"guest_import_releases\": " << prologue_totals.import_releases << ",\n";
		report << "  \"guest_import_failures\": " << prologue_totals.import_failures << ",\n";
		report << "  \"drains_per_loop\": " << drains_per_loop << ",\n";
		report << "  \"syncs_per_loop\": " << syncs_per_loop << ",\n";
		report << "  \"submits_per_loop\": " << submits_per_loop << ",\n";
		report << "  \"read_faults_per_loop\": " << faults_per_loop << ",\n";
		report << "  \"read_fault_wait_ms_per_loop\": "
		       << static_cast<double>(fault_wait_per_loop) / 1.0e6 << ",\n";
		report << "  \"gpu_submit_interval\": " << Config::GetGpuSubmitInterval() << ",\n";
		report << "  \"gpu_submit_after_writes\": "
		       << (Config::GpuSubmitAfterWritesEnabled() ? "true" : "false") << ",\n";
		report << "  \"frame_drains\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << drain_mean[i];
		}
		report << "],\n";
		report << "  \"frame_dirty_events_timed\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << recorded[frame_base + i].timed_events.size();
		}
		report << "],\n";
		report << "  \"frame_dirty_events_late\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << frame_late[i];
		}
		report << "],\n";
		report << "  \"frame_progress_captured\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << recorded[frame_base + i].progress;
		}
		report << "],\n";
		report << "  \"frame_prepares\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << prepare_mean[i].prepares;
		}
		report << "],\n";
		report << "  \"frame_scans\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << prepare_mean[i].scans;
		}
		report << "],\n";
		report << "  \"frame_scan_dirty_ranges\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << prepare_mean[i].dirty_ranges;
		}
		report << "],\n";
		report << "  \"dirty_set_once\": " << (dirty_set_once ? "true" : "false") << ",\n";
		report << "  \"spin_threads\": " << spin_threads << ",\n";
		report << "  \"writer_mode\": \""
		       << (writer_mode == Config::ReplayWriter::Guest     ? "guest"
		           : writer_mode == Config::ReplayWriter::Render  ? "render"
		                                                          : "none")
		       << "\",\n";
		report << "  \"writer_wait_us\": " << writer_wait_ns_per_loop / 1000 << ",\n";
		report << "  \"writer_pre_wait_us\": " << writer_pre_wait_ns_per_loop / 1000 << ",\n";
		report << "  \"writer_bytes\": " << writer_bytes_per_loop << ",\n";
		report << "  \"writer_skipped_pages\": " << writer_skipped_per_loop << ",\n";
		report << "  \"frame_scan_protect_calls\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << prepare_mean[i].protect_calls;
		}
		report << "],\n";
		report << "  \"frame_scan_protect_pages\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << prepare_mean[i].protect_pages;
		}
		report << "],\n";
		report << "  \"frame_scan_ns\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << prepare_mean[i].scan_ns;
		}
		report << "],\n";
		report << "  \"frame_scan_bytes\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << prepare_mean[i].dirty_bytes;
		}
		report << "],\n";
		report << "  \"frame_scan_ns_recorded\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << recorded[frame_base + i].recorded_prepares.scan_ns;
		}
		report << "],\n";
		report << "  \"frame_scan_bytes_recorded\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << recorded[frame_base + i].recorded_prepares.dirty_bytes;
		}
		report << "],\n";
		report << "  \"frame_scan_synchronized\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << prepare_mean[i].synchronized;
		}
		report << "],\n";
		report << "  \"frame_prepares_recorded\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << recorded[frame_base + i].recorded_prepares.prepares;
		}
		report << "],\n";
		report << "  \"frame_scans_recorded\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << recorded[frame_base + i].recorded_prepares.scans;
		}
		report << "],\n";
		report << "  \"frame_scan_dirty_ranges_recorded\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",")
			       << recorded[frame_base + i].recorded_prepares.dirty_ranges;
		}
		report << "],\n";
		report << "  \"frame_scan_synchronized_recorded\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",")
			       << recorded[frame_base + i].recorded_prepares.synchronized;
		}
		report << "],\n";
		report << "  \"frame_progress_replayed\": [";
		for (size_t i = 0; i < frame_count; i++) {
			report << (i == 0 ? "" : ",") << frame_progress[i];
		}
		report << "],\n";
		report << "  \"loop_ms\": " << JsonSamples(loop_ms) << ",\n";
		report << "  \"gpu_ms\": " << JsonSamples(gpu_ms) << ",\n";
		report << "  \"summary\": " << JsonStats(loop_stats) << ",\n";
		report << "  \"gpu_summary\": " << JsonStats(gpu_stats) << "\n";
		report << "}\n";
		report.close();
		::printf("  report         %s\n", report_path.string().c_str());
	} else {
		::printf("  report         could not write %s\n", report_path.string().c_str());
	}

	// Item 1b (docs/performance-roadmap.md): the render thread read faults, one line and one
	// JSON file beside the report. Inert without --gpu-readback-diagnostics.
	ReadbackDiag::Dump(dir / "readback-faults.json");

	// 6. Presented images: one per frame of the last loop, plus <path> itself for the last of
	// them, which is what --replay-image wrote when a capture held a single frame.
	if (!image.empty()) {
		if (frame_images.empty()) {
			::printf("  image          not available: the presenter has no last frame\n");
		}
		for (size_t i = 0; i < frame_images.size(); i++) {
			const auto& readback = frame_images[i];
			if (readback.pixels.empty()) {
				::printf("  image          frame %zu not available\n", i + 1);
				continue;
			}
			const auto path = FrameImagePath(image, i);
			if (!WritePresentedImage(path, readback)) {
				::printf("  image          could not write %s\n", path.string().c_str());
				continue;
			}
			::printf("  image          %s (%ux%u %s) + %s.json\n", path.string().c_str(),
			         readback.width, readback.height, readback.format.c_str(),
			         path.filename().string().c_str());
			if (i + 1 == frame_images.size() && !WritePresentedImage(image, readback)) {
				::printf("  image          could not write %s\n", image.string().c_str());
			}
		}
	}

	// WindowRun saves the pipeline cache when the guest path shuts down; the replay ends with
	// quick_exit, so it saves here and the next replay of the same capture starts warm.
	renderer->GetPipelineCache().Save();

	::fflush(stdout);
	return 0;
}

} // namespace Libs::Graphics::Replay
