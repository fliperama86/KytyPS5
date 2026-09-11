#include "graphics/replay/frameCapture.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/graphicsRun.h"
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

// About 75 000 CPU-dirty marks reach the recorder in a Nexus frame, so the buffer is reserved
// once and only ever cleared: after the first frame the append path allocates nothing. The churn
// stream is two orders of magnitude smaller but grows with the captured frame count.
constexpr size_t DIRTY_EVENT_RESERVE   = 262144;
constexpr size_t CHURN_EVENT_RESERVE   = 65536;
// One record per draw and dispatch preparation: about 11 000 a frame.
constexpr size_t PREPARE_EVENT_RESERVE = 131072;

struct Recorder {
	std::mutex                                      mutex;
	std::unordered_map<uint64_t, PendingSubmission> pending;
	std::vector<PendingSubmission>                  ordered;
	uint64_t                                        next_id = 1;
	// A lock of its own: the dirty marks arrive from every guest thread and from the GPU thread,
	// and must not queue behind a submission being copied.
	std::mutex                    dirty_mutex;
	std::vector<DirtyEventRecord> dirty_events;
	// And one more for the churn events, which arrive from the GPU thread (buffer registrations
	// and retirements) and from every guest thread that maps or unmaps memory.
	std::mutex                    churn_mutex;
	std::vector<ChurnEventRecord> churn_events;
	// The preparations of the frame. Appended from the GPU thread only, but the writer reads it
	// from there too, so it takes the lock like the others.
	std::mutex                      prepare_mutex;
	std::vector<PrepareEventRecord> prepare_events;
	// Index of the frame being recorded inside the capture window, 0 for the first. Written by
	// the GPU thread in RecordDone, read by every thread that appends an event.
	std::atomic_uint32_t frame_index {0};
};

void CapturePrepareEvent(const PrepareSample& sample);

struct State {
	bool                  armed      = false;
	int                   capture_at = -1;
	uint32_t              frames     = 1;
	bool                  exit_after = true;
	std::filesystem::path folder;
	std::atomic_bool      active {false};
	// The frame the trigger file was seen at, -1 until then. Only meaningful without
	// --frame-capture-at; the GPU thread is the only writer.
	int      trigger_frame = -1;
	Recorder recorder;
};

State& Instance() {
	static State state;
	static bool  loaded = [&] {
		state.armed = Config::FrameCaptureEnabled();
		if (state.armed) {
			state.folder     = Config::GetFrameCaptureFolder();
			state.capture_at = Config::GetFrameCaptureFrame();
			state.frames     = Config::GetFrameCaptureFrames();
			state.exit_after = Config::FrameCaptureExitEnabled();
			state.recorder.dirty_events.reserve(DIRTY_EVENT_RESERVE);
			state.recorder.churn_events.reserve(CHURN_EVENT_RESERVE);
			state.recorder.prepare_events.reserve(PREPARE_EVENT_RESERVE);
			state.active.store(true, std::memory_order_relaxed);
			Detail::g_dirty_events_armed.store(true, std::memory_order_relaxed);
			Detail::g_churn_events_armed.store(true, std::memory_order_relaxed);
			SetPrepareSink(&CapturePrepareEvent);
			LOGF("FrameCapture: armed, folder=%s frame=%d frames=%u exit=%s\n",
			     state.folder.string().c_str(), state.capture_at, state.frames,
			     state.exit_after ? "true" : "false");
		}
		return true;
	}();
	(void)loaded;
	return state;
}

// The capture's prepare sink: one record per GpuResourceManager::PrepareBda call of the frame.
// GPU thread only.
void CapturePrepareEvent(const PrepareSample& sample) {
	auto&              recorder = Instance().recorder;
	PrepareEventRecord record {};
	record.frame        = recorder.frame_index.load(std::memory_order_relaxed);
	record.progress     = GuestGpu::Progress();
	record.scanned      = sample.scanned ? 1u : 0u;
	record.dirty_ranges = sample.dirty_ranges;
	record.synchronized = sample.synchronized;
	record.scan_ns      = sample.scan_ns;
	record.dirty_bytes  = sample.dirty_bytes;

	std::scoped_lock lock(recorder.prepare_mutex);
	recorder.prepare_events.push_back(record);
}

// Churn-event counts per kind and per frame, for the manifest.
struct ChurnCounts {
	std::vector<uint64_t> registers;
	std::vector<uint64_t> retires;
	std::vector<uint64_t> maps;
	std::vector<uint64_t> unmaps;
};

std::string JsonArray(const std::vector<uint64_t>& values) {
	std::string out = "[";
	for (size_t i = 0; i < values.size(); i++) {
		out += fmt::format("{}{}", i == 0 ? "" : ", ", values[i]);
	}
	out += "]";
	return out;
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

// Adds one to the per-frame counter, growing the vector to fit the frame index.
void CountPerFrame(std::vector<uint64_t>& counts, uint32_t frame) {
	if (counts.size() <= frame) {
		counts.resize(static_cast<size_t>(frame) + 1, 0);
	}
	counts[frame]++;
}

// The arrival order of the frames' CPU-dirty marks, each keyed to the GPU thread's progress
// clock. This is what a replay needs to move the BDA generation as often as the game does.
uint64_t WriteDirtyEvents(const std::filesystem::path& folder, std::vector<uint64_t>& per_frame,
                          bool& ok) {
	Writer file;
	if (!file.Open(folder / "dirty-events.bin")) {
		LOGF("FrameCapture: cannot create dirty-events.bin\n");
		ok = false;
		return 0;
	}

	auto&            recorder = Instance().recorder;
	std::unique_lock lock(recorder.dirty_mutex);
	file.Write(recorder.dirty_events.data(),
	           recorder.dirty_events.size() * sizeof(DirtyEventRecord));
	for (const auto& event: recorder.dirty_events) {
		CountPerFrame(per_frame, event.frame);
	}
	const auto count = static_cast<uint64_t>(recorder.dirty_events.size());
	lock.unlock();

	ok = file.Close() && ok;
	return count;
}

// Every other BDA-generation bump of the captured frames: buffer registrations and retirements
// and guest map and unmap calls. Diagnostics -- the replay does not read this stream; it is what
// measures the churn a looped sequence of frames has to reproduce (docs/frame-replay.md, phase E).
uint64_t WriteChurnEvents(const std::filesystem::path& folder, ChurnCounts& counts, bool& ok) {
	Writer file;
	if (!file.Open(folder / "churn-events.bin")) {
		LOGF("FrameCapture: cannot create churn-events.bin\n");
		ok = false;
		return 0;
	}

	auto&            recorder = Instance().recorder;
	std::unique_lock lock(recorder.churn_mutex);
	file.Write(recorder.churn_events.data(),
	           recorder.churn_events.size() * sizeof(ChurnEventRecord));
	for (const auto& event: recorder.churn_events) {
		switch (static_cast<ChurnEventKind>(event.kind)) {
			case ChurnEventKind::BufferRegister:
				CountPerFrame(counts.registers, event.frame);
				break;
			case ChurnEventKind::BufferRetire: CountPerFrame(counts.retires, event.frame); break;
			case ChurnEventKind::Map: CountPerFrame(counts.maps, event.frame); break;
			case ChurnEventKind::Unmap: CountPerFrame(counts.unmaps, event.frame); break;
		}
	}
	const auto count = static_cast<uint64_t>(recorder.churn_events.size());
	lock.unlock();

	ok = file.Close() && ok;
	return count;
}

// Every draw and dispatch preparation of the captured frames and whether it scanned: the ground
// truth a replay is measured against (docs/frame-replay.md, phase E).
uint64_t WritePrepareEvents(const std::filesystem::path& folder, std::vector<uint64_t>& per_frame,
                            std::vector<uint64_t>& scans_per_frame, uint64_t& scans,
                            uint64_t& scan_ns, bool& ok) {
	Writer file;
	if (!file.Open(folder / "prepare-events.bin")) {
		LOGF("FrameCapture: cannot create prepare-events.bin\n");
		ok = false;
		return 0;
	}

	auto&            recorder = Instance().recorder;
	std::unique_lock lock(recorder.prepare_mutex);
	file.Write(recorder.prepare_events.data(),
	           recorder.prepare_events.size() * sizeof(PrepareEventRecord));
	for (const auto& event: recorder.prepare_events) {
		CountPerFrame(per_frame, event.frame);
		if (event.scanned != 0) {
			CountPerFrame(scans_per_frame, event.frame);
			scans++;
			scan_ns += event.scan_ns;
		}
	}
	const auto count = static_cast<uint64_t>(recorder.prepare_events.size());
	lock.unlock();

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

namespace Detail {
std::atomic_bool            g_dirty_events_armed {false};
std::atomic_bool            g_churn_events_armed {false};
std::atomic<PrepareSink>    g_prepare_sink {nullptr};
} // namespace Detail

void SetPrepareSink(PrepareSink sink) noexcept {
	Detail::g_prepare_sink.store(sink, std::memory_order_release);
}

bool Armed() noexcept {
	return Instance().active.load(std::memory_order_relaxed);
}

void RecordDirtyEventSlow(uint64_t vaddr, uint64_t size) {
	auto&            recorder = Instance().recorder;
	DirtyEventRecord record {};
	record.frame      = recorder.frame_index.load(std::memory_order_relaxed);
	record.progress   = GuestGpu::Progress();
	record.submission = GuestGpu::SubmissionIndex();
	record.vaddr      = vaddr;
	record.size       = size;

	std::scoped_lock lock(recorder.dirty_mutex);
	recorder.dirty_events.push_back(record);
}

void RecordChurnEventSlow(ChurnEventKind kind, uint64_t vaddr, uint64_t size) {
	auto&            recorder = Instance().recorder;
	ChurnEventRecord record {};
	record.frame    = recorder.frame_index.load(std::memory_order_relaxed);
	record.progress = GuestGpu::Progress();
	record.kind     = static_cast<uint32_t>(kind);
	record.vaddr    = vaddr;
	record.size     = size;

	std::scoped_lock lock(recorder.churn_mutex);
	recorder.churn_events.push_back(record);
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

void RecordDone(int frame_num, uint32_t progress) {
	auto& recorder = Instance().recorder;
	{
		std::scoped_lock lock(recorder.mutex);

		PendingSubmission pending;
		pending.header.kind = static_cast<uint32_t>(SubmissionKind::Done);
		// The Done record carries the frame it closes: its number and its progress count, so a
		// multi-frame capture is self-describing (frameCaptureFormat.h).
		pending.header.queue_id        = progress;
		pending.header.flip_request_id = static_cast<uint64_t>(static_cast<uint32_t>(frame_num));
		recorder.ordered.push_back(std::move(pending));
	}
	// Every event that arrives from here on belongs to the next frame of the window. A frame the
	// capture then discards takes the index back to 0 through ResetFrame.
	recorder.frame_index.fetch_add(1, std::memory_order_relaxed);
}

void ResetFrame() noexcept {
	auto& recorder = Instance().recorder;
	{
		std::scoped_lock lock(recorder.mutex);
		recorder.ordered.clear();
		recorder.pending.clear();
	}
	{
		std::scoped_lock lock(recorder.dirty_mutex);
		recorder.dirty_events.clear();
	}
	{
		std::scoped_lock lock(recorder.churn_mutex);
		recorder.churn_events.clear();
	}
	{
		std::scoped_lock lock(recorder.prepare_mutex);
		recorder.prepare_events.clear();
	}
	recorder.frame_index.store(0, std::memory_order_relaxed);
}

FrameDisposition OnFrameDone(int frame_num) {
	auto& state = Instance();
	if (!state.active.load(std::memory_order_relaxed)) {
		return FrameDisposition::Discard;
	}
	const auto frames = state.frames == 0 ? 1u : state.frames;

	int first = state.capture_at;
	if (state.capture_at < 0) {
		// Trigger mode: the window starts at the frame the file is first seen at. Once it has
		// fired the file is never tested again, so a capture of several frames does not depend on
		// the file surviving.
		if (state.trigger_frame < 0) {
			std::error_code error;
			if (std::filesystem::exists(state.folder / "trigger", error)) {
				state.trigger_frame = frame_num;
			}
		}
		first = state.trigger_frame;
	}
	if (first < 0 || frame_num < first) {
		return FrameDisposition::Discard;
	}
	const int64_t last = static_cast<int64_t>(first) + static_cast<int64_t>(frames) - 1;
	return frame_num >= last ? FrameDisposition::Write : FrameDisposition::Keep;
}

bool WriteCapture(RenderContext& renderer, int frame_num,
                  std::span<const ProcessorRegisters> processors) {
	auto& state = Instance();
	// One capture per run: disarm before anything can fail, so a broken capture does not retry
	// every frame. The dirty-mark path is disarmed with it, but only once the events already in the
	// recorder have been written out below.
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

	// 3. Everything else. The frames of the window, in order, come from the Done records the
	// recorder holds; each carries its frame number and its progress count.
	std::vector<uint64_t> frame_numbers;
	std::vector<uint64_t> progress_per_frame;
	{
		auto&            recorder = Instance().recorder;
		std::scoped_lock lock(recorder.mutex);
		for (const auto& submission: recorder.ordered) {
			if (submission.header.kind == static_cast<uint32_t>(SubmissionKind::Done)) {
				frame_numbers.push_back(submission.header.flip_request_id);
				progress_per_frame.push_back(submission.header.queue_id);
			}
		}
	}
	uint64_t progress = 0;
	for (const auto value: progress_per_frame) {
		progress += value;
	}

	const auto dirty_pages = WriteDirtyPages(renderer, ranges, folder, ok);
	std::vector<uint64_t> dirty_per_frame;
	const auto dirty_events = WriteDirtyEvents(folder, dirty_per_frame, ok);
	Detail::g_dirty_events_armed.store(false, std::memory_order_relaxed);
	ChurnCounts churn;
	const auto  churn_events = WriteChurnEvents(folder, churn, ok);
	Detail::g_churn_events_armed.store(false, std::memory_order_relaxed);
	std::vector<uint64_t> prepares_per_frame;
	std::vector<uint64_t> prepare_scans_per_frame;
	uint64_t              prepare_scans    = 0;
	uint64_t              prepare_scan_ns  = 0;
	const auto prepare_events = WritePrepareEvents(
	    folder, prepares_per_frame, prepare_scans_per_frame, prepare_scans, prepare_scan_ns, ok);
	SetPrepareSink(nullptr);
	uint32_t   width       = 0;
	uint32_t   height      = 0;
	const auto video_out   = WriteVideoOut(folder, width, height, ok);
	WriteRegisters(processors, folder, ok);
	const auto submissions = WriteSubmissions(folder, ok);
	const auto apertures   = WritePrtApertures(folder, ok);
	const auto shaders     = WriteShaders(folder, ok);

	// Pad the per-frame arrays so every one of them has an entry per captured frame, including
	// the frames in which nothing of that kind happened.
	const auto frames = frame_numbers.size();
	for (auto* counts: {&dirty_per_frame, &churn.registers, &churn.retires, &churn.maps,
	                    &churn.unmaps, &progress_per_frame, &prepares_per_frame,
	                    &prepare_scans_per_frame}) {
		counts->resize(frames, 0);
	}

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
	out += fmt::format("\t\"frame\": {},\n",
	                   frame_numbers.empty() ? static_cast<uint64_t>(frame_num)
	                                         : frame_numbers.front());
	out += fmt::format("\t\"frames\": {},\n", frames);
	out += fmt::format("\t\"snapshot_frame\": {},\n", frame_num);
	out += fmt::format("\t\"frame_numbers\": {},\n", JsonArray(frame_numbers));
	out += fmt::format("\t\"width\": {},\n", width);
	out += fmt::format("\t\"height\": {},\n", height);
	out += fmt::format("\t\"ranges\": {},\n", memory.ranges);
	out += fmt::format("\t\"committed_ranges\": {},\n", memory.committed);
	out += fmt::format("\t\"pages\": {},\n", memory.pages);
	out += fmt::format("\t\"zero_pages\": {},\n", memory.skipped_zero);
	out += fmt::format("\t\"memory_bytes\": {},\n", memory.bytes);
	out += fmt::format("\t\"memory_seconds\": {:.3f},\n", memory_seconds);
	out += fmt::format("\t\"dirty_pages\": {},\n", dirty_pages);
	out += fmt::format("\t\"dirty_events\": {},\n", dirty_events);
	out += fmt::format("\t\"dirty_events_per_frame\": {},\n", JsonArray(dirty_per_frame));
	out += fmt::format("\t\"progress_events\": {},\n", progress);
	out += fmt::format("\t\"progress_events_per_frame\": {},\n", JsonArray(progress_per_frame));
	out += fmt::format("\t\"churn_events\": {},\n", churn_events);
	out += fmt::format("\t\"churn_registers_per_frame\": {},\n", JsonArray(churn.registers));
	out += fmt::format("\t\"churn_retires_per_frame\": {},\n", JsonArray(churn.retires));
	out += fmt::format("\t\"churn_maps_per_frame\": {},\n", JsonArray(churn.maps));
	out += fmt::format("\t\"churn_unmaps_per_frame\": {},\n", JsonArray(churn.unmaps));
	out += fmt::format("\t\"prepare_events\": {},\n", prepare_events);
	out += fmt::format("\t\"prepare_scans\": {},\n", prepare_scans);
	out += fmt::format("\t\"prepare_scan_ns\": {},\n", prepare_scan_ns);
	out += fmt::format("\t\"prepares_per_frame\": {},\n", JsonArray(prepares_per_frame));
	out += fmt::format("\t\"prepare_scans_per_frame\": {},\n",
	                   JsonArray(prepare_scans_per_frame));
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
	LOGF("FrameCapture: %zu frame(s) ending at %d written to %s in %.2f s: %" PRIu64
	     " ranges, %" PRIu64 " pages (%.2f MiB), %" PRIu64 " dirty pages, %" PRIu64
	     " dirty events, %" PRIu64 " churn events, %" PRIu64 " preparations (%" PRIu64
	     " scans) over %" PRIu64 " ticks, %" PRIu64 " submissions, %zu gaps%s\n",
	     frames, frame_num, folder.string().c_str(), seconds, memory.ranges, memory.pages,
	     static_cast<double>(memory.bytes) / (1024.0 * 1024.0), dirty_pages, dirty_events,
	     churn_events, prepare_events, prepare_scans, progress, submissions, gaps.size(),
	     ok ? "" : " (INCOMPLETE)");
	std::printf("FrameCapture: %zu frame(s) ending at %d written to %s in %.2f s: %" PRIu64
	            " pages (%.2f MiB), %" PRIu64 " submissions, %" PRIu64 " dirty events, %" PRIu64
	            " churn events, %" PRIu64 " preparations (%" PRIu64 " scans) over %" PRIu64
	            " ticks, %zu gaps%s\n",
	            frames, frame_num, folder.string().c_str(), seconds, memory.pages,
	            static_cast<double>(memory.bytes) / (1024.0 * 1024.0), submissions, dirty_events,
	            churn_events, prepare_events, prepare_scans, progress, gaps.size(),
	            ok ? "" : " (INCOMPLETE)");
	std::fflush(stdout);

	return ok;
}

} // namespace Libs::Graphics::Replay
