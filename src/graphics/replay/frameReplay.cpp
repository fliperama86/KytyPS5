#include "graphics/replay/frameReplay.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/virtualMemory.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/presentation/window.h"
#include "graphics/replay/frameCaptureReader.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "libs/agc.h"
#include "loader/runtimeLinker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
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

int RunReplay(const std::filesystem::path& dir, uint32_t loops,
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

	// 2b. Shader map. Guest code created every shader before the capture; the replay recovers the
	// registrations from the headers the game left in guest memory.
	const auto shaders_start = Clock::now();
	const auto shaders       = RestoreShaderMap(restored);
	::printf("  shaders        %llu registrations recovered, %.0f ms\n",
	         static_cast<unsigned long long>(shaders), MillisSince(shaders_start));
	if (shaders == 0) {
		::printf("  WARNING        no shader headers were found; every draw will fail\n");
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

	// 3c. Submissions. The dwords stay in these vectors for the whole run: GuestGpu borrows the
	// spans and the GPU thread reads them long after the feeder moved on.
	std::vector<CaptureSubmission> submissions;
	if (!reader.ReadSubmissions(&submissions, &error)) {
		Fail(error);
		return 1;
	}
	size_t graphics_count = 0;
	size_t compute_count  = 0;
	size_t flip_count     = 0;
	size_t done_count     = 0;
	for (const auto& submission: submissions) {
		switch (static_cast<SubmissionKind>(submission.header.kind)) {
			case SubmissionKind::Graphics: graphics_count++; break;
			case SubmissionKind::Compute: compute_count++; break;
			case SubmissionKind::FlipPreparation: flip_count++; break;
			case SubmissionKind::Done: done_count++; break;
		}
	}
	if (done_count == 0) {
		Fail("submissions.bin has no Done record, so the frame has no end");
		return 1;
	}

	// 3d. Dirty pages.
	std::vector<uint64_t> dirty_pages;
	if (!reader.ReadDirtyPages(&dirty_pages, &error)) {
		Fail(error);
		return 1;
	}
	const auto dirty_ranges = CoalescePages(dirty_pages);

	::printf("  registers      %zu command processors, %zu bytes each\n", register_files.size(),
	         expected_register_size);
	::printf("  video-out      %zu registrations, handle %d, flip index %d\n", video_out.size(),
	         handle, flip_index);
	::printf("  submissions    %zu records (%zu graphics, %zu compute, %zu flips)\n",
	         submissions.size(), graphics_count, compute_count, flip_count);
	::printf("  dirty pages    %zu in %zu ranges\n", dirty_pages.size(), dirty_ranges.size());
	::fflush(stdout);

	// 4. Loop.
	g_wait_diagnostics.store(true, std::memory_order_relaxed);
	auto&               buffer_cache = renderer->GetBufferCache();
	uint64_t            presented    = 0;
	uint64_t            flips_before = VideoOut::VideoOutReplayFlipCount(handle);
	std::vector<double> loop_ms;
	std::vector<double> gpu_ms;
	loop_ms.reserve(loops);
	gpu_ms.reserve(loops);
	const auto run_start = Clock::now();

	for (uint32_t loop = 0; loop < loops; loop++) {
		if (!dirty_ranges.empty()) {
			gpu.SendCommandSync([&]() {
				for (const auto& range: dirty_ranges) {
					buffer_cache.MarkRegionAsCpuModified(range.start, range.size);
				}
			});
		}

		const auto loop_start = Clock::now();
		for (const auto& submission: submissions) {
			switch (static_cast<SubmissionKind>(submission.header.kind)) {
				case SubmissionKind::Graphics:
					gpu.Submit(submission.commands, submission.constants);
					break;
				case SubmissionKind::Compute:
					gpu.SubmitCompute(submission.header.queue_id, submission.commands);
					break;
				case SubmissionKind::FlipPreparation: {
					// The recorded request id belongs to the captured run; the replay reserves its
					// own flip through the path VideoOutSubmitFlip takes.
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
			Fail("the GPU thread did not finish loop " + std::to_string(loop + 1) + " within " +
			     std::to_string(timeout) + " ms: " + DescribeLastBlockedWait());
			return 1;
		}
		gpu.Done();
		const auto gpu_done = MillisSince(loop_start);
		// The title flips from its own command stream, so there is no recorded buffer index to
		// wait on: the loop ends when every flip queued on the port has been presented.
		if (!VideoOut::VideoOutReplayWaitFlipsDrained(handle, FLIP_TIMEOUT_MS)) {
			g_wait_diagnostics.store(false, std::memory_order_relaxed);
			Fail("the flip queued by loop " + std::to_string(loop + 1) +
			     " was not presented within " + std::to_string(FLIP_TIMEOUT_MS) + " ms");
			return 1;
		}
		presented += VideoOut::VideoOutReplayFlipCount(handle) - flips_before;
		flips_before = VideoOut::VideoOutReplayFlipCount(handle);
		loop_ms.push_back(MillisSince(loop_start));
		gpu_ms.push_back(gpu_done);
	}
	g_wait_diagnostics.store(false, std::memory_order_relaxed);
	const auto run_ms = MillisSince(run_start);

	// 5. Report. The first loop is a warm-up: pipelines, descriptor sets and history buffers are
	// all cold, so it is excluded from the statistics (docs/frame-replay.md, limits).
	const size_t skip       = loop_ms.size() > 1 ? 1 : 0;
	const auto   loop_stats = Summarize(loop_ms, skip);
	const auto   gpu_stats  = Summarize(gpu_ms, skip);

	::printf("  loops          %zu (%zu measured, loop 1 excluded), %llu flips presented\n",
	         loop_ms.size(), loop_ms.size() - skip, static_cast<unsigned long long>(presented));
	if (presented == 0) {
		::printf("  WARNING        no frame was presented; ms/loop equals ms/loop gpu\n");
	}
	PrintStats("ms/loop", loop_stats);
	PrintStats("ms/loop gpu", gpu_stats);
	::printf("  total          %.3f s\n", run_ms / 1000.0);
	::fflush(stdout);

	const auto    report_path = dir / "replay-report.json";
	std::ofstream report(report_path, std::ios::binary | std::ios::trunc);
	if (report.is_open()) {
		report << "{\n";
		report << "  \"capture\": \"" << dir.generic_string() << "\",\n";
		report << "  \"frame\": " << reader.Manifest().frame << ",\n";
		report << "  \"loops\": " << loop_ms.size() << ",\n";
		report << "  \"warmup_loops\": " << skip << ",\n";
		report << "  \"restore_ranges_ms\": " << ranges_ms << ",\n";
		report << "  \"restore_pages_ms\": " << pages_ms << ",\n";
		report << "  \"restore_pages\": " << pages_written << ",\n";
		report << "  \"restore_bytes\": " << pages_written * kPageSize << ",\n";
		report << "  \"submissions\": " << submissions.size() << ",\n";
		report << "  \"dirty_pages\": " << dirty_pages.size() << ",\n";
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

	// 6. Presented image.
	if (!image.empty()) {
		auto*          presenter = WindowGetPresenter();
		PresentedImage readback;
		if (presenter == nullptr || !presenter->ReadLastPresentedFrame(&readback)) {
			::printf("  image          not available: the presenter has no last frame\n");
		} else {
			std::ofstream raw(image, std::ios::binary | std::ios::trunc);
			raw.write(reinterpret_cast<const char*>(readback.pixels.data()),
			          static_cast<std::streamsize>(readback.pixels.size()));
			raw.close();
			auto sidecar_path = image;
			sidecar_path += ".json";
			std::ofstream sidecar(sidecar_path, std::ios::binary | std::ios::trunc);
			sidecar << "{\"width\":" << readback.width << ",\"height\":" << readback.height
			        << ",\"format\":\"" << readback.format
			        << "\",\"bytes_per_pixel\":4,\"stride\":" << readback.width * 4u << "}\n";
			sidecar.close();
			::printf("  image          %s (%ux%u %s) + %s\n", image.string().c_str(),
			         readback.width, readback.height, readback.format.c_str(),
			         sidecar_path.filename().string().c_str());
		}
	}

	// WindowRun saves the pipeline cache when the guest path shuts down; the replay ends with
	// quick_exit, so it saves here and the next replay of the same capture starts warm.
	renderer->GetPipelineCache().Save();

	::fflush(stdout);
	return 0;
}

} // namespace Libs::Graphics::Replay
