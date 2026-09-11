#ifndef EMULATOR_SRC_GRAPHICS_REPLAY_FRAMECAPTUREREADER_H_
#define EMULATOR_SRC_GRAPHICS_REPLAY_FRAMECAPTUREREADER_H_

#include "graphics/replay/frameCaptureFormat.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Libs::Graphics::Replay {

// What the replay side needs from manifest.json. Everything else in the manifest is for people
// and scripts; see frameCaptureFormat.h.
struct CaptureManifest {
	uint32_t format_version = 0;
	uint64_t frame          = 0;
	uint32_t width          = 0;
	uint32_t height         = 0;
	// Version 3. `progress_events` is GuestGpu::Progress() summed over the captured frames, the
	// draws plus dispatches the replay's progress clock has to reach for every dirty event to be
	// marked on time; both are 0 in an older capture.
	uint64_t dirty_events    = 0;
	uint64_t progress_events = 0;
	// Version 4. How many consecutive frames the capture holds; 0 in an older capture, which the
	// replay reads as one. The frames themselves are delimited by the Done records of
	// submissions.bin, which also carry each frame's number and progress count.
	uint32_t frames       = 0;
	uint64_t churn_events = 0;
};

struct CaptureRegisterFile {
	uint32_t             queue_id = 0;
	std::vector<uint8_t> data;
};

struct CaptureVideoOut {
	VideoOutRecord       header {};
	std::vector<uint8_t> attribute;
	// Two entries per buffer: data address then metadata address.
	std::vector<uint64_t> addresses;
};

struct CaptureSubmission {
	SubmissionRecord      header {};
	std::vector<uint32_t> commands;
	std::vector<uint32_t> constants;
};

// Reads a capture directory. Every call reports a failure through `error` instead of aborting, so
// a truncated or mismatched capture ends the replay with a message rather than a crash.
class CaptureReader final {
public:
	bool Open(const std::filesystem::path& dir, std::string* error);

	[[nodiscard]] const CaptureManifest&       Manifest() const noexcept { return m_manifest; }
	[[nodiscard]] const std::filesystem::path& Directory() const noexcept { return m_dir; }

	bool ReadRanges(std::vector<RangeRecord>* out, std::string* error) const;
	bool ReadDirtyPages(std::vector<uint64_t>* out, std::string* error) const;
	bool ReadRegisterFiles(std::vector<CaptureRegisterFile>* out, std::string* error) const;
	bool ReadVideoOut(std::vector<CaptureVideoOut>* out, std::string* error) const;
	bool ReadSubmissions(std::vector<CaptureSubmission>* out, std::string* error) const;
	// Version 2 streams. A version 1 capture has neither file; both then report `present`
	// false with an empty vector and no error, so an old capture still replays.
	bool ReadPrtApertures(std::vector<PrtApertureRecord>* out, bool* present,
	                      std::string* error) const;
	bool ReadShaders(std::vector<ShaderRecord>* out, bool* present, std::string* error) const;
	// The version 3 stream: every CPU-dirty mark of the frames in arrival order. Absent in a v1 or
	// v2 capture, which reports `present` false and leaves the replay on the batch path. A v3
	// stream is 24-byte records without a frame index and widens to frame 0 on the way out.
	bool ReadDirtyEvents(std::vector<DirtyEventRecord>* out, bool* present,
	                     std::string* error) const;
	// The version 4 stream: the buffer registrations, retirements and guest map and unmap calls
	// that moved the BDA generation. Diagnostics -- nothing in the replay consumes it.
	bool ReadChurnEvents(std::vector<ChurnEventRecord>* out, bool* present,
	                     std::string* error) const;

	// Streams memory.bin one page at a time, so a multi-GB capture never has to fit in memory.
	// The sink returns false to stop with an error the caller has already reported.
	using PageSink = std::function<bool(uint64_t vaddr, const uint8_t* data)>;
	bool ForEachPage(const PageSink& sink, uint64_t* pages, std::string* error) const;

	// Exposed for the unit tests; the replay reaches it through Open().
	static bool ParseManifest(const std::string& text, CaptureManifest* out, std::string* error);

private:
	std::filesystem::path m_dir;
	CaptureManifest       m_manifest;
};

} // namespace Libs::Graphics::Replay

#endif // EMULATOR_SRC_GRAPHICS_REPLAY_FRAMECAPTUREREADER_H_
