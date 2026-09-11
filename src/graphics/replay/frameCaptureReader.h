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
