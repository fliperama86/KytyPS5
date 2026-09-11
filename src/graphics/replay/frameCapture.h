#ifndef EMULATOR_SRC_GRAPHICS_REPLAY_FRAMECAPTURE_H_
#define EMULATOR_SRC_GRAPHICS_REPLAY_FRAMECAPTURE_H_

// Capture side of the frame replay harness (docs/frame-replay.md, phase A). Everything here is
// inert unless --frame-capture named a directory; the game path only ever pays the Armed() test.

#include "common/common.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/replay/frameCaptureFormat.h"

#include <atomic>
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

namespace Detail {
// True while a capture is armed and has not been written. Read on the CPU-dirty marking path,
// which runs tens of thousands of times per frame, so it is a plain relaxed load with no
// function-local static behind it.
extern std::atomic_bool g_dirty_events_armed;
// The same for the buffer and mapping churn path (format version 4).
extern std::atomic_bool g_churn_events_armed;
} // namespace Detail

// Appends one CPU-dirty mark {progress, submission, vaddr, size} to the frame (format version 3,
// docs/frame-replay.md). Called from BufferCache's invalidation path, which is where the host
// page-fault handler and the kernel write paths both land. Inert, and one relaxed atomic load,
// unless --frame-capture armed a capture.
void RecordDirtyEventSlow(uint64_t vaddr, uint64_t size);
inline void RecordDirtyEvent(uint64_t vaddr, uint64_t size) {
	if (Detail::g_dirty_events_armed.load(std::memory_order_relaxed)) {
		RecordDirtyEventSlow(vaddr, size);
	}
}

// One GpuResourceManager::PrepareBda call, reported to whoever is watching: the capture, which
// writes it to prepare-events.bin, or a replay, which counts it (format version 5,
// docs/frame-replay.md, phase E). Both need the same call, and neither is on in a normal run, so
// it is a single relaxed load of a null pointer there.
using PrepareSink = void (*)(bool scanned, uint32_t dirty_ranges, uint32_t synchronized,
                             uint64_t dirty_bytes, uint32_t scan_ns);

namespace Detail {
extern std::atomic<PrepareSink> g_prepare_sink;
} // namespace Detail

// Installs the sink; null removes it. The capture installs its own when it arms.
void SetPrepareSink(PrepareSink sink) noexcept;

inline void RecordPrepareEvent(bool scanned, uint32_t dirty_ranges, uint32_t synchronized,
                               uint64_t dirty_bytes, uint32_t scan_ns) {
	if (auto* sink = Detail::g_prepare_sink.load(std::memory_order_relaxed); sink != nullptr) {
		sink(scanned, dirty_ranges, synchronized, dirty_bytes, scan_ns);
	}
}

// True when somebody is watching the preparations, so the scan is worth timing. One relaxed load;
// without it PrepareBda would pay two clock reads per draw in a normal run.
[[nodiscard]] inline bool PrepareEventsWatched() noexcept {
	return Detail::g_prepare_sink.load(std::memory_order_relaxed) != nullptr;
}

// Appends one BDA-generation bump that is not a CPU write: a buffer registration or retirement,
// or a guest map or unmap (format version 4, docs/frame-replay.md, phase E). Diagnostics only --
// the replay does not consume the stream; it is what says whether the guest's buffer churn has a
// short enough period for a looped sequence of frames to reproduce it. Inert, and one relaxed
// atomic load, unless --frame-capture armed a capture.
void RecordChurnEventSlow(ChurnEventKind kind, uint64_t vaddr, uint64_t size);
inline void RecordChurnEvent(ChurnEventKind kind, uint64_t vaddr, uint64_t size) {
	if (Detail::g_churn_events_armed.load(std::memory_order_relaxed)) {
		RecordChurnEventSlow(kind, vaddr, size);
	}
}

// Copies the dwords and returns the capture id to carry on the Submission; 0 when not recording.
[[nodiscard]] uint64_t RecordEnqueue(SubmissionKind kind, uint32_t queue_id,
                                     std::span<const uint32_t> commands,
                                     std::span<const uint32_t> constants, uint64_t flip_request_id);
// Appends the submission to the frame in the order the GPU thread started processing it.
void RecordStarted(uint64_t capture_id);
// Appends the Done marker that closes a frame, carrying that frame's GuestGpu number and its
// progress count (draws plus dispatches), and moves the recorder on to the next frame.
void RecordDone(int frame_num, uint32_t progress);
// Drops everything recorded so far and starts the next frame at index 0.
void ResetFrame() noexcept;

// What Done() must do with the frame that just ended. A capture of K frames
// (--frame-capture-frames) keeps K frames' records and writes at the end of the last one; K is 1
// by default, which is the version 1 to 3 behaviour.
enum class FrameDisposition {
	Discard, // outside the capture window: drop what the frame recorded
	Keep,    // inside it, more frames to come: keep the records and go on
	Write,   // the window is complete: write the capture now
};

// Decides the frame that just ended. Tests the trigger file at most once per Done(), and only
// until it fires.
[[nodiscard]] FrameDisposition OnFrameDone(int frame_num);

// One command processor's register file, as GuestGpu holds it.
struct ProcessorRegisters {
	uint32_t              queue_id = 0;
	const HW::Context*    context  = nullptr;
	const HW::UserConfig* config   = nullptr;
	const HW::Shader*     shader   = nullptr;
};

// Flushes the GPU caches into guest memory and writes the whole capture directory. GPU thread
// only, with the submission queues already drained. `frame_num` is the frame whose end the
// snapshot is taken at, the last of the captured window. Returns true when the directory is
// complete.
bool WriteCapture(RenderContext& renderer, int frame_num,
                  std::span<const ProcessorRegisters> processors);

// True when the capture must end the process (--frame-capture-exit, default true).
[[nodiscard]] bool ExitAfterCapture() noexcept;

} // namespace Libs::Graphics::Replay

#endif // EMULATOR_SRC_GRAPHICS_REPLAY_FRAMECAPTURE_H_
