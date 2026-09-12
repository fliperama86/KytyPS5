#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Whether the module still reads the FlattenedSrt binding. Stage 1b of
// docs/gpu-descriptor-fetch.md lowers marked flat SRT slots into the shader prologue, so a program
// whose every slot is lowered needs the binding only for the indirect-image search table that
// shares the buffer past the slots.
inline bool UsesFlattenedRuntime(const Program& program) {
	const auto& lowered = program.info.gpu_read_slots;
	for (size_t slot = 0; slot < program.srt_reads.size(); slot++) {
		if (slot >= lowered.size() || lowered[slot] == 0u) {
			return true;
		}
	}
	return std::ranges::any_of(program.info.images, [](const ImageResource& image) {
		return image.indirect_search_iterations != 0u;
	});
}

void AllocateBindings(Program& program, uint32_t push_data_start_dword = 0);

const DescriptorBinding* FindBinding(const BindingLayout& layout, DescriptorBindingKind kind);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_BINDINGLAYOUT_H_ */
