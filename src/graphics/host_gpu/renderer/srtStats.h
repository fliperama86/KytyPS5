#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SRTSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SRTSTATS_H_

#include <cstdint>

// Opt-in per-draw descriptor-mix statistics, enabled by KYTY_DEBUG_SRT_STATS=1.
//
// A GPU-side descriptor fetch design depends on how often a shader's descriptor layout is stable
// across draws, how many descriptor sources a stage resolves, and how much of the SRT evaluator is
// actual memory traffic. One game run with this on answers that: the collector writes
// _Diagnostics/srt-stats-<yyyymmdd-hhmmss>.json in the runtime directory at exit and rewrites the
// same file every 600 frames, so a killed run still leaves data.
//
// Every entry point is a no-op when the variable is unset; the payload is plain counters and small
// hash maps, so nothing is allocated on the render thread when the collector is off.
namespace Libs::Graphics::SrtStats {

namespace Detail {
extern bool enabled;
} // namespace Detail

[[nodiscard]] inline bool Enabled() {
	return Detail::enabled;
}

// One (draw, stage) event. The caller resolves every field from the stage's ResourcePlan and the
// permutation it selected, so this header stays free of recompiler types.
struct StageEvent {
	const char* stage_name             = ""; // static storage duration
	uint64_t    shader_hash            = 0;
	uint32_t    descriptor_sources     = 0;
	uint32_t    buffer_sources         = 0;
	uint32_t    scalar_buffer_sources  = 0;
	uint32_t    image_sources          = 0;
	uint32_t    sampler_sources        = 0;
	uint32_t    indirect_image_sources = 0;
	uint32_t    flat_insts             = 0;
	uint32_t    flat_reads             = 0;
	// Identity of the tuple over every buffer resource of the specialized
	// (packed_stride, descriptor_format, descriptor_swizzle).
	uint64_t buffer_layout_key = 0;
	// Identity of the decoded vertex-fetch layout the program cache keys on.
	uint64_t vertex_layout_key = 0;
	bool     has_vertex_layout = false;
	bool     uses_dma          = false;
	bool     push_overflow     = false;
	// The stage skipped CPU materialization of its buffer descriptor sources and kept the variant
	// its last CPU materialization chose (docs/gpu-descriptor-fetch.md, stage 1).
	bool gpu_fetch = false;
};

// Why a program that can fetch its own buffer descriptors still took the CPU path, plus the two
// events the feedback readback produces.
enum class GpuDescriptorEvent : uint32_t {
	// No variant has been materialized on the CPU yet, so there is no tuple to keep.
	CpuFirst,
	// A shader reported that a runtime V# no longer matches what it was specialized against.
	CpuFeedback,
	// The program exceeded its mismatch budget and is on the CPU path for good.
	CpuPinned,
	// One bit read back from the DescriptorFeedback buffer.
	FeedbackBit,
	// A program crossing the mismatch budget, counted once.
	ProgramPinned,
	Count,
};

void BeginEvent(bool dispatch);
void RecordStage(const StageEvent& event);
void RecordGpuDescriptor(GpuDescriptorEvent event);
void EndEvent();
void RecordGraphicsPipeline(const void* pipeline);
void EndFrame();
// Writes the final file. Safe to call more than once and from an emergency shutdown.
void WriteAtExit();

// Opens a draw or dispatch accumulator for as long as its stages are resolved. Does nothing when
// the collector is off.
class Scope {
public:
	explicit Scope(bool dispatch): m_active(Enabled()) {
		if (m_active) {
			BeginEvent(dispatch);
		}
	}
	~Scope() {
		if (m_active) {
			EndEvent();
		}
	}

	Scope(const Scope&)            = delete;
	Scope(Scope&&)                 = delete;
	Scope& operator=(const Scope&) = delete;
	Scope& operator=(Scope&&)      = delete;

private:
	bool m_active;
};

} // namespace Libs::Graphics::SrtStats

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SRTSTATS_H_
