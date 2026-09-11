#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GPUDESCRIPTORFETCH_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GPUDESCRIPTORFETCH_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Stage 1 of docs/gpu-descriptor-fetch.md: the buffer descriptors a shader evaluates itself.
//
// MarkGpuFetchBuffers decides which BufferResources qualify and leaves the flat SRT program the
// emitter lowers on Program::flat. Everything here is a no-op unless --gpu-descriptors is on.
void MarkGpuFetchBuffers(Program& program);

// A buffer descriptor is four dwords wide; a source of any other width is not a V#.
inline constexpr uint32_t GpuFetchDescriptorDwords = 4;

// Whether the SPIR-V lowering can emit the flat root behind descriptor source `source`.
//
// The three exclusions mirror the emitter (spirvEmitterSrt.cpp): a clean-context read has no
// shader equivalent because the specialization reader lives on the host, GetShaderBase is a
// per-draw constant the module cannot bake, and a user-data register outside the runtime's span
// can never reach the binding layout. `registers` collects the absolute user-data registers the
// root reads when it qualifies.
inline bool GpuFetchRootLowerable(const ResourcePlan& plan, uint32_t source,
                                  std::vector<uint32_t>* registers) {
	const auto& flat = plan.flat;
	if (!flat.compiled || source >= flat.sources.size() ||
	    source >= plan.descriptor_sources.size() ||
	    plan.descriptor_sources[source].dword_count != GpuFetchDescriptorDwords) {
		return false;
	}
	const auto& root = flat.sources[source];
	if (!root.valid || root.count == 0u || root.first > flat.schedule.size() ||
	    root.count > flat.schedule.size() - root.first) {
		return false;
	}
	const auto  first_register = registers != nullptr ? registers->size() : size_t {0};
	const auto  reject         = [&]() {
        if (registers != nullptr) {
            registers->resize(first_register);
        }
        return false;
	};
	std::vector<uint8_t> produced(flat.insts.size(), 0u);
	for (uint32_t step = 0; step < root.count; step++) {
		const auto index = flat.schedule[root.first + step];
		if (index >= flat.insts.size()) {
			return reject();
		}
		const auto& inst = flat.insts[index];
		if (inst.clean != 0u || inst.op == SrtFlatOp::ShaderBase) {
			return reject();
		}
		if (inst.op == SrtFlatOp::UserData) {
			if (inst.imm >= plan.user_data_count) {
				return reject();
			}
			const auto reg = plan.user_data_base + static_cast<uint32_t>(inst.imm);
			if (reg >= NumScalarRegs) {
				return reject();
			}
			if (registers != nullptr) {
				registers->push_back(reg);
			}
		}
		produced[index] = 1u;
	}
	// EmitSrtFlatRoot refuses a root whose result registers the schedule never produced. Only the
	// descriptor's own dwords carry a register; the rest of the array is unused.
	for (uint32_t dword = 0; dword < GpuFetchDescriptorDwords; dword++) {
		const auto result = root.results[dword];
		if (result >= produced.size() || produced[result] == 0u) {
			return reject();
		}
	}
	return true;
}

// The user-data registers the gpu_fetch roots read, sorted and deduplicated. The binding layout
// packs these next to the registers the shader body reads so EmitSrtFlatRoot can find them.
inline std::vector<uint32_t> GpuFetchUserDataRegisters(const Program& program) {
	std::vector<uint32_t> registers;
	if (!program.info.gpu_descriptors) {
		return registers;
	}
	for (const auto& buffer: program.info.buffers) {
		if (buffer.gpu_fetch) {
			GpuFetchRootLowerable(program, buffer.source, &registers);
		}
	}
	std::sort(registers.begin(), registers.end());
	registers.erase(std::unique(registers.begin(), registers.end()), registers.end());
	return registers;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GPUDESCRIPTORFETCH_H_ */
