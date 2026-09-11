#ifndef EMULATOR_SRC_GRAPHICS_REPLAY_FRAMECAPTURE_H_
#define EMULATOR_SRC_GRAPHICS_REPLAY_FRAMECAPTURE_H_

// Capture side of the frame replay harness (docs/frame-replay.md, phase A). Everything here is
// inert unless --frame-capture named a directory; the game path only ever pays the Armed() test.

#include "common/common.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/replay/frameCaptureFormat.h"

#include <cstdint>
#include <span>

namespace Libs::Graphics {
class RenderContext;
} // namespace Libs::Graphics

namespace Libs::Graphics::Replay {

// True while --frame-capture is on and no capture has been written yet. One relaxed atomic load.
[[nodiscard]] bool Armed() noexcept;

// True when the submissions of the frame the GPU is on must be recorded. `frame_num` is
// GuestGpu::GetFrameNum() at submit time.
[[nodiscard]] bool RecordingFrame(int frame_num) noexcept;

// Copies the dwords and returns the capture id to carry on the Submission; 0 when not recording.
[[nodiscard]] uint64_t RecordEnqueue(SubmissionKind kind, uint32_t queue_id,
                                     std::span<const uint32_t> commands,
                                     std::span<const uint32_t> constants, uint64_t flip_request_id);
// Appends the submission to the frame in the order the GPU thread started processing it.
void RecordStarted(uint64_t capture_id);
// Appends the Done marker that closes the frame.
void RecordDone();
// Drops everything recorded for the frame that just ended.
void ResetFrame() noexcept;

// True when the Done() that ends `frame_num` must write the capture. Tests the trigger file at
// most once per Done().
[[nodiscard]] bool ShouldCapture(int frame_num);

// One command processor's register file, as GuestGpu holds it.
struct ProcessorRegisters {
	uint32_t              queue_id = 0;
	const HW::Context*    context  = nullptr;
	const HW::UserConfig* config   = nullptr;
	const HW::Shader*     shader   = nullptr;
};

// Flushes the GPU caches into guest memory and writes the whole capture directory. GPU thread
// only, with the submission queues already drained. Returns true when the directory is complete.
bool WriteCapture(RenderContext& renderer, int frame_num,
                  std::span<const ProcessorRegisters> processors);

// True when the capture must end the process (--frame-capture-exit, default true).
[[nodiscard]] bool ExitAfterCapture() noexcept;

} // namespace Libs::Graphics::Replay

#endif // EMULATOR_SRC_GRAPHICS_REPLAY_FRAMECAPTURE_H_
