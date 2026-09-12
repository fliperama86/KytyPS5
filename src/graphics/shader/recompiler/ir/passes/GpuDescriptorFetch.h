#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GPUDESCRIPTORFETCH_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GPUDESCRIPTORFETCH_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Stage 1 and 1b of docs/gpu-descriptor-fetch.md: the buffer descriptors and the flattened SRT
// scalar reads a shader evaluates itself.
//
// MarkGpuFetchBuffers decides which BufferResources and which flat SRT slots qualify and leaves
// the flat SRT program the emitter lowers on Program::flat. Everything here is a no-op unless
// --gpu-descriptors is on; the slot half additionally needs --gpu-srt-reads.
void MarkGpuFetchBuffers(Program& program);

// A buffer descriptor is four dwords wide; a source of any other width is not a V#.
inline constexpr uint32_t GpuFetchDescriptorDwords = 4;

// Whether the SPIR-V lowering can emit `root`, whose schedule must produce every register in
// `results`.
//
// The three exclusions mirror the emitter (spirvEmitterSrt.cpp): a clean-context read has no
// shader equivalent because the specialization reader lives on the host, GetShaderBase is a
// per-draw constant the module cannot bake, and a user-data register outside the runtime's span
// can never reach the binding layout. `registers` collects the absolute user-data registers the
// root reads when it qualifies.
inline bool GpuFetchRootSchedulable(const ResourcePlan& plan, const SrtFlatRoot& root,
                                    std::span<const uint32_t> results,
                                    std::vector<uint32_t>*    registers) {
	const auto& flat = plan.flat;
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
	// EmitSrtFlatRoot refuses a root whose result registers the schedule never produced.
	for (const auto result: results) {
		if (result >= produced.size() || produced[result] == 0u) {
			return reject();
		}
	}
	return true;
}

// Whether the SPIR-V lowering can emit the flat root behind descriptor source `source`. Only the
// descriptor's own four dwords carry a result register; the rest of the array is unused.
inline bool GpuFetchRootLowerable(const ResourcePlan& plan, uint32_t source,
                                  std::vector<uint32_t>* registers) {
	const auto& flat = plan.flat;
	if (!flat.compiled || source >= flat.sources.size() ||
	    source >= plan.descriptor_sources.size() ||
	    plan.descriptor_sources[source].dword_count != GpuFetchDescriptorDwords) {
		return false;
	}
	const auto& root = flat.sources[source];
	return GpuFetchRootSchedulable(
	    plan, root, std::span<const uint32_t>(root.results.data(), GpuFetchDescriptorDwords),
	    registers);
}

// Whether the SPIR-V lowering can emit the flat root behind SRT read slot `slot` (stage 1b). A
// slot the plan flagged clean is evaluated through the host's specialization reader and has no
// shader equivalent, so it stays on the CPU whatever the root looks like.
inline bool GpuFetchReadLowerable(const ResourcePlan& plan, uint32_t slot,
                                  std::vector<uint32_t>* registers) {
	const auto& flat = plan.flat;
	if (!flat.compiled || slot >= flat.flat_reads.size() || slot >= plan.srt_reads.size() ||
	    (slot < plan.clean_flat_slots.size() && plan.clean_flat_slots[slot] != 0u)) {
		return false;
	}
	const auto& root = flat.flat_reads[slot];
	return GpuFetchRootSchedulable(plan, root, std::span<const uint32_t>(&root.result, 1),
	                               registers);
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
	// The marked read slots were validated against the same flat program, so the schedule walk
	// here only has to collect their registers.
	const auto& slots = program.info.gpu_read_slots;
	for (uint32_t slot = 0; slot < slots.size(); slot++) {
		if (slots[slot] != 0u && slot < program.flat.flat_reads.size()) {
			const auto& root = program.flat.flat_reads[slot];
			GpuFetchRootSchedulable(program, root, std::span<const uint32_t>(&root.result, 1),
			                        &registers);
		}
	}
	std::sort(registers.begin(), registers.end());
	registers.erase(std::unique(registers.begin(), registers.end()), registers.end());
	return registers;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GPUDESCRIPTORFETCH_H_ */
