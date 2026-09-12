#include <algorithm>
#include "graphics/shader/recompiler/ir/passes/GpuDescriptorFetch.h"

#include "common/emulatorConfig.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {

void MarkGpuFetchBuffers(Program& program) {
	program.info.gpu_descriptors        = false;
	program.info.gpu_prologue_table     = false;
	program.info.gpu_fetch_side_effects = false;
	program.info.gpu_read_slots.clear();
	for (auto& buffer: program.info.buffers) {
		buffer.gpu_fetch = false;
	}
	// Stage 1b lowers the flattened SRT scalar reads as well, so a program with no buffer resource
	// at all can still qualify on its reads alone.
	const bool reads_enabled = Config::GpuSrtReadsEnabled() && !program.srt_reads.empty();
	if (!Config::GpuDescriptorsEnabled() || !program.resource_tracking_complete ||
	    program.shader_info_complete || program.binding_layout_complete ||
	    (program.info.buffers.empty() && !reads_enabled)) {
		return;
	}
	// The renderer evaluates the extracted plan, not the program, so build the same plan here:
	// ExtractResourcePlan ends in CompileSrtPlan, and its flat program is what the emitter
	// lowers. Building it twice costs a little compile time and keeps the two sides identical.
	auto plan = ExtractResourcePlan(program);
	if (!plan.flat.compiled) {
		return;
	}
	// A shader with side effects stays on the CPU path entirely. A fetched read that misses
	// (unmapped page, invalid root) yields zero, which is one wrong frame for a consumer but
	// permanent for a producer: docs/investigations/gpu-descriptors-stage1-crash-2026-09-11.md
	// shows a compute shader storing a null V# it derived from such a read, which a later CPU
	// walk then dies on.
	const bool side_effects =
	    program.info.uses_dma ||
	    std::ranges::any_of(program.info.buffers,
	                        [](const BufferResource& b) { return b.written || b.atomic; }) ||
	    std::ranges::any_of(program.info.images,
	                        [](const ImageResource& i) { return i.written || i.atomic; });
	// Step 2 of docs/sync-points-design.md lifts that rule for compute programs: with the
	// prologue page table on, a root read resolves through the mirror of whatever buffer covers
	// the page and through imported guest memory otherwise, so the miss the rule exists for can
	// only happen on a range the import refused or on nothing mapped at all -- and the prologue
	// the emitter builds for such a program returns before its first side effect instead of
	// storing what it derived from a zero. The resources with the side effects themselves keep
	// their host binding either way; only read-only buffers are ever marked below.
	const bool lift_side_effects =
	    Config::GpuFetchSideEffectsEnabled() && program.stage == ShaderType::Compute;
	if (side_effects && !lift_side_effects) {
		return;
	}
	bool any = false;
	for (auto& buffer: program.info.buffers) {
		// Stage 1 covers read-only buffers only: a written or atomic resource still needs the
		// host binding, and an image alias is resolved by the texture cache.
		if (!buffer.read || buffer.written || buffer.atomic ||
		    buffer.image_alias != BufferResource::NoImageAlias) {
			continue;
		}
		if (!GpuFetchRootLowerable(plan, buffer.source, nullptr)) {
			continue;
		}
		buffer.gpu_fetch = true;
		any              = true;
	}
	// Stage 1b: the flattened SRT slots the shader reads through BDA instead of the FlattenedSrt
	// binding. A clean slot has no shader equivalent and GpuFetchReadLowerable rejects it; the
	// indirect-image search table lives past the slots in the same buffer and is untouched.
	std::vector<uint8_t> reads;
	if (reads_enabled && plan.srt_reads.size() == program.srt_reads.size()) {
		bool any_read = false;
		reads.assign(plan.srt_reads.size(), 0u);
		for (uint32_t slot = 0; slot < reads.size(); slot++) {
			if (GpuFetchReadLowerable(plan, slot, nullptr)) {
				reads[slot] = 1u;
				any_read    = true;
			}
		}
		if (!any_read) {
			reads.clear();
		} else {
			any = true;
		}
	}
	if (!any) {
		return;
	}
	program.info.gpu_descriptors = true;
	// Step 1 of docs/sync-points-design.md: with the prologue table on, every root and
	// flattened read this program evaluates resolves through it.
	program.info.gpu_prologue_table = Config::GpuPrologueTableEnabled();
	// Step 2: the safety net is part of the module, so it belongs to the program's identity like
	// the table itself. Only a program that actually has side effects carries it.
	program.info.gpu_fetch_side_effects = side_effects;
	program.info.gpu_read_slots  = std::move(reads);
	program.flat                 = std::move(plan.flat);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
