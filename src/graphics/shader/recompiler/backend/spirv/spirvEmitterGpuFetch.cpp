// Shader-side buffer descriptor fetch (docs/gpu-descriptor-fetch.md, stages 1 and 1b).
//
// MarkGpuFetchBuffers (ir/passes/GpuDescriptorFetch.cpp) picks the buffer resources whose SRT root
// the shader can evaluate itself, and the flat SRT read slots it can evaluate instead of loading
// them from the FlattenedSrt binding. This file emits that evaluation once in the function
// prologue. Each marked read slot is lowered with EmitSrtFlatRoot and parked in
// EmitterState::gpu_read_values, where the ReadConst case of spirvEmitterMemory.cpp picks it up.
// Each gpu_fetch descriptor root is lowered the same way, its four dwords are decoded as a V#,
// and the resulting base and GCN range are parked in EmitterState::gpu_fetch_buffers, where
// PrepareStorageBufferResourceAccess picks them up in place of the bound descriptor.
//
// The decode mirrors the host exactly: ShaderBufferResource in graphics/shader/shaderBindings.h
// for the field positions, RenderExecutor::FindBuffers for the range (stride * num_records, or
// num_records when unstrided) and BuildResourceSpecialization for the descriptor type zeroing and
// for the packed_stride / format / dst_sel tuple this module was compiled against. Any runtime
// disagreement with that tuple sets the program's bit in the DescriptorFeedback buffer.

#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include <array>
#include <string>
#include <cstdlib>
#include <span>
#include <vector>

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

uint32_t NotBool(EmitterState& state, uint32_t value) {
	return Unary(state, OpLogicalNot, TypeBool(state), value);
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

bool CanRecordFeedback(const EmitterState& state) {
	return state.descriptor_feedback_variable != 0u &&
	       state.program.bindings.feedback_slot_dword != IR::BindingLayout::NoFeedbackSlot;
}

// Sets this program's bit in the DescriptorFeedback buffer. Same shape as RecordBdaFault, on the
// feedback buffer and atomically: a lost update here would hide a mismatch from the host, where a
// lost fault bit only delays a page by one more frame.
void SetFeedbackBit(EmitterState& state) {
	const auto slot = EmitShaderDataDwordLoad(state, state.program.bindings.feedback_slot_dword);
	const auto word =
	    Binary(state, OpShiftRightLogical, TypeU32(state), slot, ConstantU32(state, 5));
	const auto bit =
	    Binary(state, OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 1),
	           Binary(state, OpBitwiseAnd, TypeU32(state), slot, ConstantU32(state, 31)));
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                           state.descriptor_feedback_variable, ConstantU32(state, 0), word});
	const auto previous = state.builder.AllocateId();
	state.builder.AddFunction({OpAtomicOr, TypeU32(state), previous, pointer,
	                           ConstantU32(state, ScopeDevice),
	                           ConstantU32(state, MemorySemanticsNone), bit});
}

void RecordFeedback(EmitterState& state, uint32_t condition) {
	if (condition == 0u || !CanRecordFeedback(state)) {
		return;
	}
	EmitIfCondition(state, condition, [&]() { SetFeedbackBit(state); });
}

// Step 2 of docs/sync-points-design.md: the dword past the prologue's own miss counter, so the
// host can say how often the safety net fired. The fault buffer always has room for both when a
// program carries the prologue table, which --gpu-fetch-side-effects requires.
void CountSideEffectSkip(EmitterState& state) {
	if (state.fault_buffer_variable == 0u) {
		return;
	}
	constexpr uint32_t SkipCounterIndex =
	    static_cast<uint32_t>(BufferCache::CACHING_NUMPAGES / 32u) + 1u;
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state), pointer,
	                           state.fault_buffer_variable, ConstantU32(state, 0),
	                           ConstantU32(state, SkipCounterIndex)});
	const auto previous = state.builder.AllocateId();
	state.builder.AddFunction({OpAtomicIAdd, TypeU32(state), previous, pointer,
	                           ConstantU32(state, ScopeDevice),
	                           ConstantU32(state, MemorySemanticsNone), ConstantU32(state, 1)});
}

// The safety net. A program with side effects whose prologue could not evaluate every root and
// flattened read must not run: a zero descriptor it stores outlives the frame
// (docs/investigations/gpu-descriptors-stage1-crash-2026-09-11.md). The entry point returns here,
// before the body and therefore before the program's first side effect, having set the program's
// feedback bit -- which puts the next dispatch of it on the CPU path, exactly as a specialization
// mismatch does -- and counted itself. The page that could not be read has already recorded its
// fault bit inside the page-table lookup, so the host maps it for the next frame.
//
// A root reads user data and guest memory at addresses derived from it, so its value is the same
// in every invocation and the branch is uniform by construction; the subgroup vote makes that
// explicit and keeps a divergence, if one were ever possible, on the conservative side (the whole
// subgroup skips rather than half of it storing).
void EmitSideEffectGuard(EmitterState& state, uint32_t invalid, uint32_t first_invalid,
                         uint32_t fail_address, uint32_t fail_reason) {
	const auto voted = state.builder.AllocateId();
	state.builder.AddFunction({OpGroupNonUniformAny, TypeBool(state), voted,
	                           ConstantU32(state, ScopeSubgroup), invalid});
	const auto skip_label  = state.builder.AllocateId();
	const auto merge_label = state.builder.AllocateId();
	state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
	state.builder.AddFunction({OpBranchConditional, voted, skip_label, merge_label});
	EmitLabel(state, skip_label);
	if (CanRecordFeedback(state)) {
		SetFeedbackBit(state);
	}
	CountSideEffectSkip(state);
	// Diagnostic record (docs/sync-points-design.md, step 2 fact check): the first target the
	// prologue found invalid and this program's feedback slot, two dwords past the skip counter.
	if (state.fault_buffer_variable != 0u && first_invalid != 0u) {
		constexpr uint32_t RecordIndex =
		    static_cast<uint32_t>(BufferCache::CACHING_NUMPAGES / 32u) + 2u;
		const auto target_pointer = state.builder.AllocateId();
		state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state),
		                           target_pointer, state.fault_buffer_variable,
		                           ConstantU32(state, 0), ConstantU32(state, RecordIndex)});
		state.builder.AddFunction({OpStore, target_pointer, first_invalid});
		if (CanRecordFeedback(state)) {
			const auto slot =
			    EmitShaderDataDwordLoad(state, state.program.bindings.feedback_slot_dword);
			const auto slot_pointer = state.builder.AllocateId();
			state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state),
			                           slot_pointer, state.fault_buffer_variable,
			                           ConstantU32(state, 0), ConstantU32(state, RecordIndex + 1u)});
			state.builder.AddFunction({OpStore, slot_pointer, slot});
		}
		if (false && fail_address != 0u && fail_reason != 0u) {
			const auto low  = Narrow(state, fail_address);
			const auto high = Narrow(state, Binary(state, OpShiftRightLogical, TypeScalarU64(state),
			                                       fail_address, WideConstant(state, 32)));
			const uint32_t values[] = {low, high, fail_reason};
			for (uint32_t i = 0; i < 3; i++) {
				const auto pointer = state.builder.AllocateId();
				state.builder.AddFunction({OpAccessChain, TypeStorageBufferElementPointer(state),
				                           pointer, state.fault_buffer_variable,
				                           ConstantU32(state, 0),
				                           ConstantU32(state, RecordIndex + 2u + i)});
				state.builder.AddFunction({OpStore, pointer, values[i]});
			}
		}
	}
	state.builder.AddFunction({OpReturn});
	EmitLabel(state, merge_label);
}

} // namespace

void EmitGpuFetchDescriptors(ValueEmitContext& ctx) {
	auto& state = ctx.state;
	if (!state.program.info.gpu_descriptors) {
		return;
	}
	const auto& flat     = state.program.flat;
	uint32_t    mismatch = 0;
	// Step 2: a program with side effects collects the validity of everything its prologue
	// evaluates, and skips the whole dispatch rather than deriving anything from a zero.
	const bool guarded = state.program.info.gpu_fetch_side_effects;
	uint32_t   invalid = 0;
	uint32_t   first_invalid = guarded ? ConstantU32(state, UINT32_MAX) : 0u;
	// Diagnostic bisection (KYTY_GUARD_SELECT): "reads", "buffers", "lt<N>" or "ge<N>" restrict
	// which prologue targets feed the safety net, by target kind or by target index.
	static const std::string guard_select = [] {
		const char* value = std::getenv("KYTY_GUARD_SELECT");
		return std::string(value != nullptr ? value : "");
	}();
	const auto note_invalid = [&](uint32_t valid, uint32_t target_index, bool is_read) {
		if (!guarded) {
			return;
		}
		if (!guard_select.empty()) {
			if (guard_select == "reads" && !is_read) {
				return;
			}
			if (guard_select == "buffers" && is_read) {
				return;
			}
			if (guard_select.rfind("lt", 0) == 0 &&
			    !(target_index < static_cast<uint32_t>(std::atoi(guard_select.c_str() + 2)))) {
				return;
			}
			if (guard_select.rfind("ge", 0) == 0 &&
			    !(target_index >= static_cast<uint32_t>(std::atoi(guard_select.c_str() + 2)))) {
				return;
			}
		}
		invalid = AnyOf(state, invalid, NotBool(state, valid));
		first_invalid =
		    Select(state, TypeU32(state),
		           AllOf(state, NotBool(state, valid),
		                 Binary(state, OpIEqual, TypeBool(state), first_invalid,
		                        ConstantU32(state, UINT32_MAX))),
		           ConstantU32(state, target_index), first_invalid);
	};
	// Every root of the prologue is lowered in one pass on one emitter: the roots of a program
	// walk the same SRT chain and differ only in their last steps, so a step another root already
	// produced is reused. Lowering them one emitter each cost a quarter of a million SPIR-V words
	// and seconds of driver compile time per program (docs/sync-points-design.md, step 2).
	//
	// Stage 1b's flat SRT reads come first so their uniform values dominate every use in the body,
	// and independently of the descriptors: a program may have marked reads and no marked buffer.
	const auto& read_slots = state.program.info.gpu_read_slots;
	struct PrologueTarget {
		uint32_t slot     = UINT32_MAX; // flat SRT read slot, or UINT32_MAX
		uint32_t resource = UINT32_MAX; // buffer resource, or UINT32_MAX
	};
	std::vector<PrologueTarget>          targets;
	std::vector<const IR::SrtFlatRoot*>  roots;
	std::vector<std::array<uint32_t, 4>> wanted;
	std::vector<uint32_t>                wanted_count;
	if (!read_slots.empty()) {
		state.gpu_read_values.assign(read_slots.size(), 0u);
		for (uint32_t slot = 0; slot < read_slots.size(); slot++) {
			if (read_slots[slot] == 0u) {
				continue;
			}
			if (slot >= flat.flat_reads.size()) {
				ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::FlattenedSrt, slot,
				                             "shader-fetched SRT read has no flat root");
			}
			const auto& root = flat.flat_reads[slot];
			targets.push_back({.slot = slot});
			roots.push_back(&root);
			wanted.push_back({root.result, 0u, 0u, 0u});
			wanted_count.push_back(1u);
		}
	}
	for (uint32_t resource = 0; resource < state.program.info.buffers.size(); resource++) {
		const auto& buffer = state.program.info.buffers[resource];
		if (!buffer.gpu_fetch) {
			continue;
		}
		if (resource >= state.gpu_fetch_buffers.size() || buffer.source >= flat.sources.size()) {
			ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, resource,
			                             "shader-fetched buffer has no flat descriptor root");
		}
		const auto& root = flat.sources[buffer.source];
		targets.push_back({.resource = resource});
		roots.push_back(&root);
		wanted.push_back({root.results[0], root.results[1], root.results[2], root.results[3]});
		wanted_count.push_back(4u);
	}
	std::vector<SrtRootRequest> requests(targets.size());
	for (size_t index = 0; index < targets.size(); index++) {
		requests[index] = {roots[index],
		                   std::span<const uint32_t>(wanted[index].data(), wanted_count[index])};
	}
	std::vector<SrtLoweredRoot> lowered_roots(targets.size());
	EmitSrtFlatRoots(ctx, flat, requests, {}, lowered_roots);

	for (size_t index = 0; index < targets.size(); index++) {
		const auto& target  = targets[index];
		const auto& lowered = lowered_roots[index];
		if (target.resource == UINT32_MAX) {
			const auto slot = target.slot;
			if (!lowered.supported || lowered.count != 1u) {
				ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::FlattenedSrt, slot,
				                             "shader-fetched SRT read has no SPIR-V lowering");
			}
			// A root the shader could not walk reads as zero, exactly as an invalid descriptor
			// root does; the unmapped page it failed on already set its fault bit. A guarded
			// program never reaches the use: the prologue returns first.
			note_invalid(lowered.valid, static_cast<uint32_t>(index), true);
			state.gpu_read_values[slot] =
			    Select(state, TypeU32(state), lowered.valid, Narrow(state, lowered.results[0]),
			           ConstantU32(state, 0));
			continue;
		}
		const auto  resource = target.resource;
		const auto& buffer   = state.program.info.buffers[resource];
		if (!lowered.supported || lowered.count != 4u) {
			ExitDescriptorBindingFailure(state, IR::DescriptorBindingKind::Buffers, resource,
			                             "shader-fetched descriptor root has no SPIR-V lowering");
		}
		note_invalid(lowered.valid, static_cast<uint32_t>(index), false);
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
	if (guarded && invalid != 0u) {
		EmitSideEffectGuard(state, invalid, first_invalid,
		                    lowered_roots.empty() ? 0u : lowered_roots[0].fail_address,
		                    lowered_roots.empty() ? 0u : lowered_roots[0].fail_reason);
	}
	RecordFeedback(state, mismatch);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
