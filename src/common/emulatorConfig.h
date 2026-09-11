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
std::filesystem::path GetReplayImage();

ThreadAffinity GetThreadAffinity();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled();
#endif

const Keymap& GetKeymap();

} // namespace Config

#endif /* KYTY_COMMON_EMULATOR_CONFIG_H_ */
