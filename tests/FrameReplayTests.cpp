// Unit tests for the frame capture reader plus the generator for a minimal synthetic capture.
//
//   frame_replay_tests                    runs the reader tests
//   frame_replay_tests --write-capture D  writes a minimal valid capture into directory D
//
// The synthetic capture is what src/graphics/replay/frameReplay.cpp is exercised against without
// the game: three committed ranges, a handful of non-zero pages, an empty dirty-page set, a few
// CPU-dirty events and churn events, a zeroed graphics register file, one video-out registration
// of a small 2D buffer, and two frames of NOP packets, each a graphics submission plus a flip
// preparation and closed by a Done record. See docs/frame-replay.md.

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/replay/frameCaptureReader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace Libs::Graphics::Replay;

namespace {

int g_failures = 0;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "FrameReplayTests: failed: %s\n", text);
		g_failures++;
	}
}

#define CHECK(expr) Check((expr), #expr)

// The guest layout the synthetic capture restores. Every address is inside the Windows user range
// and 64 KiB aligned, which the video-out surface footprint requires.
constexpr uint64_t DISPLAY_ADDRESS = 0x1100000000ull;
constexpr uint64_t DISPLAY_SIZE    = 0x100000ull;
constexpr uint64_t SCRATCH_ADDRESS = 0x1100200000ull;
constexpr uint64_t SCRATCH_SIZE    = 0x40000ull;
constexpr uint64_t RESERVED_ADDRESS = 0x1100400000ull;
constexpr uint64_t RESERVED_SIZE    = 0x40000ull;

constexpr uint32_t RANGE_TYPE_RESERVED = 0;
constexpr uint32_t RANGE_TYPE_DIRECT   = 2;
constexpr uint32_t RANGE_TYPE_FLEXIBLE = 3;

constexpr uint32_t PROT_READ_WRITE     = 0x03;
constexpr uint32_t PROT_GPU_READ_WRITE = 0x33;

// A PRT aperture well clear of the ranges above, inside the kernel aperture window
// (0x0f00000000 to 0xfc00000000) and 16 KiB aligned.
constexpr uint64_t PRT_APERTURE_ADDRESS = 0x1200000000ull;
constexpr uint64_t PRT_APERTURE_SIZE    = 0x400000ull;

constexpr uint32_t DISPLAY_WIDTH  = 256;
constexpr uint32_t DISPLAY_HEIGHT = 128;
// VIDEO_OUT_FORMAT_POLICIES: B8G8R8A8 sRGB, four bytes per element.
constexpr uint64_t DISPLAY_PIXEL_FORMAT = 0x8000000000000000ull;

// The guest-facing VideoOutBufferAttribute2, mirrored from videoOut.cpp; the replay hands these
// bytes back to VideoOutRegisterBuffers2 unchanged.
#pragma pack(push, 1)
struct VideoOutBufferAttribute2Mirror {
	uint32_t reserved0                   = 0;
	uint32_t tiling_mode                 = 0;
	uint32_t aspect_ratio                = 0;
	uint32_t width                       = 0;
	uint32_t height                      = 0;
	uint32_t pitch_in_pixel              = 0;
	uint64_t option                      = 0;
	uint64_t pixel_format                = 0;
	uint64_t dcc_cb_register_clear_color = 0;
	uint32_t dcc_control                 = 0;
	uint32_t pad0                        = 0;
	uint64_t reserved1[3] {};
};
#pragma pack(pop)
static_assert(sizeof(VideoOutBufferAttribute2Mirror) == 80);

size_t RegisterFileSize() {
	return sizeof(Libs::Graphics::HW::Context) + sizeof(Libs::Graphics::HW::UserConfig) +
	       sizeof(Libs::Graphics::HW::Shader);
}

class Writer final {
public:
	explicit Writer(const std::filesystem::path& path)
	    : m_stream(path, std::ios::binary | std::ios::trunc) {}

	[[nodiscard]] bool Ok() const { return m_stream.is_open(); }

	void Bytes(const void* data, size_t size) {
		m_stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	}

	template <typename T>
	void Value(const T& value) {
		Bytes(&value, sizeof(value));
	}

private:
	std::ofstream m_stream;
};

RangeRecord MakeRange(uint64_t vaddr, uint64_t size, uint32_t prot, uint32_t type,
                      const char* name) {
	RangeRecord record {};
	record.vaddr = vaddr;
	record.size  = size;
	record.prot  = prot;
	record.type  = type;
	const auto length = std::min(std::strlen(name), sizeof(record.name) - 1);
	std::memcpy(record.name, name, length);
	return record;
}

// The frames' CPU-dirty marks: one before the first draw of frame 0, then two during it, and one
// in frame 1. `submission` is carried for people reading the stream; nothing in the reader looks
// at it.
std::vector<DirtyEventRecord> DirtyEvents() {
	std::vector<DirtyEventRecord> events;
	events.push_back({0, 0, 0, SCRATCH_ADDRESS, 0x1000});
	events.push_back({0, 3, 1, SCRATCH_ADDRESS + 0x1000, 0x40});
	events.push_back({0, 17, 2, DISPLAY_ADDRESS, 0x2000});
	events.push_back({1, 5, 0, SCRATCH_ADDRESS + 0x2000, 0x80});
	return events;
}

// The BDA preparations of the same two frames: three in the first, two of which scanned, and two
// in the second, one of which did. This is the ground truth a replay is compared with.
std::vector<PrepareEventRecord> PrepareEvents() {
	std::vector<PrepareEventRecord> events;
	events.push_back({0, 2, 1, 4, 7, 0, 0x8000});
	events.push_back({0, 6, 0, 0, 0, 0, 0});
	events.push_back({0, 14, 1, 2, 3, 0, 0x2000});
	events.push_back({1, 3, 1, 1, 1, 0, 0x1000});
	events.push_back({1, 8, 0, 0, 0, 0, 0});
	return events;
}

// The other generation bumps of the same two frames: a buffer registered and retired in the
// first, a guest map and unmap in the second.
std::vector<ChurnEventRecord> ChurnEvents() {
	std::vector<ChurnEventRecord> events;
	events.push_back({0, 2, static_cast<uint32_t>(ChurnEventKind::BufferRegister), SCRATCH_ADDRESS,
	                  0x4000});
	events.push_back({0, 9, static_cast<uint32_t>(ChurnEventKind::BufferRetire), SCRATCH_ADDRESS,
	                  0x4000});
	events.push_back({1, 0, static_cast<uint32_t>(ChurnEventKind::Map), DISPLAY_ADDRESS, 0x10000});
	events.push_back({1, 4, static_cast<uint32_t>(ChurnEventKind::Unmap), DISPLAY_ADDRESS, 0x10000});
	return events;
}

// Two frames, the second shorter, and their GuestGpu frame numbers.
constexpr uint32_t CAPTURE_FRAMES         = 2;
constexpr uint64_t FIRST_FRAME_NUMBER     = 42;
constexpr uint32_t FRAME_PROGRESS[2]      = {21, 13};
constexpr uint32_t PROGRESS_EVENTS        = FRAME_PROGRESS[0] + FRAME_PROGRESS[1];

// A PM4 type-3 NOP: KYTY_PM4(2, IT_NOP, 0) followed by one payload dword the parser skips.
constexpr uint32_t PM4_NOP_HEADER = 0xc0001000u;

std::vector<uint32_t> NopStream(uint32_t packets) {
	std::vector<uint32_t> dwords;
	for (uint32_t i = 0; i < packets; i++) {
		dwords.push_back(PM4_NOP_HEADER);
		dwords.push_back(0);
	}
	return dwords;
}

bool WriteCapture(const std::filesystem::path& dir) {
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);

	const std::vector<RangeRecord> ranges {
	    MakeRange(DISPLAY_ADDRESS, DISPLAY_SIZE, PROT_GPU_READ_WRITE, RANGE_TYPE_DIRECT, "display"),
	    MakeRange(SCRATCH_ADDRESS, SCRATCH_SIZE, PROT_READ_WRITE, RANGE_TYPE_FLEXIBLE, "scratch"),
	    MakeRange(RESERVED_ADDRESS, RESERVED_SIZE, 0, RANGE_TYPE_RESERVED, "reserved"),
	};
	{
		Writer writer(dir / "ranges.bin");
		if (!writer.Ok()) {
			return false;
		}
		for (const auto& range: ranges) {
			writer.Value(range);
		}
	}

	// Four non-zero display pages with a recognizable gradient and one scratch page.
	uint64_t page_count = 0;
	{
		Writer writer(dir / "memory.bin");
		if (!writer.Ok()) {
			return false;
		}
		std::vector<uint8_t> page(kPageSize);
		for (uint32_t index = 0; index < 4; index++) {
			for (size_t i = 0; i < page.size(); i += 4) {
				page[i + 0] = static_cast<uint8_t>(i);        // blue
				page[i + 1] = static_cast<uint8_t>(i >> 8u);  // green
				page[i + 2] = static_cast<uint8_t>(0x40u * (index + 1u)); // red
				page[i + 3] = 0xff;
			}
			const uint64_t vaddr = DISPLAY_ADDRESS + index * kPageSize;
			writer.Value(vaddr);
			writer.Bytes(page.data(), page.size());
			page_count++;
		}
		std::fill(page.begin(), page.end(), static_cast<uint8_t>(0x5a));
		writer.Value(SCRATCH_ADDRESS);
		writer.Bytes(page.data(), page.size());
		page_count++;
	}

	// The captured frame leaves nothing CPU-dirty at its end, so dirty-pages.bin is empty; the
	// v3 event stream still carries the marks that arrived during it, one before the first draw
	// and two keyed to the progress clock.
	{
		Writer writer(dir / "dirty-pages.bin");
		if (!writer.Ok()) {
			return false;
		}
	}

	{
		Writer writer(dir / "dirty-events.bin");
		if (!writer.Ok()) {
			return false;
		}
		for (const auto& event: DirtyEvents()) {
			writer.Value(event);
		}
	}

	{
		Writer writer(dir / "churn-events.bin");
		if (!writer.Ok()) {
			return false;
		}
		for (const auto& event: ChurnEvents()) {
			writer.Value(event);
		}
	}

	{
		Writer writer(dir / "prepare-events.bin");
		if (!writer.Ok()) {
			return false;
		}
		for (const auto& event: PrepareEvents()) {
			writer.Value(event);
		}
	}

	{
		Writer writer(dir / "registers.bin");
		if (!writer.Ok()) {
			return false;
		}
		RegisterFileRecord header {};
		header.queue_id = 0;
		header.size     = static_cast<uint32_t>(RegisterFileSize());
		writer.Value(header);
		const std::vector<uint8_t> zeros(header.size, 0);
		writer.Bytes(zeros.data(), zeros.size());
	}

	{
		Writer writer(dir / "videoout.bin");
		if (!writer.Ok()) {
			return false;
		}
		VideoOutRecord header {};
		header.handle         = 1;
		header.set_index      = 0;
		header.index_start    = 0;
		header.count          = 1;
		header.category       = 0;
		header.attribute_size = sizeof(VideoOutBufferAttribute2Mirror);
		writer.Value(header);

		VideoOutBufferAttribute2Mirror attribute {};
		attribute.width        = DISPLAY_WIDTH;
		attribute.height       = DISPLAY_HEIGHT;
		attribute.pixel_format = DISPLAY_PIXEL_FORMAT;
		writer.Value(attribute);

		const uint64_t addresses[2] {DISPLAY_ADDRESS, 0};
		writer.Bytes(addresses, sizeof(addresses));
	}

	{
		Writer writer(dir / "prt.bin");
		if (!writer.Ok()) {
			return false;
		}
		PrtApertureRecord record {};
		record.index   = 0;
		record.address = PRT_APERTURE_ADDRESS;
		record.size    = PRT_APERTURE_SIZE;
		writer.Value(record);
	}

	{
		Writer writer(dir / "shaders.bin");
		if (!writer.Ok()) {
			return false;
		}
		ShaderRecord record {};
		record.code_address    = SCRATCH_ADDRESS;
		record.code_size_bytes = 0x100;
		writer.Value(record);
	}

	size_t submission_count = 0;
	{
		Writer writer(dir / "submissions.bin");
		if (!writer.Ok()) {
			return false;
		}
		for (uint32_t frame = 0; frame < CAPTURE_FRAMES; frame++) {
			const auto       draw = NopStream(8 - 2 * frame);
			SubmissionRecord graphics {};
			graphics.kind           = static_cast<uint32_t>(SubmissionKind::Graphics);
			graphics.command_dwords = static_cast<uint32_t>(draw.size());
			writer.Value(graphics);
			writer.Bytes(draw.data(), draw.size() * sizeof(uint32_t));
			submission_count++;

			SubmissionRecord flip {};
			flip.kind            = static_cast<uint32_t>(SubmissionKind::FlipPreparation);
			flip.flip_request_id = 1 + frame;
			writer.Value(flip);
			submission_count++;

			// A Done record ends the frame and carries its number and progress count (v4).
			SubmissionRecord done {};
			done.kind            = static_cast<uint32_t>(SubmissionKind::Done);
			done.flip_request_id = FIRST_FRAME_NUMBER + frame;
			done.queue_id        = FRAME_PROGRESS[frame];
			writer.Value(done);
			submission_count++;
		}
	}

	{
		std::ofstream manifest(dir / "manifest.json", std::ios::binary | std::ios::trunc);
		if (!manifest.is_open()) {
			return false;
		}
		manifest << "{\n";
		manifest << "  \"format_version\": " << kFormatVersion << ",\n";
		manifest << "  \"title_id\": \"SYNTHETIC\",\n";
		manifest << "  \"commit\": \"synthetic\",\n";
		manifest << "  \"frame\": " << FIRST_FRAME_NUMBER << ",\n";
		manifest << "  \"frames\": " << CAPTURE_FRAMES << ", \"snapshot_frame\": "
		         << FIRST_FRAME_NUMBER + CAPTURE_FRAMES - 1 << ",\n";
		manifest << "  \"width\": " << DISPLAY_WIDTH << ", \"height\": " << DISPLAY_HEIGHT << ",\n";
		manifest << "  \"ranges\": " << ranges.size() << ", \"pages\": " << page_count
		         << ", \"dirty_pages\": 0, \"submissions\": " << submission_count << ",\n";
		manifest << "  \"dirty_events\": " << DirtyEvents().size()
		         << ", \"progress_events\": " << PROGRESS_EVENTS << ",\n";
		manifest << "  \"churn_events\": " << ChurnEvents().size() << ",\n";
		manifest << "  \"prepare_events\": " << PrepareEvents().size() << ", \"prepare_scans\": 3,\n";
		manifest << "  \"gaps\": []\n";
		manifest << "}\n";
	}

	return true;
}

void CopyCapture(const std::filesystem::path& from, const std::filesystem::path& to) {
	std::error_code ec;
	std::filesystem::remove_all(to, ec);
	std::filesystem::create_directories(to, ec);
	std::filesystem::copy(from, to, std::filesystem::copy_options::overwrite_existing, ec);
}

void TruncateFile(const std::filesystem::path& path, uintmax_t bytes) {
	std::error_code ec;
	std::filesystem::resize_file(path, bytes, ec);
}

void ReplaceManifestVersion(const std::filesystem::path& dir, const std::string& text) {
	std::ofstream manifest(dir / "manifest.json", std::ios::binary | std::ios::trunc);
	manifest << text;
}

void RunTests(const std::filesystem::path& root) {
	const auto good = root / "good";
	CHECK(WriteCapture(good));

	CaptureManifest manifest {};
	std::string     error;
	CHECK(CaptureReader::ParseManifest("{\"format_version\": 1, \"frame\": 7}", &manifest, &error));
	CHECK(manifest.format_version == 1);
	CHECK(manifest.frame == 7);
	CHECK(CaptureReader::ParseManifest("{\"format_version\": 1}", &manifest, &error));
	CHECK(manifest.format_version == 1);
	CHECK(CaptureReader::ParseManifest(
	    "{\"format_version\": 3, \"dirty_events\": 12, \"progress_events\": 34}", &manifest,
	    &error));
	CHECK(manifest.dirty_events == 12);
	CHECK(manifest.progress_events == 34);
	// A version 3 manifest has neither key, and the replay then reads the capture as one frame.
	CHECK(manifest.frames == 0);
	CHECK(manifest.churn_events == 0);
	CHECK(CaptureReader::ParseManifest(
	    "{\"format_version\": 4, \"frames\": 6, \"churn_events\": 900}", &manifest, &error));
	CHECK(manifest.frames == 6);
	CHECK(manifest.churn_events == 900);
	// A version 4 manifest carries no prepare ground truth.
	CHECK(manifest.prepare_events == 0);
	CHECK(manifest.prepare_scans == 0);
	CHECK(CaptureReader::ParseManifest(
	    "{\"format_version\": 5, \"prepare_events\": 11000, \"prepare_scans\": 352}", &manifest,
	    &error));
	CHECK(manifest.prepare_events == 11000);
	CHECK(manifest.prepare_scans == 352);
	CHECK(!CaptureReader::ParseManifest("{\"format_version\": 6}", &manifest, &error));
	CHECK(error.find("version") != std::string::npos);
	CHECK(!CaptureReader::ParseManifest("{\"frame\": 1}", &manifest, &error));

	CaptureReader reader;
	CHECK(reader.Open(good, &error));
	CHECK(reader.Manifest().frame == FIRST_FRAME_NUMBER);
	CHECK(reader.Manifest().frames == CAPTURE_FRAMES);
	CHECK(reader.Manifest().width == DISPLAY_WIDTH);

	std::vector<RangeRecord> ranges;
	CHECK(reader.ReadRanges(&ranges, &error));
	CHECK(ranges.size() == 3);
	CHECK(ranges[0].vaddr == DISPLAY_ADDRESS);
	CHECK(ranges[0].type == RANGE_TYPE_DIRECT);
	CHECK(std::string(ranges[1].name) == "scratch");

	std::vector<uint64_t> dirty;
	CHECK(reader.ReadDirtyPages(&dirty, &error));
	CHECK(dirty.empty());

	CHECK(reader.Manifest().dirty_events == DirtyEvents().size());
	CHECK(reader.Manifest().progress_events == PROGRESS_EVENTS);

	std::vector<DirtyEventRecord> events;
	bool                          events_present = false;
	CHECK(reader.ReadDirtyEvents(&events, &events_present, &error));
	CHECK(events_present);
	CHECK(events.size() == 4);
	CHECK(events[0].frame == 0);
	CHECK(events[0].progress == 0);
	CHECK(events[0].vaddr == SCRATCH_ADDRESS);
	CHECK(events[0].size == 0x1000);
	CHECK(events[1].progress == 3);
	CHECK(events[1].submission == 1);
	CHECK(events[2].progress == 17);
	CHECK(events[2].vaddr == DISPLAY_ADDRESS);
	// The last mark belongs to the capture's second frame.
	CHECK(events[3].frame == 1);
	CHECK(events[3].progress == 5);

	std::vector<ChurnEventRecord> churn;
	bool                          churn_present = false;
	CHECK(reader.ReadChurnEvents(&churn, &churn_present, &error));
	CHECK(churn_present);
	CHECK(churn.size() == 4);
	CHECK(churn[0].frame == 0);
	CHECK(churn[0].kind == static_cast<uint32_t>(ChurnEventKind::BufferRegister));
	CHECK(churn[0].vaddr == SCRATCH_ADDRESS);
	CHECK(churn[1].kind == static_cast<uint32_t>(ChurnEventKind::BufferRetire));
	CHECK(churn[2].frame == 1);
	CHECK(churn[2].kind == static_cast<uint32_t>(ChurnEventKind::Map));
	CHECK(churn[3].kind == static_cast<uint32_t>(ChurnEventKind::Unmap));
	CHECK(churn[3].size == 0x10000);

	std::vector<PrepareEventRecord> prepares;
	bool                            prepares_present = false;
	CHECK(reader.ReadPrepareEvents(&prepares, &prepares_present, &error));
	CHECK(prepares_present);
	CHECK(prepares.size() == PrepareEvents().size());
	CHECK(prepares[0].frame == 0);
	CHECK(prepares[0].scanned == 1);
	CHECK(prepares[0].dirty_ranges == 4);
	CHECK(prepares[0].synchronized == 7);
	CHECK(prepares[0].dirty_bytes == 0x8000);
	CHECK(prepares[1].scanned == 0);
	CHECK(prepares[3].frame == 1);
	CHECK(prepares[3].scanned == 1);
	CHECK(reader.Manifest().prepare_events == PrepareEvents().size());
	CHECK(reader.Manifest().prepare_scans == 3);

	std::vector<CaptureRegisterFile> register_files;
	CHECK(reader.ReadRegisterFiles(&register_files, &error));
	CHECK(register_files.size() == 1);
	CHECK(register_files[0].queue_id == 0);
	CHECK(register_files[0].data.size() == RegisterFileSize());

	std::vector<CaptureVideoOut> video_out;
	CHECK(reader.ReadVideoOut(&video_out, &error));
	CHECK(video_out.size() == 1);
	CHECK(video_out[0].attribute.size() == sizeof(VideoOutBufferAttribute2Mirror));
	CHECK(video_out[0].addresses.size() == 2);
	CHECK(video_out[0].addresses[0] == DISPLAY_ADDRESS);

	std::vector<PrtApertureRecord> apertures;
	bool                           present = false;
	CHECK(reader.ReadPrtApertures(&apertures, &present, &error));
	CHECK(present);
	CHECK(apertures.size() == 1);
	CHECK(apertures[0].address == PRT_APERTURE_ADDRESS);
	CHECK(apertures[0].size == PRT_APERTURE_SIZE);

	std::vector<ShaderRecord> shaders;
	CHECK(reader.ReadShaders(&shaders, &present, &error));
	CHECK(present);
	CHECK(shaders.size() == 1);
	CHECK(shaders[0].code_address == SCRATCH_ADDRESS);

	std::vector<CaptureSubmission> submissions;
	CHECK(reader.ReadSubmissions(&submissions, &error));
	CHECK(submissions.size() == 3 * CAPTURE_FRAMES);
	CHECK(submissions[0].commands.size() == 16);
	CHECK(submissions[0].commands[0] == PM4_NOP_HEADER);
	CHECK(submissions[1].header.kind == static_cast<uint32_t>(SubmissionKind::FlipPreparation));
	CHECK(submissions[2].header.kind == static_cast<uint32_t>(SubmissionKind::Done));
	// Every Done record ends a frame and says which frame it was and how far its progress clock
	// got, which is how a replay splits the stream without a per-frame index in the manifest.
	CHECK(submissions[2].header.flip_request_id == FIRST_FRAME_NUMBER);
	CHECK(submissions[2].header.queue_id == FRAME_PROGRESS[0]);
	CHECK(submissions[3].commands.size() == 12);
	CHECK(submissions[5].header.kind == static_cast<uint32_t>(SubmissionKind::Done));
	CHECK(submissions[5].header.flip_request_id == FIRST_FRAME_NUMBER + 1);
	CHECK(submissions[5].header.queue_id == FRAME_PROGRESS[1]);

	uint64_t pages = 0;
	uint64_t seen  = 0;
	CHECK(reader.ForEachPage(
	    [&](uint64_t vaddr, const uint8_t* data) {
		    if (vaddr == SCRATCH_ADDRESS) {
			    CHECK(data[0] == 0x5a);
		    }
		    seen++;
		    return true;
	    },
	    &pages, &error));
	CHECK(pages == 5);
	CHECK(seen == 5);

	// A capture cut short inside a record is rejected, not read as far as it goes.
	const auto      truncated = root / "truncated";
	std::error_code ec;
	CopyCapture(good, truncated);
	{
		const auto size = std::filesystem::file_size(truncated / "submissions.bin", ec);
		TruncateFile(truncated / "submissions.bin", size - 8);
		CaptureReader short_reader;
		CHECK(short_reader.Open(truncated, &error));
		std::vector<CaptureSubmission> records;
		CHECK(!short_reader.ReadSubmissions(&records, &error));
		CHECK(error.find("submissions.bin") != std::string::npos);
	}
	{
		const auto size = std::filesystem::file_size(truncated / "memory.bin", ec);
		TruncateFile(truncated / "memory.bin", size - 32);
		CaptureReader short_reader;
		CHECK(short_reader.Open(truncated, &error));
		uint64_t count = 0;
		CHECK(!short_reader.ForEachPage([](uint64_t, const uint8_t*) { return true; }, &count,
		                                &error));
		CHECK(error.find("memory.bin") != std::string::npos);
	}
	{
		const auto size = std::filesystem::file_size(truncated / "ranges.bin", ec);
		TruncateFile(truncated / "ranges.bin", size - 4);
		CaptureReader short_reader;
		CHECK(short_reader.Open(truncated, &error));
		std::vector<RangeRecord> records;
		CHECK(!short_reader.ReadRanges(&records, &error));
	}

	// A version 1 capture has no prt.bin and no shaders.bin; it still opens, and the two optional
	// streams report themselves absent rather than failing.
	const auto legacy = root / "legacy";
	CopyCapture(good, legacy);
	std::filesystem::remove(legacy / "prt.bin", ec);
	std::filesystem::remove(legacy / "shaders.bin", ec);
	std::filesystem::remove(legacy / "dirty-events.bin", ec);
	std::filesystem::remove(legacy / "churn-events.bin", ec);
	std::filesystem::remove(legacy / "prepare-events.bin", ec);
	ReplaceManifestVersion(legacy, "{\"format_version\": 1, \"frame\": 5}\n");
	{
		CaptureReader legacy_reader;
		CHECK(legacy_reader.Open(legacy, &error));
		CHECK(legacy_reader.Manifest().format_version == 1);
		std::vector<PrtApertureRecord> none;
		bool                           legacy_present = true;
		CHECK(legacy_reader.ReadPrtApertures(&none, &legacy_present, &error));
		CHECK(!legacy_present);
		CHECK(none.empty());
		std::vector<ShaderRecord> no_shaders;
		CHECK(legacy_reader.ReadShaders(&no_shaders, &legacy_present, &error));
		CHECK(!legacy_present);
		CHECK(no_shaders.empty());
		std::vector<DirtyEventRecord> no_events;
		CHECK(legacy_reader.ReadDirtyEvents(&no_events, &legacy_present, &error));
		CHECK(!legacy_present);
		CHECK(no_events.empty());
		std::vector<ChurnEventRecord> no_churn;
		CHECK(legacy_reader.ReadChurnEvents(&no_churn, &legacy_present, &error));
		CHECK(!legacy_present);
		CHECK(no_churn.empty());
		std::vector<PrepareEventRecord> no_prepares;
		CHECK(legacy_reader.ReadPrepareEvents(&no_prepares, &legacy_present, &error));
		CHECK(!legacy_present);
		CHECK(no_prepares.empty());
		CHECK(legacy_reader.Manifest().dirty_events == 0);
		CHECK(legacy_reader.Manifest().progress_events == 0);
		CHECK(legacy_reader.Manifest().frames == 0);
	}

	// A version 3 capture: its dirty events are 24-byte records without a frame index, and the
	// reader widens them onto the one frame such a capture has.
	const auto legacy3 = root / "legacy3";
	CopyCapture(good, legacy3);
	std::filesystem::remove(legacy3 / "churn-events.bin", ec);
	std::filesystem::remove(legacy3 / "prepare-events.bin", ec);
	{
		Writer writer(legacy3 / "dirty-events.bin");
		CHECK(writer.Ok());
		for (const auto& event: DirtyEvents()) {
			const DirtyEventRecordV3 record {event.progress, event.submission, event.vaddr,
			                                 event.size};
			writer.Value(record);
		}
	}
	ReplaceManifestVersion(legacy3, "{\"format_version\": 3, \"frame\": 42, "
	                                "\"dirty_events\": 4, \"progress_events\": 21}\n");
	{
		CaptureReader v3_reader;
		CHECK(v3_reader.Open(legacy3, &error));
		CHECK(v3_reader.Manifest().format_version == 3);
		CHECK(v3_reader.Manifest().frames == 0);
		std::vector<DirtyEventRecord> widened;
		bool                          v3_present = false;
		CHECK(v3_reader.ReadDirtyEvents(&widened, &v3_present, &error));
		CHECK(v3_present);
		CHECK(widened.size() == DirtyEvents().size());
		CHECK(widened[0].frame == 0);
		CHECK(widened[1].progress == 3);
		CHECK(widened[2].vaddr == DISPLAY_ADDRESS);
		// The v3 record of the fourth mark says nothing about a second frame, so it widens to 0.
		CHECK(widened[3].frame == 0);
		CHECK(widened[3].size == 0x80);
		std::vector<ChurnEventRecord> v3_churn;
		CHECK(v3_reader.ReadChurnEvents(&v3_churn, &v3_present, &error));
		CHECK(!v3_present);
	}

	// A truncated optional stream is still an error.
	{
		const auto size = std::filesystem::file_size(truncated / "prt.bin", ec);
		TruncateFile(truncated / "prt.bin", size - 4);
		CaptureReader short_reader;
		CHECK(short_reader.Open(truncated, &error));
		std::vector<PrtApertureRecord> records;
		bool                           short_present = false;
		CHECK(!short_reader.ReadPrtApertures(&records, &short_present, &error));
		CHECK(error.find("prt.bin") != std::string::npos);
	}
	{
		const auto size = std::filesystem::file_size(truncated / "dirty-events.bin", ec);
		TruncateFile(truncated / "dirty-events.bin", size - 7);
		CaptureReader short_reader;
		CHECK(short_reader.Open(truncated, &error));
		std::vector<DirtyEventRecord> records;
		bool                          short_present = false;
		CHECK(!short_reader.ReadDirtyEvents(&records, &short_present, &error));
		CHECK(error.find("dirty-events.bin") != std::string::npos);
	}
	{
		const auto size = std::filesystem::file_size(truncated / "prepare-events.bin", ec);
		TruncateFile(truncated / "prepare-events.bin", size - 5);
		CaptureReader short_reader;
		CHECK(short_reader.Open(truncated, &error));
		std::vector<PrepareEventRecord> records;
		bool                            short_present = false;
		CHECK(!short_reader.ReadPrepareEvents(&records, &short_present, &error));
		CHECK(error.find("prepare-events.bin") != std::string::npos);
	}
	{
		const auto size = std::filesystem::file_size(truncated / "churn-events.bin", ec);
		TruncateFile(truncated / "churn-events.bin", size - 9);
		CaptureReader short_reader;
		CHECK(short_reader.Open(truncated, &error));
		std::vector<ChurnEventRecord> records;
		bool                          short_present = false;
		CHECK(!short_reader.ReadChurnEvents(&records, &short_present, &error));
		CHECK(error.find("churn-events.bin") != std::string::npos);
	}

	// A capture from a different format version is rejected before anything is restored.
	const auto mismatched = root / "mismatched";
	CopyCapture(good, mismatched);
	ReplaceManifestVersion(mismatched, "{\"format_version\": 99, \"frame\": 1}\n");
	CaptureReader other_reader;
	CHECK(!other_reader.Open(mismatched, &error));
	CHECK(error.find("99") != std::string::npos);

	// A directory with no manifest at all is rejected too.
	const auto empty = root / "empty";
	std::filesystem::create_directories(empty, ec);
	CaptureReader empty_reader;
	CHECK(!empty_reader.Open(empty, &error));
}

} // namespace

int main(int argc, char* argv[]) {
	if (argc == 3 && std::string(argv[1]) == "--write-capture") {
		const std::filesystem::path dir = argv[2];
		if (!WriteCapture(dir)) {
			std::fprintf(stderr, "FrameReplayTests: could not write a capture into %s\n", argv[2]);
			return 1;
		}
		std::printf("wrote a synthetic capture into %s\n", argv[2]);
		return 0;
	}

	std::error_code ec;
	const auto      root = std::filesystem::temp_directory_path(ec) / "kyty_frame_replay_tests";
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root, ec);

	RunTests(root);

	std::filesystem::remove_all(root, ec);

	if (g_failures != 0) {
		std::fprintf(stderr, "FrameReplayTests: %d checks failed\n", g_failures);
		return 1;
	}
	std::printf("FrameReplayTests: ok\n");
	return 0;
}
