#include "common/emulatorConfig.h"

#include "common/assert.h"

#include <algorithm>
#include <memory>

namespace Config {

static std::unique_ptr<ConfigOptions> g_config;

void Initialize() {
	EXIT_IF(g_config != nullptr);

	g_config = std::make_unique<ConfigOptions>();
}

void Shutdown() {
	g_config.reset();
}

void Load(const ConfigOptions& cfg) {
	EXIT_IF(g_config == nullptr);
	EXIT_IF(cfg.user_name.empty() || cfg.user_name.size() > MAX_USER_NAME_LENGTH);
	EXIT_IF(!IsConfiguredUserIdValid(cfg.user_id));

	*g_config = cfg;
}

uint32_t GetScreenWidth() {
	return g_config->screen_width;
}

uint32_t GetScreenHeight() {
	return g_config->screen_height;
}

const std::string& GetUserName() {
	return g_config->user_name;
}

int32_t GetUserId() {
	return g_config->user_id;
}

PresentMode GetPresentMode() {
	return g_config->present_mode;
}

int32_t GetGpuIndex() {
	return g_config->gpu_index;
}

bool FullscreenEnabled() {
	return g_config->fullscreen_enabled;
}

uint32_t GetVblankFrequency() {
	return std::clamp(g_config->vblank_frequency, 30u, 360u);
}

uint32_t GetConsoleLanguage() {
	return g_config->console_language;
}

bool VulkanValidationEnabled() {
	return g_config->vulkan_validation_enabled;
}

bool ShaderValidationEnabled() {
	return g_config->shader_validation_enabled;
}

bool ShaderLdsWaitcntBarrierEnabled() {
	return g_config->shader_lds_waitcnt_barrier_enabled;
}

bool ShaderStorageImageBoundsCheckEnabled() {
	return g_config->shader_storage_image_bounds_check_enabled;
}

bool GpuDescriptorsEnabled() {
	return g_config->gpu_descriptors_enabled;
}

bool GpuSrtReadsEnabled() {
	return g_config->gpu_descriptors_enabled && g_config->gpu_srt_reads_enabled;
}

bool GpuPrologueTableEnabled() {
	return g_config->gpu_prologue_table_enabled;
}

bool GpuIndirectEnabled() {
	return g_config->gpu_indirect_enabled;
}

bool GpuIndirectDrawsEnabled() {
	return g_config->gpu_indirect_draws_enabled;
}

bool GpuReadbackProducerWaitEnabled() {
	return g_config->gpu_readback_producer_wait_enabled;
}

bool GpuReadbackDiagnosticsEnabled() {
	return g_config->gpu_readback_diagnostics_enabled;
}

uint32_t GetGpuSubmitInterval() {
	return g_config->gpu_submit_interval;
}

bool GpuSubmitAfterWritesEnabled() {
	return g_config->gpu_submit_after_writes_enabled;
}

bool BdaAsyncProtectEnabled() {
	return g_config->bda_async_protect_enabled;
}

BdaAsyncProtectAffinity GetBdaAsyncProtectAffinity() {
	return g_config->bda_async_protect_affinity;
}

ShaderOptimizationType GetShaderOptimizationType() {
	return g_config->shader_optimization_type;
}

ShaderLogDirection GetShaderLogDirection() {
	return g_config->shader_log_direction;
}

std::filesystem::path GetShaderLogFolder() {
	return g_config->shader_log_folder;
}

bool CommandBufferDumpEnabled() {
	return g_config->command_buffer_dump_enabled;
}

std::filesystem::path GetCommandBufferDumpFolder() {
	return g_config->command_buffer_dump_folder;
}

bool FrameCaptureEnabled() {
	return g_config->frame_capture_enabled;
}

std::filesystem::path GetFrameCaptureFolder() {
	return g_config->frame_capture_folder;
}

int32_t GetFrameCaptureFrame() {
	return g_config->frame_capture_at;
}

uint32_t GetFrameCaptureFrames() {
	return g_config->frame_capture_frames == 0 ? 1u : g_config->frame_capture_frames;
}

bool FrameCaptureExitEnabled() {
	return g_config->frame_capture_exit;
}

bool GraphicsDebugDumpEnabled() {
	return g_config->graphics_debug_dump_enabled;
}

OutputDirection GetPrintfDirection() {
	return g_config->printf_direction;
}

std::filesystem::path GetPrintfOutputFile() {
	return g_config->printf_output_file;
}

ProfilerDirection GetProfilerDirection() {
	return g_config->profiler_direction;
}

bool SpirvDebugPrintfEnabled() {
	return g_config->spirv_debug_printf_enabled;
}

bool GpuAssistedValidationEnabled() {
	return g_config->gpu_assisted_validation_enabled && g_config->vulkan_validation_enabled;
}

bool RenderDocEnabled() {
	return g_config->renderdoc_enabled;
}

bool ReadbackLinearImagesEnabled() {
	return g_config->readback_linear_images;
}

bool PlayGoHackEnabled() {
	return g_config->playgo_hack_enabled;
}

bool ReplayEnabled() {
	return !g_config->replay_dir.empty();
}

std::filesystem::path GetReplayDir() {
	return g_config->replay_dir;
}

uint32_t GetReplayLoops() {
	return g_config->replay_loops == 0 ? 1u : g_config->replay_loops;
}

uint32_t GetReplayFrames() {
	return g_config->replay_frames;
}

bool ReplayDirtySetOnce() {
	return g_config->replay_dirty_set_once;
}

uint32_t GetReplaySpinThreads() {
	return g_config->replay_spin_threads;
}

ReplayWriter GetReplayWriter() {
	return g_config->replay_writer;
}

std::filesystem::path GetReplayImage() {
	return g_config->replay_image;
}

ThreadAffinity GetThreadAffinity() {
	return g_config->thread_affinity;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
bool RedZoneProtectionEnabled() {
	return g_config->red_zone_protection_enabled;
}
#endif

const Keymap& GetKeymap() {
	return g_config->keymap;
}

} // namespace Config
