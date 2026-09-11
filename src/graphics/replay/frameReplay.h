#ifndef EMULATOR_SRC_GRAPHICS_REPLAY_FRAMEREPLAY_H_
#define EMULATOR_SRC_GRAPHICS_REPLAY_FRAMEREPLAY_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Libs::Graphics::Replay {

// Restores the capture in `dir`, feeds its frame through the real queue `loops` times and reports
// milliseconds per loop on stdout and in replay-report.json next to the capture. Returns the
// process exit code: 0 when the replay ran, non-zero on any failure. See docs/frame-replay.md.
int RunReplay(const std::filesystem::path& dir, uint32_t loops, const std::filesystem::path& image);

// WAIT_REG_MEM diagnostics, so a wait that never completes ends the replay with the address and
// the compare parameters instead of spinning. Off outside a replay; the recording call is behind
// the check, which is one relaxed atomic load on the GPU thread.
[[nodiscard]] bool IsWaitDiagnosticsEnabled() noexcept;
void        RecordBlockedWait(uint64_t address, uint64_t value, uint64_t reference, uint64_t mask,
                              uint32_t function, uint32_t width);
std::string DescribeLastBlockedWait();

} // namespace Libs::Graphics::Replay

#endif // EMULATOR_SRC_GRAPHICS_REPLAY_FRAMEREPLAY_H_
