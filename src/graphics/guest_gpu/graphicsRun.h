#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/threads.h"
#include "common/uniqueFunction.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

namespace Libs::Graphics {

class RenderContext;

class GuestGpu final {
public:
	explicit GuestGpu(RenderContext& renderer);
	~GuestGpu();
	KYTY_CLASS_NO_COPY(GuestGpu);

	void               Shutdown();
	[[nodiscard]] bool IsStopping();
	void               SendCommand(Common::UniqueFunction<void>&& command);
	void               SendCommandSync(Common::UniqueFunction<void>&& command);

	// Submitted command memory is borrowed and must remain valid until GPU execution completes.
	void              Submit(std::span<const uint32_t> draw_commands,
	                         std::span<const uint32_t> constant_commands);
	void              SubmitCompute(uint32_t queue, std::span<const uint32_t> commands);
	void              SubmitFlipPreparation(uint64_t request_id);
	void              Done();
	[[nodiscard]] int GetFrameNum() const;

	// Blocks the calling thread until the GPU thread has drained every queue. Not callable from
	// the GPU thread itself.
	void WaitForIdle();
	// The same with a deadline; false means the GPU thread is still busy. Frame replay uses it to
	// catch a WAIT_REG_MEM that never completes instead of spinning (docs/frame-replay.md).
	bool WaitForIdleFor(uint32_t timeout_ms);

	// Frame replay: the recorded register-file layout of one command processor, Context then
	// UserConfig then Shader, and the number of processors a capture may carry.
	[[nodiscard]] static size_t   RegisterFileSize() noexcept;
	[[nodiscard]] static uint32_t RegisterFileQueueCount() noexcept { return QueueCount; }
	// Copies recorded register bytes into one command processor. GPU thread only; the replay
	// reaches it through SendCommandSync.
	bool RestoreRegisterFile(uint32_t queue_id, const void* data, size_t size);

	[[nodiscard]] static bool IsGpuThread() noexcept;

	// The frame's progress clock (docs/frame-replay.md, phases D and E). Relaxed atomic
	// increments on the GPU thread, reset by Done(): one when a draw or dispatch starts and one
	// after its resource preparation stage, so a CPU write that arrives before a draw's
	// GpuResourceManager::PrepareBda carries a different value from one that arrives after it.
	// (The second tick is unconditional, not tied to whether the stage needed a BDA preparation,
	// so the clock does not depend on --gpu-descriptors.) It is the only always-on addition the
	// CPU-write timing needed: a counter that means the same thing in a game run and in a replay,
	// which wall time does not. The frame capture keys every CPU-dirty mark to it and the replay
	// applies each mark when the clock reaches its value, so the marks land at the same point in
	// the GPU thread's work and the BDA generation moves as often as it does in the game.
	[[nodiscard]] static uint32_t Progress() noexcept {
		return s_progress.load(std::memory_order_relaxed);
	}
	// Called with the new value on the GPU thread every time the clock ticks; the frame replay
	// installs one to apply the recorded CPU writes of that tick inline, on the thread and at the
	// point the game's own writes reached the cache. Null outside a replay, one relaxed load.
	using ProgressHook = void (*)(uint32_t progress);
	static void SetProgressHook(ProgressHook hook) noexcept {
		s_progress_hook.store(hook, std::memory_order_release);
	}
	static void BumpProgress() noexcept {
		const auto value = s_progress.fetch_add(1, std::memory_order_relaxed) + 1;
		if (auto* hook = s_progress_hook.load(std::memory_order_relaxed); hook != nullptr) {
			hook(value);
		}
	}
	static void ResetProgress() noexcept { s_progress.store(0, std::memory_order_relaxed); }
	// Index of the submission the GPU thread has started, also reset by Done(). Recorded next to
	// a dirty event so the stream can be read by hand; nothing depends on it.
	[[nodiscard]] static uint32_t SubmissionIndex() noexcept {
		return s_submission_index.load(std::memory_order_relaxed);
	}

private:
	static constexpr uint32_t ComputePipeCount     = 7;
	static constexpr uint32_t QueuesPerComputePipe = 8;
	static constexpr uint32_t ComputeQueueCount    = ComputePipeCount * QueuesPerComputePipe;
	static constexpr uint32_t ComputeQueueBase     = 0x20;
	static constexpr uint32_t QueueCount           = 1 + ComputeQueueCount;

	enum class SubmissionType { Graphics, Compute, FlipPreparation };

	struct Submission {
		SubmissionType            type     = SubmissionType::Graphics;
		uint32_t                  queue_id = 0;
		std::span<const uint32_t> commands;
		std::span<const uint32_t> constant_commands;
		Pm4Execution              command_execution;
		Pm4Execution              constant_execution;
		bool                      reset_processor   = false;
		bool                      started           = false;
		bool                      command_complete  = false;
		bool                      constant_complete = false;
		bool                      blocked           = false;
		uint64_t                  flip_request_id   = 0;
		// Frame capture id (docs/frame-replay.md); 0 unless --frame-capture is recording.
		uint64_t capture_id = 0;
	};

	void              Enqueue(Submission submission);
	void              CaptureFrame(int frame_num);
	void              ProcessCommands();
	bool              Process(Submission& submission);
	static void       ThreadRun(void* data);
	CommandProcessor& GetProcessor(uint32_t queue_id);

	RenderContext&                                 m_renderer;
	Common::Mutex                                  m_submission_mutex;
	Common::Mutex                                  m_queue_mutex;
	std::mutex                                     m_shutdown_mutex;
	Common::CondVar                                m_work_available;
	Common::CondVar                                m_idle;
	std::array<std::deque<Submission>, QueueCount> m_queues;
	std::deque<Common::UniqueFunction<void>>       m_commands;
	std::atomic_uint32_t                           m_pending_commands {0};
	uint32_t                                       m_next_queue        = 0;
	uint32_t                                       m_submission_count  = 0;
	bool                                           m_processing        = false;
	bool                                           m_graphics_done     = true;
	bool                                           m_accepting         = true;
	bool                                           m_stopping          = false;
	bool                                           m_shutdown_complete = false;

	std::unique_ptr<CommandProcessor>                                m_gfx_cp;
	std::array<std::unique_ptr<CommandProcessor>, ComputeQueueCount> m_compute_cp;

	uint64_t        m_submit_id = 0;
	std::atomic_int m_done_num  = 0;
	std::jthread    m_thread;

	// Written only by the GPU thread, read by any thread; see Progress() above.
	inline static std::atomic_uint32_t              s_progress {0};
	inline static std::atomic<GuestGpu::ProgressHook> s_progress_hook {nullptr};
	inline static std::atomic_uint32_t s_submission_index {0};

	friend class CommandProcessor;
};
} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GRAPHICSRUN_H_ */
