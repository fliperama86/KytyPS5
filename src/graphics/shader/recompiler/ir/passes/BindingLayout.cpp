#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"

#include "common/assert.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/GpuDescriptorFetch.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

[[noreturn]] void BindingFail(const char* message) {
	EXIT("shader binding layout failed: %s", message);
	std::abort();
}

std::vector<uint32_t> CollectUserData(const Program& program) {
	std::array<bool, NumScalarRegs> registers {};
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::GetUserData || !inst.HasUses()) {
				continue;
			}
			if (inst.Arg(0).GetType() != Type::ScalarReg) {
				BindingFail("typed shader contains an invalid user-data register");
			}
			const auto index = RegIndex(inst.Arg(0).ScalarRegister());
			if (index >= NumScalarRegs) {
				BindingFail("typed shader contains an invalid user-data register");
			}
			registers[index] = true;
		}
	}
	std::vector<uint32_t> result;
	for (uint32_t index = 0; index < registers.size(); index++) {
		if (registers[index]) {
			result.push_back(index);
		}
	}
	return result;
}

void AddBinding(BindingLayout& layout, DescriptorBindingKind kind,
                std::vector<uint32_t> resources = {}) {
	layout.descriptors.push_back({kind, std::move(resources)});
}

bool UsesGds(const Program& program) {
	bool uses_gds = false;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (SharedAccessOf(inst.GetOpcode()) == SharedAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= program.memory_info.size()) {
				BindingFail("typed shader contains invalid shared-memory metadata");
			}
			const auto kind = program.memory_info[index].kind;
			if (kind != ResourceKind::Lds && kind != ResourceKind::Gds) {
				BindingFail("typed shader contains invalid shared-memory metadata");
			}
			uses_gds |= kind == ResourceKind::Gds;
		}
	}
	return uses_gds;
}

} // namespace

void AllocateBindings(Program& program, uint32_t push_data_start_dword) {
	if (!program.shader_info_complete || program.binding_layout_complete) {
		EXIT("shader binding layout failed: %s", !program.shader_info_complete
		                                             ? "shader info is not ready"
		                                             : "binding layout already allocated");
	}
	BindingLayout next;
	next.user_data_registers = CollectUserData(program);
	if (program.info.gpu_descriptors) {
		// The shader body never reads the registers a gpu_fetch root walks, so the layout has to
		// pack them too (docs/gpu-descriptor-fetch.md, stage 1).
		for (const auto reg: GpuFetchUserDataRegisters(program)) {
			const auto at = std::ranges::lower_bound(next.user_data_registers, reg);
			if (at == next.user_data_registers.end() || *at != reg) {
				next.user_data_registers.insert(at, reg);
			}
		}
	}
	next.memory_offset_dword = static_cast<uint32_t>(next.user_data_registers.size());
	next.memory_offset_count = static_cast<uint32_t>(program.info.buffers.size());
	if (program.info.gpu_descriptors) {
		// One dword past the memory offsets; the host packs the program's feedback slot id there.
		next.feedback_slot_dword =
		    next.memory_offset_dword + (next.memory_offset_count + 3u) / 4u;
	}
	next.push_data_start_dword =
	    PushData::StartFor(push_data_start_dword, next.ShaderDataDwords());

	if (!program.info.buffers.empty()) {
		std::vector<uint32_t> resources(program.info.buffers.size());
		for (uint32_t i = 0; i < resources.size(); i++) {
			resources[i] = i;
		}
		AddBinding(next, DescriptorBindingKind::Buffers, std::move(resources));
	}

	std::array<std::vector<uint32_t>, ImageBindingCount> image_groups;
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto kind = DescriptorBindingForImage(program.info.images[i]);
		if (!kind.has_value()) {
			EXIT("shader binding layout failed: image %u has an invalid binding class", i);
		}
		const auto group = ImageBindingIndex(*kind);
		if (group >= image_groups.size()) {
			EXIT("shader binding layout failed: image %u has an unmapped binding class", i);
		}
		auto&      resources = image_groups[group];
		const auto dynamic   = program.info.images[i].mip_mode == ImageMipMode::DynamicStorage;
		const auto count     = dynamic ? program.info.images[i].mip_count : 1u;
		if (count == 0u || (!dynamic && program.info.images[i].mip_count != 1u)) {
			EXIT("shader binding layout failed: image %u has invalid specialized mip count %u", i,
			     program.info.images[i].mip_count);
		}
		resources.insert(resources.end(), count, i);
	}
	for (uint32_t i = 0; i < image_groups.size(); i++) {
		if (!image_groups[i].empty()) {
			AddBinding(next, static_cast<DescriptorBindingKind>(FirstImageBinding + i),
			           std::move(image_groups[i]));
		}
	}

	if (!program.info.samplers.empty()) {
		std::vector<uint32_t> resources(program.info.samplers.size());
		for (uint32_t i = 0; i < resources.size(); i++) {
			resources[i] = i;
		}
		AddBinding(next, DescriptorBindingKind::Samplers, std::move(resources));
	}
	if (UsesGds(program)) {
		AddBinding(next, DescriptorBindingKind::Gds);
	}
	if (program.info.uses_dma || program.info.gpu_descriptors) {
		AddBinding(next, DescriptorBindingKind::BdaPagetable);
		AddBinding(next, DescriptorBindingKind::FaultBuffer);
	}
	if (program.info.gpu_descriptors) {
		AddBinding(next, DescriptorBindingKind::DescriptorFeedback);
	}
	if (UsesFlattenedRuntime(program)) {
		AddBinding(next, DescriptorBindingKind::FlattenedSrt);
	}

	if (next.ShaderDataDwords() != 0 && !next.UsesPushData()) {
		AddBinding(next, DescriptorBindingKind::ShaderData);
	}

	program.bindings                = std::move(next);
	program.binding_layout_complete = true;
}

const DescriptorBinding* FindBinding(const BindingLayout& layout, DescriptorBindingKind kind) {
	for (const auto& binding: layout.descriptors) {
		if (binding.kind == kind) {
			return &binding;
		}
	}
	return nullptr;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
