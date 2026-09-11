#include "graphics/replay/frameCapture.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/presentation/videoOut.h"
#include "graphics/shader/shader.h"
#include "kernel/memory.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::Replay {

namespace {

// Guest pages that cannot be read back are listed in the manifest, coalesced; the list is bounded
// so a capture of a mostly unreadable address space still produces a readable manifest.
constexpr size_t   MAX_REPORTED_GAPS = 4096;
constexpr uint64_t READ_CHUNK        = 1024 * 1024;
constexpr size_t   WRITE_BUFFER_SIZE = 8 * 1024 * 1024;

struct Gap {
	uint64_t    vaddr = 0;
	uint64_t    size  = 0;
	std::string reason;
};

struct PendingSubmission {
	SubmissionRecord      header;
	std::vector<uint32_t> commands;
	std::vector<uint32_t> constants;
};

struct Recorder {
	std::mutex                                      mutex;
	std::unordered_map<uint64_t, PendingSubmission> pending;
	std::vector<PendingSubmission>                  ordered;
	uint64_t                                        next_id = 1;
};

struct State {
	bool                  armed      = false;
	int                   capture_at = -1;
	bool                  exit_after = true;
	std::filesystem::path folder;
	std::atomic_bool      active {false};
	Recorder              recorder;
};

State& Instance() {
	static State state;
	static bool  loaded = [&] {
		state.armed = Config::FrameCaptureEnabled();
		if (state.armed) {
			state.folder     = Config::GetFrameCaptureFolder();
			state.capture_at = Config::GetFrameCaptureFrame();
			state.exit_after = Config::FrameCaptureExitEnabled();
			state.active.store(true, std::memory_order_relaxed);
			LOGF("FrameCapture: armed, folder=%s frame=%d exit=%s\n", state.folder.string().c_str(),
			     state.capture_at, state.exit_after ? "true" : "false");
		}
		return true;
	}();
	(void)loaded;
	return state;
}

// A plain buffered writer: memory.bin is measured in gigabytes and every other file is tiny.
class Writer final {
public:
	Writer() { m_buffer.reserve(WRITE_BUFFER_SIZE); }
	~Writer() { Close(); }
	KYTY_CLASS_NO_COPY(Writer);

	bool Open(const std::filesystem::path& path) {
		Close();
		m_file = std::fopen(path.string().c_str(), "wb");
		m_ok   = m_file != nullptr;
		return m_ok;
	}

	void Write(const void* data, size_t size) {
		if (!m_ok || size == 0) {
			return;
		}
		if (m_buffer.size() + size > WRITE_BUFFER_SIZE) {
			FlushBuffer();
		}
		if (size >= WRITE_BUFFER_SIZE) {
			m_ok = m_ok && std::fwrite(data, 1, size, m_file) == size;
			m_written += size;
			return;
		}
		const auto* bytes = static_cast<const uint8_t*>(data);
		m_buffer.insert(m_buffer.end(), bytes, bytes + size);
	}

	bool Close() {
		if (m_file == nullptr) {
			return m_ok;
		}
		FlushBuffer();
		m_ok   = (std::fclose(m_file) == 0) && m_ok;
		m_file = nullptr;
		return m_ok;
	}

	[[nodiscard]] bool     Ok() const noexcept { return m_ok; }
	[[nodiscard]] uint64_t Written() const noexcept { return m_written + m_buffer.size(); }

private:
	void FlushBuffer() {
		if (m_buffer.empty()) {
			return;
		}
		m_ok = m_ok && std::fwrite(m_buffer.data(), 1, m_buffer.size(), m_file) == m_buffer.size();
		m_written += m_buffer.size();
		m_buffer.clear();
	}

	std::FILE*           m_file = nullptr;
	std::vector<uint8_t> m_buffer;
	uint64_t             m_written = 0;
	bool                 m_ok      = false;
};

bool IsZero(const uint8_t* data, size_t size) {
	static const std::vector<uint8_t> zero(kPageSize, 0);
	return std::memcmp(data, zero.data(), size) == 0;
}

void AddGap(std::vector<Gap>& gaps, uint64_t& dropped, uint64_t vaddr, uint64_t size,
            const char* reason) {
	if (!gaps.empty() && gaps.back().vaddr + gaps.back().size == vaddr &&
	    gaps.back().reason == reason) {
		gaps.back().size += size;
		return;
	}
	if (gaps.size() >= MAX_REPORTED_GAPS) {
		dropped++;
		return;
	}
	gaps.push_back({vaddr, size, reason});
}

std::string JsonEscape(const std::string& value) {
	std::string out;
	out.reserve(value.size() + 8);
	for (const char c: value) {
		if (c == '"' || c == '\\') {
			out += '\\';
			out += c;
		} else if (static_cast<unsigned char>(c) < 0x20) {
			out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
		} else {
			out += c;
		}
	}
	return out;
}

struct MemoryStats {
	uint64_t ranges       = 0;
	uint64_t committed    = 0;
	uint64_t pages        = 0;
	uint64_t bytes        = 0;
	uint64_t skipped_zero = 0;
};

// Reads [vaddr, vaddr + size) into `out`. The guest is mapped at the same host address, but the
// GPU page manager can hold any page at PAGE_NOACCESS, so the backing-store alias is tried first
// and the direct read only behind an explicit readability probe.
bool ReadGuest(uint64_t vaddr, uint64_t size, uint8_t* out) {
	if (LibKernel::Memory::TryReadBacking(vaddr, out, size)) {
		return true;
	}
	if (HostMemoryRangeIsReadable(vaddr, size)) {
		std::memcpy(out, reinterpret_cast<const void*>(vaddr), size);
		return true;
	}
	return false;
}

MemoryStats WriteMemory(const std::vector<LibKernel::Memory::VirtualRangeSnapshot>& ranges,
                        const std::filesystem::path& folder, std::vector<Gap>& gaps,
                        uint64_t& dropped_gaps, bool& ok) {
	MemoryStats stats;

	Writer ranges_file;
	Writer memory_file;
	if (!ranges_file.Open(folder / "ranges.bin") || !memory_file.Open(folder / "memory.bin")) {
		LOGF("FrameCapture: cannot create ranges.bin or memory.bin\n");
		ok = false;
		return stats;
	}

	std::vector<uint8_t> chunk(READ_CHUNK);
	for (const auto& range: ranges) {
		RangeRecord record {};
		record.vaddr = range.start;
		record.size  = range.size;
		record.prot  = static_cast<uint32_t>(range.protection);
		record.type  = range.type;
		std::memcpy(record.name, range.name, sizeof(record.name));
		ranges_file.Write(&record, sizeof(record));
		stats.ranges++;

		if (!range.is_committed || range.size == 0) {
			continue;
		}
		stats.committed++;

		const auto begin = range.start & ~(kPageSize - 1);
		const auto end   = (range.start + range.size + kPageSize - 1) & ~(kPageSize - 1);
		for (auto cursor = begin; cursor < end; cursor += READ_CHUNK) {
			const auto bytes = std::min<uint64_t>(READ_CHUNK, end - cursor);
			if (!ReadGuest(cursor, bytes, chunk.data())) {
				// Fall back to per-page reads so one protected page does not lose a megabyte.
				for (uint64_t page = cursor; page < cursor + bytes; page += kPageSize) {
					if (!ReadGuest(page, kPageSize, chunk.data())) {
						AddGap(gaps, dropped_gaps, page, kPageSize, "page not readable");
						continue;
					}
					if (IsZero(chunk.data(), kPageSize)) {
						stats.skipped_zero++;
						continue;
					}
					memory_file.Write(&page, sizeof(page));
					memory_file.Write(chunk.data(), kPageSize);
					stats.pages++;
					stats.bytes += kPageSize;
				}
				continue;
			}
			for (uint64_t offset = 0; offset < bytes; offset += kPageSize) {
				const auto page = cursor + offset;
				if (IsZero(chunk.data() + offset, kPageSize)) {
					stats.skipped_zero++;
					continue;
				}
				memory_file.Write(&page, sizeof(page));
				memory_file.Write(chunk.data() + offset, kPageSize);
				stats.pages++;
				stats.bytes += kPageSize;
			}
		}
	}

	ok = ranges_file.Close() && ok;
	ok = memory_file.Close() && ok;
	return stats;
}

uint64_t WriteDirtyPages(RenderContext&                                              renderer,
                         const std::vector<LibKernel::Memory::VirtualRangeSnapshot>& ranges,
                         const std::filesystem::path& folder, bool& ok) {
	Writer file;
	if (!file.Open(folder / "dirty-pages.bin")) {
		LOGF("FrameCapture: cannot create dirty-pages.bin\n");
		ok = false;
		return 0;
	}

	uint64_t count = 0;
	auto&    cache = renderer.GetBufferCache();

	for (const auto& range: ranges) {
		if (!range.is_committed || range.size == 0 ||
		    !GuestRange {range.start, range.size}.Valid()) {
			continue;
		}
		uint64_t last = UINT64_MAX;
		cache.ForEachCpuModifiedRange(
		    range.start, range.size, [&](uint64_t address, uint64_t size) noexcept {
			    const auto begin = address & ~(kPageSize - 1);
			    const auto end   = (address + size + kPageSize - 1) & ~(kPageSize - 1);
			    for (auto page = begin; page < end; page += kPageSize) {
				    if (page == last) {
					    continue;
				    }
				    last = page;
				    file.Write(&page, sizeof(page));
				    count++;
			    }
		    });
	}

	ok = file.Close() && ok;
	return count;
}

void WriteRegisters(std::span<const ProcessorRegisters> processors,
                    const std::filesystem::path& folder, bool& ok) {
	static_assert(std::is_trivially_copyable_v<HW::Context>);
	static_assert(std::is_trivially_copyable_v<HW::UserConfig>);
	static_assert(std::is_trivially_copyable_v<HW::Shader>);

	Writer file;
	if (!file.Open(folder / "registers.bin")) {
		LOGF("FrameCapture: cannot create registers.bin\n");
		ok = false;
		return;
	}

	for (const auto& processor: processors) {
		RegisterFileRecord record {};
		record.queue_id = processor.queue_id;
		record.size     = static_cast<uint32_t>(sizeof(HW::Context) + sizeof(HW::UserConfig) +
		                                        sizeof(HW::Shader));
		file.Write(&record, sizeof(record));
		file.Write(processor.context, sizeof(HW::Context));
		file.Write(processor.config, sizeof(HW::UserConfig));
		file.Write(processor.shader, sizeof(HW::Shader));
	}

	ok = file.Close() && ok;
}

uint32_t WriteVideoOut(const std::filesystem::path& folder, uint32_t& width, uint32_t& height,
                       bool& ok) {
	Writer file;
	if (!file.Open(folder / "videoout.bin")) {
		LOGF("FrameCapture: cannot create videoout.bin\n");
		ok = false;
		return 0;
	}

	const auto registrations = VideoOut::VideoOutSnapshotRegistrations();
	for (const auto& entry: registrations) {
		VideoOutRecord record {};
		record.handle         = entry.handle;
		record.set_index      = entry.set_index;
		record.index_start    = entry.index_start;
		record.count          = entry.count;
		record.category       = entry.category;
		record.attribute_size = entry.attribute_size;
		file.Write(&record, sizeof(record));
		file.Write(entry.attribute.data(), entry.attribute_size);
		for (int i = 0; i < entry.count; i++) {
			const uint64_t addresses[2] = {entry.buffers[static_cast<size_t>(i)].first,
			                               entry.buffers[static_cast<size_t>(i)].second};
			file.Write(addresses, sizeof(addresses));
		}
		if (width == 0 && entry.width != 0) {
			width  = entry.width;
			height = entry.height;
		}
	}

	ok = file.Close() && ok;
	return static_cast<uint32_t>(registrations.size());
}

// The partially-resident-texture apertures. Without them a replay cannot read a partly
// resident image at all: Memory::TryReadPrtBacking refuses outside an aperture.
uint32_t WritePrtApertures(const std::filesystem::path& folder, bool& ok) {
	Writer file;
	if (!file.Open(folder / "prt.bin")) {
		LOGF("FrameCapture: cannot create prt.bin\n");
		ok = false;
		return 0;
	}

	const auto apertures = LibKernel::Memory::SnapshotPrtApertures();
	for (const auto& aperture: apertures) {
		PrtApertureRecord record {};
		record.index   = aperture.index;
		record.address = aperture.address;
		record.size    = aperture.size;
		file.Write(&record, sizeof(record));
	}

	ok = file.Close() && ok;
	return static_cast<uint32_t>(apertures.size());
}

// The AGC shader map. Guest code built it through sceAgcCreateShader before the capture; a
// replay runs no guest code, so every draw and dispatch would fail to resolve its shader.
uint64_t WriteShaders(const std::filesystem::path& folder, bool& ok) {
	Writer file;
	if (!file.Open(folder / "shaders.bin")) {
		LOGF("FrameCapture: cannot create shaders.bin\n");
		ok = false;
		return 0;
	}

	const auto entries = ShaderSnapshotMap();
	for (const auto& entry: entries) {
		ShaderRecord record {};
		record.code_address        = entry.code_address;
		record.user_data           = reinterpret_cast<uint64_t>(entry.data.user_data);
		record.input_semantics     = reinterpret_cast<uint64_t>(entry.data.input_semantics);
		record.num_input_semantics = entry.data.num_input_semantics;
		record.code_size_bytes     = entry.data.code_size_bytes;
		record.scratch_size_dwords = entry.data.scratch_size_dwords;
		record.type                = static_cast<uint32_t>(entry.data.type);
		file.Write(&record, sizeof(record));
	}

	ok = file.Close() && ok;
	return static_cast<uint64_t>(entries.size());
}

uint64_t WriteSubmissions(const std::filesystem::path& folder, bool& ok) {
	Writer file;
	if (!file.Open(folder / "submissions.bin")) {
		LOGF("FrameCapture: cannot create submissions.bin\n");
		ok = false;
		return 0;
	}

	auto&            recorder = Instance().recorder;
	std::unique_lock lock(recorder.mutex);
	for (const auto& submission: recorder.ordered) {
		file.Write(&submission.header, sizeof(submission.header));
		file.Write(submission.commands.data(), submission.commands.size() * sizeof(uint32_t));
		file.Write(submission.constants.data(), submission.constants.size() * sizeof(uint32_t));
	}
	const auto count = static_cast<uint64_t>(recorder.ordered.size());
	lock.unlock();

	ok = file.Close() && ok;
	return count;
}

std::string TitleId() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		title_id = "UNKNOWN";
	}
	return title_id;
}

} // namespace

bool Armed() noexcept {
	return Instance().active.load(std::memory_order_relaxed);
}

bool ExitAfterCapture() noexcept {
	return Instance().exit_after;
}

bool RecordingFrame(int frame_num) noexcept {
	auto& state = Instance();
	if (!state.active.load(std::memory_order_relaxed)) {
		return false;
	}
	return state.capture_at < 0 || frame_num >= state.capture_at;
}

uint64_t RecordEnqueue(SubmissionKind kind, uint32_t queue_id, std::span<const uint32_t> commands,
                       std::span<const uint32_t> constants, uint64_t flip_request_id) {
	auto&            recorder = Instance().recorder;
	std::scoped_lock lock(recorder.mutex);

	PendingSubmission pending;
	pending.header.kind            = static_cast<uint32_t>(kind);
	pending.header.queue_id        = queue_id;
	pending.header.flip_request_id = flip_request_id;
	pending.header.command_dwords  = static_cast<uint32_t>(commands.size());
	pending.header.constant_dwords = static_cast<uint32_t>(constants.size());
	pending.commands.assign(commands.begin(), commands.end());
	pending.constants.assign(constants.begin(), constants.end());

	const auto id = recorder.next_id++;
	recorder.pending.emplace(id, std::move(pending));
	return id;
}

void RecordStarted(uint64_t capture_id) {
	auto&            recorder = Instance().recorder;
	std::scoped_lock lock(recorder.mutex);

	const auto it = recorder.pending.find(capture_id);
	if (it == recorder.pending.end()) {
		return;
	}
	recorder.ordered.push_back(std::move(it->second));
	recorder.pending.erase(it);
}

void RecordDone() {
	auto&            recorder = Instance().recorder;
	std::scoped_lock lock(recorder.mutex);

	PendingSubmission pending;
	pending.header.kind = static_cast<uint32_t>(SubmissionKind::Done);
	recorder.ordered.push_back(std::move(pending));
}

void ResetFrame() noexcept {
	auto&            recorder = Instance().recorder;
	std::scoped_lock lock(recorder.mutex);
	recorder.ordered.clear();
	recorder.pending.clear();
}

bool ShouldCapture(int frame_num) {
	auto& state = Instance();
	if (!state.active.load(std::memory_order_relaxed)) {
		return false;
	}
	if (state.capture_at >= 0) {
		return frame_num == state.capture_at;
	}

	std::error_code error;
	return std::filesystem::exists(state.folder / "trigger", error);
}

bool WriteCapture(RenderContext& renderer, int frame_num,
                  std::span<const ProcessorRegisters> processors) {
	auto& state = Instance();
	// One capture per run: disarm before anything can fail, so a broken capture does not retry
	// every frame.
	state.active.store(false, std::memory_order_relaxed);

	const auto  started = std::chrono::steady_clock::now();
	const auto& folder  = state.folder;

	std::error_code error;
	std::filesystem::create_directories(folder, error);
	if (!std::filesystem::is_directory(folder, error)) {
		LOGF("FrameCapture: cannot create %s\n", folder.string().c_str());
		return false;
	}

	std::vector<Gap> gaps;
	uint64_t         dropped_gaps = 0;

	// 1. Flush what the GPU owns back into guest memory: buffers first, then images, which are the
	// authority for the ranges they own.
	const auto flushed_bytes = renderer.GetBufferCache().FlushGpuModifiedMemory();

	std::vector<TextureCache::CaptureGap> image_gaps;
	const auto flushed_images = renderer.GetTextureCache().FlushGpuModifiedImages(image_gaps);

	auto&      scheduler       = renderer.GetCommandScheduler();
	const auto completion_tick = scheduler.CurrentTick();
	scheduler.EndRendering();
	scheduler.Finish();
	scheduler.WaitPriorityOperations(completion_tick);

	for (const auto& gap: image_gaps) {
		if (gaps.size() >= MAX_REPORTED_GAPS) {
			dropped_gaps++;
			continue;
		}
		gaps.push_back({gap.address, gap.size, gap.reason});
	}

	bool ok = true;

	// 2. Guest memory.
	const auto ranges = LibKernel::Memory::SnapshotVirtualRanges();
	const auto memory = WriteMemory(ranges, folder, gaps, dropped_gaps, ok);
	const auto memory_seconds =
	    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

	// 3. Everything else.
	const auto dirty_pages = WriteDirtyPages(renderer, ranges, folder, ok);
	uint32_t   width       = 0;
	uint32_t   height      = 0;
	const auto video_out   = WriteVideoOut(folder, width, height, ok);
	WriteRegisters(processors, folder, ok);
	const auto submissions = WriteSubmissions(folder, ok);
	const auto apertures   = WritePrtApertures(folder, ok);
	const auto shaders     = WriteShaders(folder, ok);

	if (width == 0) {
		width  = Config::GetScreenWidth();
		height = Config::GetScreenHeight();
	}

	// 4. Manifest. No presenter readback path exists yet, so there is no frame.png or frame.raw;
	// docs/frame-replay.md records that as an open item for phase B.
	std::string out;
	out.reserve(4096 + gaps.size() * 96);
	out += "{\n";
	out += fmt::format("\t\"format_version\": {},\n", kFormatVersion);
	out += fmt::format("\t\"title_id\": \"{}\",\n", JsonEscape(TitleId()));
	out += fmt::format("\t\"commit\": \"{}\",\n", JsonEscape(KYTY_GIT_VERSION));
	out += fmt::format("\t\"frame\": {},\n", frame_num);
	out += fmt::format("\t\"width\": {},\n", width);
	out += fmt::format("\t\"height\": {},\n", height);
	out += fmt::format("\t\"ranges\": {},\n", memory.ranges);
	out += fmt::format("\t\"committed_ranges\": {},\n", memory.committed);
	out += fmt::format("\t\"pages\": {},\n", memory.pages);
	out += fmt::format("\t\"zero_pages\": {},\n", memory.skipped_zero);
	out += fmt::format("\t\"memory_bytes\": {},\n", memory.bytes);
	out += fmt::format("\t\"memory_seconds\": {:.3f},\n", memory_seconds);
	out += fmt::format("\t\"dirty_pages\": {},\n", dirty_pages);
	out += fmt::format("\t\"submissions\": {},\n", submissions);
	out += fmt::format("\t\"prt_apertures\": {},\n", apertures);
	out += fmt::format("\t\"shaders\": {},\n", shaders);
	out += fmt::format("\t\"video_out_registrations\": {},\n", video_out);
	out += fmt::format("\t\"register_files\": {},\n", processors.size());
	out += fmt::format("\t\"flushed_buffer_bytes\": {},\n", flushed_bytes);
	out += fmt::format("\t\"flushed_images\": {},\n", flushed_images);
	out += fmt::format("\t\"dropped_gaps\": {},\n", dropped_gaps);
	if (gaps.empty()) {
		out += "\t\"gaps\": []\n";
	} else {
		out += "\t\"gaps\": [\n";
		for (size_t i = 0; i < gaps.size(); i++) {
			out += fmt::format("\t\t{{\"vaddr\": \"0x{:016x}\", \"size\": \"0x{:x}\", "
			                   "\"reason\": \"{}\"}}{}\n",
			                   gaps[i].vaddr, gaps[i].size, JsonEscape(gaps[i].reason),
			                   i + 1 == gaps.size() ? "" : ",");
		}
		out += "\t]\n";
	}
	out += "}\n";

	Writer manifest;
	if (!manifest.Open(folder / "manifest.json")) {
		LOGF("FrameCapture: cannot create manifest.json\n");
		ok = false;
	} else {
		manifest.Write(out.data(), out.size());
		ok = manifest.Close() && ok;
	}

	const auto seconds =
	    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	LOGF("FrameCapture: frame %d written to %s in %.2f s: %" PRIu64 " ranges, %" PRIu64
	     " pages (%.2f MiB), %" PRIu64 " dirty pages, %" PRIu64 " submissions, %zu gaps%s\n",
	     frame_num, folder.string().c_str(), seconds, memory.ranges, memory.pages,
	     static_cast<double>(memory.bytes) / (1024.0 * 1024.0), dirty_pages, submissions,
	     gaps.size(), ok ? "" : " (INCOMPLETE)");
	std::printf("FrameCapture: frame %d written to %s in %.2f s: %" PRIu64 " pages (%.2f MiB), "
	            "%" PRIu64 " submissions, %zu gaps%s\n",
	            frame_num, folder.string().c_str(), seconds, memory.pages,
	            static_cast<double>(memory.bytes) / (1024.0 * 1024.0), submissions, gaps.size(),
	            ok ? "" : " (INCOMPLETE)");
	std::fflush(stdout);

	return ok;
}

} // namespace Libs::Graphics::Replay
