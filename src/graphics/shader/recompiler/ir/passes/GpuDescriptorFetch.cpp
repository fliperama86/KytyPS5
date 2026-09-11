#include <algorithm>
#include "graphics/shader/recompiler/ir/passes/GpuDescriptorFetch.h"

#include "common/emulatorConfig.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {

void MarkGpuFetchBuffers(Program& program) {
	program.info.gpu_descriptors = false;
	for (auto& buffer: program.info.buffers) {
		buffer.gpu_fetch = false;
	}
	if (!Config::GpuDescriptorsEnabled() || !program.resource_tracking_complete ||
	    program.shader_info_complete || program.binding_layout_complete ||
	    program.info.buffers.empty()) {
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
	if (side_effects) {
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
	if (!any) {
		return;
	}
	program.info.gpu_descriptors = true;
	program.flat                 = std::move(plan.flat);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
