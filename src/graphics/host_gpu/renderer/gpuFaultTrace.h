#pragma once

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace Libs::Graphics::GpuFaultTrace {

// Diagnostic-only, bounded CPU history. Copy binding state while it is live;
// device-loss reporting must not inspect caches or query allocations afterward.
inline std::mutex history_mutex;
inline std::deque<std::string> history;
inline uint64_t sequence = 0;

inline bool Enabled() {
	static const bool enabled = std::getenv("KYTY_DEBUG_RESOURCE_TRACE") != nullptr;
	return enabled;
}

inline void Record(std::string entry) {
	std::lock_guard lock(history_mutex);
	entry = "dispatch_sequence=" + std::to_string(++sequence) + " " + entry;
	if (sequence == 1) {
		std::printf("GPU resource trace first entry:\n%s", entry.c_str());
		std::fflush(stdout);
	}
	if (history.size() == 128) {
		history.pop_front();
	}
	history.push_back(std::move(entry));
}

inline void Dump() {
	const char* path = std::getenv("KYTY_DEBUG_RESOURCE_TRACE");
	if (path == nullptr) {
		return;
	}
	std::lock_guard lock(history_mutex);
	if (FILE* file = std::fopen(path, "wb")) {
		bool ok = true;
		for (const auto& entry : history) {
			ok &= std::fwrite(entry.data(), 1, entry.size(), file) == entry.size();
		}
		ok &= std::fclose(file) == 0;
		std::printf("GPU resource trace: %s entries=%zu path=%s\n",
		            ok ? "saved" : "write failed", history.size(), path);
	} else {
		std::printf("GPU resource trace: cannot open %s\n", path);
	}
}

} // namespace Libs::Graphics::GpuFaultTrace
