#include "graphics/replay/frameCaptureReader.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Libs::Graphics::Replay {

namespace {

void SetError(std::string* error, const std::string& text) {
	if (error != nullptr) {
		*error = text;
	}
}

std::string FileName(const std::filesystem::path& path) {
	return path.filename().string();
}

// Opens one capture file for reading. A missing file is a hard error: every stream the format
// lists is written by the capture, empty ones as zero-byte files.
bool OpenStream(const std::filesystem::path& path, std::ifstream* stream, std::string* error) {
	stream->open(path, std::ios::binary);
	if (!stream->is_open()) {
		SetError(error, "cannot open " + FileName(path));
		return false;
	}
	return true;
}

// Reads exactly `size` bytes. `at_end` tells a clean end of stream from a truncated record.
bool ReadExact(std::ifstream& stream, void* data, size_t size, bool* at_end) {
	if (at_end != nullptr) {
		*at_end = false;
	}
	if (size == 0) {
		return true;
	}
	stream.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
	const auto read = static_cast<size_t>(stream.gcount());
	if (read == size) {
		return true;
	}
	if (read == 0 && at_end != nullptr) {
		*at_end = true;
	}
	return false;
}

template <typename T>
bool ReadRecordHeader(std::ifstream& stream, T* out, bool* at_end) {
	return ReadExact(stream, out, sizeof(T), at_end);
}

// Finds "key": <number> in a flat JSON object. The manifest has no nesting at the top level
// apart from "gaps", which the replay does not read, so a scan is enough and keeps the reader
// free of a JSON dependency (the unit tests build it without the emulator).
bool ManifestNumber(const std::string& text, const char* key, uint64_t* out, bool* found) {
	*found             = false;
	const auto quoted  = std::string("\"") + key + "\"";
	const auto key_pos = text.find(quoted);
	if (key_pos == std::string::npos) {
		return true;
	}
	auto pos = text.find(':', key_pos + quoted.size());
	if (pos == std::string::npos) {
		return false;
	}
	pos++;
	while (pos < text.size() && (std::isspace(static_cast<unsigned char>(text[pos])) != 0)) {
		pos++;
	}
	if (pos >= text.size() || (std::isdigit(static_cast<unsigned char>(text[pos])) == 0)) {
		return false;
	}
	uint64_t value = 0;
	while (pos < text.size() && (std::isdigit(static_cast<unsigned char>(text[pos])) != 0)) {
		value = value * 10u + static_cast<uint64_t>(text[pos] - '0');
		pos++;
	}
	*out   = value;
	*found = true;
	return true;
}

} // namespace

bool CaptureReader::ParseManifest(const std::string& text, CaptureManifest* out,
                                  std::string* error) {
	CaptureManifest manifest {};
	uint64_t        value = 0;
	bool            found = false;

	if (!ManifestNumber(text, "format_version", &value, &found) || !found) {
		SetError(error, "manifest.json has no readable format_version");
		return false;
	}
	manifest.format_version = static_cast<uint32_t>(value);
	if (manifest.format_version < kMinFormatVersion || manifest.format_version > kFormatVersion) {
		SetError(error, "capture format version " + std::to_string(manifest.format_version) +
		                    " but this build reads versions " + std::to_string(kMinFormatVersion) +
		                    " to " + std::to_string(kFormatVersion));
		return false;
	}

	if (!ManifestNumber(text, "frame", &value, &found)) {
		SetError(error, "manifest.json has a malformed frame");
		return false;
	}
	manifest.frame = found ? value : 0;

	if (!ManifestNumber(text, "width", &value, &found)) {
		SetError(error, "manifest.json has a malformed width");
		return false;
	}
	manifest.width = found ? static_cast<uint32_t>(value) : 0;

	if (!ManifestNumber(text, "height", &value, &found)) {
		SetError(error, "manifest.json has a malformed height");
		return false;
	}
	manifest.height = found ? static_cast<uint32_t>(value) : 0;

	if (!ManifestNumber(text, "dirty_events", &value, &found)) {
		SetError(error, "manifest.json has a malformed dirty_events");
		return false;
	}
	manifest.dirty_events = found ? value : 0;

	if (!ManifestNumber(text, "progress_events", &value, &found)) {
		SetError(error, "manifest.json has a malformed progress_events");
		return false;
	}
	manifest.progress_events = found ? value : 0;

	// Version 4. A capture written before it has one frame and no churn stream.
	if (!ManifestNumber(text, "frames", &value, &found)) {
		SetError(error, "manifest.json has a malformed frames");
		return false;
	}
	manifest.frames = found ? static_cast<uint32_t>(value) : 0;

	if (!ManifestNumber(text, "churn_events", &value, &found)) {
		SetError(error, "manifest.json has a malformed churn_events");
		return false;
	}
	manifest.churn_events = found ? value : 0;

	*out = manifest;
	return true;
}

bool CaptureReader::Open(const std::filesystem::path& dir, std::string* error) {
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) {
		SetError(error, "capture directory does not exist: " + dir.string());
		return false;
	}

	std::ifstream manifest;
	if (!OpenStream(dir / "manifest.json", &manifest, error)) {
		return false;
	}
	std::ostringstream text;
	text << manifest.rdbuf();
	if (!ParseManifest(text.str(), &m_manifest, error)) {
		return false;
	}

	m_dir = dir;
	return true;
}

bool CaptureReader::ReadRanges(std::vector<RangeRecord>* out, std::string* error) const {
	std::ifstream stream;
	if (!OpenStream(m_dir / "ranges.bin", &stream, error)) {
		return false;
	}
	out->clear();
	for (;;) {
		RangeRecord record {};
		bool        at_end = false;
		if (!ReadExact(stream, &record, sizeof(record), &at_end)) {
			if (at_end) {
				return true;
			}
			SetError(error, "ranges.bin ends inside a record");
			return false;
		}
		out->push_back(record);
	}
}

bool CaptureReader::ReadDirtyPages(std::vector<uint64_t>* out, std::string* error) const {
	std::ifstream stream;
	if (!OpenStream(m_dir / "dirty-pages.bin", &stream, error)) {
		return false;
	}
	out->clear();
	for (;;) {
		uint64_t vaddr  = 0;
		bool     at_end = false;
		if (!ReadExact(stream, &vaddr, sizeof(vaddr), &at_end)) {
			if (at_end) {
				return true;
			}
			SetError(error, "dirty-pages.bin ends inside an address");
			return false;
		}
		out->push_back(vaddr);
	}
}

bool CaptureReader::ReadRegisterFiles(std::vector<CaptureRegisterFile>* out,
                                      std::string*                      error) const {
	std::ifstream stream;
	if (!OpenStream(m_dir / "registers.bin", &stream, error)) {
		return false;
	}
	out->clear();
	for (;;) {
		RegisterFileRecord header {};
		bool               at_end = false;
		if (!ReadRecordHeader(stream, &header, &at_end)) {
			if (at_end) {
				return true;
			}
			SetError(error, "registers.bin ends inside a record header");
			return false;
		}
		CaptureRegisterFile file;
		file.queue_id = header.queue_id;
		file.data.resize(header.size);
		if (!ReadExact(stream, file.data.data(), file.data.size(), nullptr)) {
			SetError(error, "registers.bin ends inside a register file");
			return false;
		}
		out->push_back(std::move(file));
	}
}

bool CaptureReader::ReadVideoOut(std::vector<CaptureVideoOut>* out, std::string* error) const {
	std::ifstream stream;
	if (!OpenStream(m_dir / "videoout.bin", &stream, error)) {
		return false;
	}
	out->clear();
	for (;;) {
		VideoOutRecord header {};
		bool           at_end = false;
		if (!ReadRecordHeader(stream, &header, &at_end)) {
			if (at_end) {
				return true;
			}
			SetError(error, "videoout.bin ends inside a record header");
			return false;
		}
		if (header.count < 0) {
			SetError(error, "videoout.bin record has a negative buffer count");
			return false;
		}
		CaptureVideoOut registration;
		registration.header = header;
		registration.attribute.resize(header.attribute_size);
		if (!ReadExact(stream, registration.attribute.data(), registration.attribute.size(),
		               nullptr)) {
			SetError(error, "videoout.bin ends inside a buffer attribute");
			return false;
		}
		registration.addresses.resize(static_cast<size_t>(header.count) * 2u);
		if (!ReadExact(stream, registration.addresses.data(),
		               registration.addresses.size() * sizeof(uint64_t), nullptr)) {
			SetError(error, "videoout.bin ends inside a buffer address list");
			return false;
		}
		out->push_back(std::move(registration));
	}
}

bool CaptureReader::ReadSubmissions(std::vector<CaptureSubmission>* out, std::string* error) const {
	std::ifstream stream;
	if (!OpenStream(m_dir / "submissions.bin", &stream, error)) {
		return false;
	}
	out->clear();
	for (;;) {
		SubmissionRecord header {};
		bool             at_end = false;
		if (!ReadRecordHeader(stream, &header, &at_end)) {
			if (at_end) {
				return true;
			}
			SetError(error, "submissions.bin ends inside a record header");
			return false;
		}
		if (header.kind > static_cast<uint32_t>(SubmissionKind::Done)) {
			SetError(error, "submissions.bin has an unknown submission kind " +
			                    std::to_string(header.kind));
			return false;
		}
		CaptureSubmission submission;
		submission.header = header;
		submission.commands.resize(header.command_dwords);
		if (!ReadExact(stream, submission.commands.data(),
		               submission.commands.size() * sizeof(uint32_t), nullptr)) {
			SetError(error, "submissions.bin ends inside a command buffer");
			return false;
		}
		submission.constants.resize(header.constant_dwords);
		if (!ReadExact(stream, submission.constants.data(),
		               submission.constants.size() * sizeof(uint32_t), nullptr)) {
			SetError(error, "submissions.bin ends inside a constant buffer");
			return false;
		}
		out->push_back(std::move(submission));
	}
}

// A stream a version 1 capture does not have: a missing file is not an error, a truncated one
// is.
template <typename T>
static bool ReadOptionalRecords(const std::filesystem::path& path, std::vector<T>* out,
                                bool* present, std::string* error, const char* what) {
	out->clear();
	*present = false;

	std::ifstream stream(path, std::ios::binary);
	if (!stream.is_open()) {
		return true;
	}
	*present = true;
	for (;;) {
		T    record {};
		bool at_end = false;
		if (!ReadExact(stream, &record, sizeof(record), &at_end)) {
			if (at_end) {
				return true;
			}
			SetError(error, std::string(what) + " ends inside a record");
			return false;
		}
		out->push_back(record);
	}
}

bool CaptureReader::ReadPrtApertures(std::vector<PrtApertureRecord>* out, bool* present,
                                     std::string* error) const {
	return ReadOptionalRecords(m_dir / "prt.bin", out, present, error, "prt.bin");
}

bool CaptureReader::ReadShaders(std::vector<ShaderRecord>* out, bool* present,
                                std::string* error) const {
	return ReadOptionalRecords(m_dir / "shaders.bin", out, present, error, "shaders.bin");
}

bool CaptureReader::ReadDirtyEvents(std::vector<DirtyEventRecord>* out, bool* present,
                                    std::string* error) const {
	if (m_manifest.format_version >= 4) {
		return ReadOptionalRecords(m_dir / "dirty-events.bin", out, present, error,
		                           "dirty-events.bin");
	}
	// A version 3 stream has no frame index and exactly one frame to belong to.
	std::vector<DirtyEventRecordV3> legacy;
	if (!ReadOptionalRecords(m_dir / "dirty-events.bin", &legacy, present, error,
	                         "dirty-events.bin")) {
		return false;
	}
	out->clear();
	out->reserve(legacy.size());
	for (const auto& record: legacy) {
		out->push_back({0, record.progress, record.submission, record.vaddr, record.size});
	}
	return true;
}

bool CaptureReader::ReadChurnEvents(std::vector<ChurnEventRecord>* out, bool* present,
                                    std::string* error) const {
	return ReadOptionalRecords(m_dir / "churn-events.bin", out, present, error, "churn-events.bin");
}

bool CaptureReader::ForEachPage(const PageSink& sink, uint64_t* pages, std::string* error) const {
	std::ifstream stream;
	if (!OpenStream(m_dir / "memory.bin", &stream, error)) {
		return false;
	}
	std::vector<uint8_t> page(kPageSize);
	uint64_t             count = 0;
	for (;;) {
		uint64_t vaddr  = 0;
		bool     at_end = false;
		if (!ReadExact(stream, &vaddr, sizeof(vaddr), &at_end)) {
			if (at_end) {
				if (pages != nullptr) {
					*pages = count;
				}
				return true;
			}
			SetError(error, "memory.bin ends inside a page address");
			return false;
		}
		if (!ReadExact(stream, page.data(), page.size(), nullptr)) {
			SetError(error, "memory.bin ends inside page data");
			return false;
		}
		if (!sink(vaddr, page.data())) {
			SetError(error, "memory.bin page could not be restored");
			return false;
		}
		count++;
	}
}

} // namespace Libs::Graphics::Replay
