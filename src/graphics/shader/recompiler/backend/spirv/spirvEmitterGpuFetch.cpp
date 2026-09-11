// Shader-side buffer descriptor fetch (docs/gpu-descriptor-fetch.md, stage 1).
//
// MarkGpuFetchBuffers (ir/passes/GpuDescriptorFetch.cpp) picks the buffer resources whose SRT root
// the shader can evaluate itself. This file emits that evaluation once in the function prologue:
// every gpu_fetch root is lowered with EmitSrtFlatRoot, its four dwords are decoded as a V#, and
// the resulting base and GCN range are parked in EmitterState::gpu_fetch_buffers, where
// PrepareStorageBufferResourceAccess picks them up in place of the bound descriptor.
//
// The decode mirrors the host exactly: ShaderBufferResource in graphics/shader/shaderBindings.h
// for the field positions, RenderExecutor::FindBuffers for the range (stride * num_records, or
// num_records when unstrided) and BuildResourceSpecialization for the descriptor type zeroing and
// for the packed_stride / format / dst_sel tuple this module was compiled against. Any runtime
// disagreement with that tuple sets the program's bit in the DescriptorFeedback buffer.

#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <array>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

// The BDA page table describes a 40-bit space, so nothing above it is readable in-shader.
constexpr uint64_t GpuFetchAddressLimit = uint64_t {1} << 40u;

uint32_t WideConstant(EmitterState& state, uint64_t value) {
	return state.builder.Constant(
	    OpConstant, TypeScalarU64(state),
	    {static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32u)});
}

uint32_t Narrow(EmitterState& state, uint32_t value) {
	return Unary(state, OpUConvert, TypeU32(state), value);
}

uint32_t Widen(EmitterState& state, uint32_t value) {
	return Unary(state, OpUConvert, TypeScalarU64(state), value);
}

uint32_t Field(EmitterState& state, uint32_t value, uint32_t offset, uint32_t bits) {
	const auto shifted =
	    offset == 0u ? value
	                 : Binary(state, OpShiftRightLogical, TypeU32(state), value,
	                          ConstantU32(state, offset));
	if (bits >= 32u) {
		return shifted;
	}
	return Binary(state, OpBitwiseAnd, TypeU32(state), shifted,
	              ConstantU32(state, (1u << bits) - 1u));
}

uint32_t ShiftedInto(EmitterState& state, uint32_t value, uint32_t offset) {
	return offset == 0u ? value
	                    : Binary(state, OpShiftLeftLogical, TypeU32(state), value,
	                             ConstantU32(state, offset));
}

uint32_t EqualU32(EmitterState& state, uint32_t value, uint32_t constant) {
	return Binary(state, OpIEqual, TypeBool(state), value, ConstantU32(state, constant));
}

uint32_t DiffersFrom(EmitterState& state, uint32_t value, uint32_t constant) {
	return Binary(state, OpINotEqual, TypeBool(state), value, ConstantU32(state, constant));
}

uint32_t AnyOf(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	return lhs == 0u ? rhs : Binary(state, OpLogicalOr, TypeBool(state), lhs, rhs);
}

uint32_t AllOf(EmitterState& state, uint32_t lhs, uint32_t rhs) {
	return Binary(state, OpLogicalAnd, TypeBool(state), lhs, rhs);
}

// BuildResourceSpecialization normalises the packed stride before baking it: an unstrided
// descriptor keeps neither the swizzle bit nor the index stride, and an unswizzled one keeps no
// index stride. The compare has to normalise the same way or it reports a phantom mismatch.
uint32_t RuntimePackedStride(EmitterState& state, uint32_t stride, uint32_t swizzle,
                             uint32_t index_stride, uint32_t add_tid) {
	auto packed = stride;
	packed      = Binary(state, OpBitwiseOr, TypeU32(state), packed, ShiftedInto(state, swizzle, 14));
	packed = Binary(state, OpBitwiseOr, TypeU32(state), packed, ShiftedInto(state, index_stride, 16));
	packed = Binary(state, OpBitwiseOr, TypeU32(state), packed, ShiftedInto(state, add_tid, 20));
	const auto unstrided  = EqualU32(state, stride, 0);
	const auto unswizzled = EqualU32(state, swizzle, 0);
	const auto mask       = Select(
	          state, TypeU32(state), unstrided, ConstantU32(state, ~((1u << 14u) | (3u << 16u))),
	          Select(state, TypeU32(state), unswizzled, ConstantU32(state, ~(3u << 16u)),
	                 ConstantU32(state, UINT32_MAX)));
	return Binary(state, OpBitwiseAnd, TypeU32(state), packed, mask);
}

// Same shape as RecordBdaFault, on the feedback buffer and atomically: a lost update here would
// hide a mismatch from the host, where a lost fault bit only delays a page by one more frame.
void RecordFeedback(EmitterState& state, uint32_t condition) {
	const auto slot_dword = state.program.bindings.feedback_slot_dword;
	if (condition == 0u || state.descriptor_feedback_variable == 0u ||
	    slot_dword == IR::BindingLayout::NoFeedbackSlot) {
		return;
	}
	EmitIfCondition(state, condition, [&]() {
		const auto slot = EmitShaderDataDwordLoad(state, slot_dword);
		const auto word =
		    Binary(state, OpShiftRightLogical, TypeU32(state), slot, ConstantU32(state, 5));
		const auto bit = Binary(
		    state, OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 1),
		    Binary(state, OpBitwiseAnd, TypeU32(state), slot, ConstantU32(state, 31)));
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
		                           state.descriptor_feedback_variable, ConstantU32(state, 0),
		                           word});
		const auto previous = state.builder.AllocateId();
		state.builder.AddFunction({OpAtomicOr, TypeU32(state), previous, pointer,
		                           ConstantU32(state, ScopeDevice),
		                           ConstantU32(state, MemorySemanticsNone), bit});
	});
}

} // namespace

void EmitGpuFetchDescriptors(ValueEmitContext& ctx) {
	auto& state = ctx.state;
	if (!state.program.info.gpu_descriptors) {
		return;
	}
	const auto& flat     = state.program.flat;
	uint32_t    mismatch = 0;
	for (uint32_t resource = 0; resource < state.program.info.buffers.size(); resource++) {
		const auto& buffer = state.program.info.buffers[resource];
		if (!buffer.gpu_fetch) {
			continue;
		}
		if (resource >= state.gpu_fetch_buffers.size() || buffer.source >= flat.sources.size()) {
			ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, resource,
			                             "shader-fetched buffer has no flat descriptor root");
		}
		const auto&                   root = flat.sources[buffer.source];
		const std::array<uint32_t, 4> results {root.results[0], root.results[1], root.results[2],
		                                       root.results[3]};
		const auto lowered = EmitSrtFlatRoot(ctx, flat, root, results, {});
		if (!lowered.supported || lowered.count != results.size()) {
			ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, resource,
			                             "shader-fetched descriptor root has no SPIR-V lowering");
		}
		std::array<uint32_t, 4> dword {};
		for (uint32_t index = 0; index < dword.size(); index++) {
			dword[index] = Narrow(state, lowered.results[index]);
		}
		// A non-zero descriptor type is not a V#; the host materialization zeroes the whole
		// descriptor before deriving anything from it.
		const auto typed = DiffersFrom(state, Field(state, dword[3], 30, 2), 0);
		for (auto& value: dword) {
			value = Select(state, TypeU32(state), typed, ConstantU32(state, 0), value);
		}

		const auto base = Binary(
		    state, OpBitwiseOr, TypeScalarU64(state), Widen(state, dword[0]),
		    Binary(state, OpShiftLeftLogical, TypeScalarU64(state),
		           Widen(state, Field(state, dword[1], 0, 16)), WideConstant(state, 32)));
		const auto stride       = Field(state, dword[1], 16, 14);
		const auto swizzle      = Field(state, dword[1], 31, 1);
		const auto records      = dword[2];
		const auto index_stride = Field(state, dword[3], 21, 2);
		const auto add_tid      = Field(state, dword[3], 23, 1);

		// RenderExecutor::FindBuffers derives the host binding's range the same way, and the
		// bound range is what the OpArrayLength check tests today.
		const auto size = Select(
		    state, TypeScalarU64(state), EqualU32(state, stride, 0), Widen(state, records),
		    Binary(state, OpIMul, TypeScalarU64(state), Widen(state, stride), Widen(state, records)));
		const auto dwords = Binary(state, OpShiftRightLogical, TypeScalarU64(state), size,
		                           WideConstant(state, 2));
		const auto capped = Select(
		    state, TypeScalarU64(state),
		    Binary(state, OpUGreaterThan, TypeBool(state), dwords, WideConstant(state, UINT32_MAX)),
		    WideConstant(state, UINT32_MAX), dwords);
		const auto present =
		    Binary(state, OpINotEqual, TypeBool(state), base, WideConstant(state, 0));
		const auto sized =
		    Binary(state, OpINotEqual, TypeBool(state), size, WideConstant(state, 0));
		// Above the page table's 40-bit space the lookup would index it out of range.
		const auto reachable =
		    Binary(state, OpULessThanEqual, TypeBool(state),
		           Binary(state, OpIAdd, TypeScalarU64(state), base, size),
		           WideConstant(state, GpuFetchAddressLimit));
		const auto usable = AllOf(state, lowered.valid,
		                          AllOf(state, present, AllOf(state, sized, reachable)));
		state.gpu_fetch_buffers[resource] = {
		    .base   = base,
		    .length = Select(state, TypeU32(state), usable, Narrow(state, capped),
		                     ConstantU32(state, 0)),
		};

		auto differs = DiffersFrom(
		    state, RuntimePackedStride(state, stride, swizzle, index_stride, add_tid),
		    buffer.packed_stride);
		if (buffer.formatted) {
			differs = AnyOf(state, differs,
			                DiffersFrom(state, Field(state, dword[3], 12, 7),
			                            static_cast<uint32_t>(buffer.descriptor_format)));
			differs = AnyOf(state, differs,
			                DiffersFrom(state, Field(state, dword[3], 0, 12),
			                            buffer.descriptor_swizzle));
		}
		// A root the shader could not evaluate says nothing about the specialization; the fault
		// bit already tells the host to map the page.
		mismatch = AnyOf(state, mismatch, AllOf(state, lowered.valid, differs));
	}
	RecordFeedback(state, mismatch);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
