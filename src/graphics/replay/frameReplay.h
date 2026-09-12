#ifndef EMULATOR_SRC_GRAPHICS_REPLAY_FRAMEREPLAY_H_
#define EMULATOR_SRC_GRAPHICS_REPLAY_FRAMEREPLAY_H_

#include "common/emulatorConfig.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace Libs::Graphics::Replay {

// Restores the capture in `dir` and feeds its frames through the real queue `loops` times, each
// loop replaying the *last* `frames` recorded frames in order (0 means every frame the capture
// holds). The last is the one the memory snapshot was taken at the end of, so it is the frame
// whose labels and ring slots the restore is consistent with; earlier frames are further from it. Reports milliseconds per loop and per frame on stdout and in replay-report.json next to
// the capture. Returns the process exit code: 0 when the replay ran, non-zero on any failure.
// See docs/frame-replay.md.
// `dirty_set_once` marks the recorded CPU-dirty page set at restore only, instead of before every
// replayed frame; the frame's recorded CPU-write events are applied per frame either way.
// `spin_threads` host threads busy-wait on the guest CPU set for the length of the run, imitating
// the game's job-system workers, which spin in guest code while the render thread works.
// `writer`, when it is not None, is one thread on that CPU group which rewrites the bytes of every
// dirty event's range immediately before the replay marks it, so the BDA scan that follows copies
// cache lines another core dirtied moments earlier (docs/bda-sync-design.md, step 0).
int RunReplay(const std::filesystem::path& dir, uint32_t loops, uint32_t frames,
              bool dirty_set_once, uint32_t spin_threads, Config::ReplayWriter writer,
              const std::filesystem::path& image);

// WAIT_REG_MEM diagnostics, so a wait that never completes ends the replay with the address and
// the compare parameters instead of spinning. Off outside a replay; the recording call is behind
// the check, which is one relaxed atomic load on the GPU thread.
[[nodiscard]] bool IsWaitDiagnosticsEnabled() noexcept;
void        RecordBlockedWait(uint64_t address, uint64_t value, uint64_t reference, uint64_t mask,
                              uint32_t function, uint32_t width);
std::string DescribeLastBlockedWait();

} // namespace Libs::Graphics::Replay

#endif // EMULATOR_SRC_GRAPHICS_REPLAY_FRAMEREPLAY_H_
