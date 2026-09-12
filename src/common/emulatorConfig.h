#ifndef KYTY_COMMON_EMULATOR_CONFIG_H_
#define KYTY_COMMON_EMULATOR_CONFIG_H_

#include "common/common.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Config {

void Initialize();
void Shutdown();

struct Lifecycle {
	static constexpr const char* name       = "Config";
	static constexpr auto        initialize = Config::Initialize;
	static constexpr auto        shutdown   = Config::Shutdown;
};

enum class ShaderOptimizationType { None, Size, Performance };

enum class ShaderLogDirection { Silent, Console, File };

enum class ProfilerDirection { None, Network };

enum class OutputDirection { Silent, Console, File };

enum class PresentMode { Fifo, Mailbox, Immediate };

// Auto derives the render/present and guest CPU masks from the host L3 cache topology at startup;
// None leaves every thread where the scheduler puts it.
enum class ThreadAffinity { Auto, None };

// Which CPU group the replay's memory writer runs on, or None for no writer at all. It rewrites
// the bytes of a dirty event's range just before the replay marks it, so the BDA scan that
// follows copies lines another core wrote moments earlier (docs/bda-sync-design.md, step 0).
enum class ReplayWriter { None, Guest, Render };

// Which CPU group design P's re-protection helper thread runs on
// (docs/bda-sync-design.md, --bda-async-protect).
enum class BdaAsyncProtectAffinity { Guest, Render };

using Keymap = std::vector<std::string>;

constexpr uint32_t DEFAULT_CONSOLE_LANGUAGE = 1;
constexpr uint32_t MAX_CONSOLE_LANGUAGE     = 29;
constexpr std::size_t MAX_USER_NAME_LENGTH = 16;
constexpr int32_t DEFAULT_USER_ID           = 1000;
constexpr uint32_t DEFAULT_REPLAY_LOOPS     = 20;

constexpr bool IsConfiguredUserIdValid(int32_t user_id) {
	constexpr int32_t USER_ID_EVERYONE = 0xfe;
	constexpr int32_t USER_ID_SYSTEM   = 0xff;
	return user_id >= 0 && user_id != USER_ID_EVERYONE && user_id != USER_ID_SYSTEM;
}

struct ConfigOptions {
	uint32_t               screen_width                = 1280;
	uint32_t               screen_height               = 720;
	std::string            user_name                   = "Kyty";
	int32_t                user_id                     = DEFAULT_USER_ID;
	PresentMode            present_mode                = PresentMode::Fifo;
	int32_t                gpu_index                   = -1;
	bool                   fullscreen_enabled          = false;
	uint32_t               vblank_frequency            = 60;
	uint32_t               console_language            = DEFAULT_CONSOLE_LANGUAGE;
	bool                   vulkan_validation_enabled   = false;
	bool                   shader_validation_enabled   = false;
	bool                   shader_lds_waitcnt_barrier_enabled = false;
	bool                   shader_storage_image_bounds_check_enabled = true;
	// Shaders evaluate eligible buffer descriptors in-shader (docs/gpu-descriptor-fetch.md).
	bool                   gpu_descriptors_enabled = false;
	// Stage 1b of the same document: the flattened SRT scalar reads are lowered into the shader
	// prologue too, so the render thread stops evaluating them. Needs gpu_descriptors_enabled.
	bool                   gpu_srt_reads_enabled = false;
	// Step 1 of the artifact-free path (docs/sync-points-design.md): a second BDA page table for
	// the shader prologue whose default entry is the guest page itself, imported with
	// VK_EXT_external_memory_host, so a descriptor or SRT read can never miss. The data table is
	// unchanged. Off, nothing is imported and no shader carries the binding.
	bool                   gpu_prologue_table_enabled = false;
	// Step 2 of the same document: a compute program with side effects (a written or atomic
	// buffer or image, or uses_dma) evaluates its read-only descriptor roots and its flattened
	// SRT reads in the shader prologue like any other program, instead of being excluded from
	// stage 1 as a whole. Needs gpu_descriptors_enabled and gpu_prologue_table_enabled: without
	// the prologue table a root read can miss and a producer would store a zero descriptor.
	bool                   gpu_fetch_side_effects_enabled = false;
	// Vulkan consumes indirect draw and dispatch arguments in place instead of the render thread
	// reading them back from the GPU (docs/performance-roadmap.md, item 1).
	bool                   gpu_indirect_enabled = false;
	// The draw half of the same item. Separate because it measures neutral in replay: it removes
	// the argument syncs but pays a buffer-cache lookup per draw instead.
	bool                   gpu_indirect_draws_enabled = false;
	// Item 1b of docs/performance-roadmap.md: a readback taken inside an SRT evaluation waits only
	// for the submission that produced the page, on its own command buffer, instead of draining
	// everything the render thread has recorded since the last drain.
	bool                   gpu_readback_producer_wait_enabled = false;
	// The characterisation behind that item: count the render thread's read faults, time their
	// download and wait, and record what was being evaluated and which submission produced the
	// page. Off in every measured run that is not the characterisation itself.
	bool                   gpu_readback_diagnostics_enabled = false;
	// "Periodic submits" of item 1b: after every N draws and dispatches the render thread records,
	// the open command buffer is ended and submitted without a wait, so the device executes it
	// while the CPU keeps recording. 0 keeps the old behaviour, one submission per guest
	// submission or drain.
	uint32_t               gpu_submit_interval = 0;
	// The same, additionally right after any draw or dispatch that claimed a range for the GPU
	// (ObtainBuffer with is_written), so a producer the render thread is about to read starts
	// executing at once.
	bool                   gpu_submit_after_writes_enabled = false;
	// Design P of docs/bda-sync-design.md: the memory tracker stops re-protecting the pages a BDA
	// scan uploads on the render thread and hands them to a helper thread, which protects them in
	// batches; the next scan uploads each landed page once more. Off, the tracker is unchanged.
	bool                   bda_async_protect_enabled = false;
	// Which CPU group that helper thread runs on. Measured both ways, see the design document.
	BdaAsyncProtectAffinity bda_async_protect_affinity = BdaAsyncProtectAffinity::Guest;
	ShaderOptimizationType shader_optimization_type    = ShaderOptimizationType::None;
	ShaderLogDirection     shader_log_direction        = ShaderLogDirection::Silent;
	std::filesystem::path  shader_log_folder           = "_Shaders";
	bool                   command_buffer_dump_enabled = false;
	std::filesystem::path  command_buffer_dump_folder  = "_Buffers";
	// Frame capture for the replay harness (docs/frame-replay.md). Off unless a folder is
	// given; frame_capture_at < 0 waits for a file named "trigger" inside that folder.
	// frame_capture_frames is how many consecutive frames are recorded from there on.
	bool                  frame_capture_enabled = false;
	std::filesystem::path frame_capture_folder;
	int32_t               frame_capture_at     = -1;
	uint32_t              frame_capture_frames = 1;
	bool                  frame_capture_exit   = true;
	bool                   graphics_debug_dump_enabled = false;
	OutputDirection        printf_direction            = OutputDirection::Silent;
	std::filesystem::path  printf_output_file          = "_kyty.txt";
	ProfilerDirection      profiler_direction          = ProfilerDirection::None;
	bool                   spirv_debug_printf_enabled  = false;
	bool                   gpu_assisted_validation_enabled = false;
	bool                   renderdoc_enabled           = false;
	bool                   readback_linear_images      = false;
	bool                   playgo_hack_enabled         = false;
	// Frame replay (docs/frame-replay.md). An empty replay_dir leaves every replay path inert.
	std::filesystem::path  replay_dir;
	uint32_t               replay_loops                = DEFAULT_REPLAY_LOOPS;
	// How many of the capture's frames one loop replays; 0 means all of them.
	uint32_t               replay_frames               = 0;
	// Whether the recorded CPU-dirty page set is re-marked only once at restore (true, the
	// default: it is what the game's own BDA scans see) or before every replayed frame (false,
	// the phase B to D behaviour). See docs/frame-replay.md, phase E.
	bool                   replay_dirty_set_once       = true;
	// Host threads that spin on the guest CPUs for the length of a replay, imitating the game's
	// job-system workers, which busy-wait in guest code. 0 leaves the machine to the replay.
	uint32_t               replay_spin_threads         = 0;
	// How long a replayed loop may take before the replay calls the GPU thread hung, warm-up and
	// measured loops alike. The defaults are 600 s for the warm-up, which compiles every pipeline
	// the frame touches, and 2 s for a measured loop; a configuration whose loop is slower than
	// that -- --gpu-fetch-side-effects, whose prologues are long -- needs this to be measured at
	// all. 0 keeps the defaults.
	uint32_t               replay_timeout_ms           = 0;
	// The replay's memory writer: none, or one thread on the guest or the render CPU group that
	// rewrites each dirty event's range immediately before the mark. See docs/bda-sync-design.md.
	ReplayWriter           replay_writer               = ReplayWriter::None;
	std::filesystem::path  replay_image;
	ThreadAffinity         thread_affinity             = ThreadAffinity::Auto;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	bool red_zone_protection_enabled = false;
#endif
	Keymap keymap;
};

void Load(const ConfigOptions& cfg);

uint32_t GetScreenWidth();
uint32_t GetScreenHeight();
const std::string& GetUserName();
int32_t  GetUserId();
PresentMode GetPresentMode();
int32_t GetGpuIndex();
bool     FullscreenEnabled();
uint32_t GetVblankFrequency();
uint32_t GetConsoleLanguage();
bool     VulkanValidationEnabled();

bool                   ShaderValidationEnabled();
bool                   ShaderLdsWaitcntBarrierEnabled();
bool                   ShaderStorageImageBoundsCheckEnabled();
bool                   GpuDescriptorsEnabled();
bool                   GpuSrtReadsEnabled();
bool                   GpuPrologueTableEnabled();
bool                   GpuFetchSideEffectsEnabled();
bool                   GpuIndirectEnabled();
bool                   GpuIndirectDrawsEnabled();
bool                   GpuReadbackProducerWaitEnabled();
bool                   GpuReadbackDiagnosticsEnabled();
uint32_t               GetGpuSubmitInterval();
bool                   GpuSubmitAfterWritesEnabled();
bool                   BdaAsyncProtectEnabled();
BdaAsyncProtectAffinity GetBdaAsyncProtectAffinity();
ShaderOptimizationType GetShaderOptimizationType();
ShaderLogDirection     GetShaderLogDirection();
std::filesystem::path  GetShaderLogFolder();

bool                  CommandBufferDumpEnabled();
std::filesystem::path GetCommandBufferDumpFolder();

bool                  FrameCaptureEnabled();
std::filesystem::path GetFrameCaptureFolder();
int32_t               GetFrameCaptureFrame();
uint32_t              GetFrameCaptureFrames();
bool                  FrameCaptureExitEnabled();

bool GraphicsDebugDumpEnabled();

OutputDirection       GetPrintfDirection();
std::filesystem::path GetPrintfOutputFile();

ProfilerDirection GetProfilerDirection();

bool SpirvDebugPrintfEnabled();

bool GpuAssistedValidationEnabled();

bool RenderDocEnabled();
bool ReadbackLinearImagesEnabled();
bool PlayGoHackEnabled();

bool                  ReplayEnabled();
std::filesystem::path GetReplayDir();
uint32_t              GetReplayLoops();
uint32_t              GetReplayFrames();
bool                  ReplayDirtySetOnce();
uint32_t              GetReplaySpinThreads();
uint32_t              GetReplayTimeoutMs();
ReplayWriter          GetReplayWriter();
std::filesystem::path GetReplayImage();

ThreadAffinity GetThreadAffinity();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled();
#endif

const Keymap& GetKeymap();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
