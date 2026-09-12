// Lowers an SrtFlatProgram to SPIR-V (spike).
//
// CompileSrtPlan (SrtWalker.cpp) turns a ResourcePlan into a straight-line program that
// FlatMachine replays on the CPU once per draw. This file emits the same program into a shader
// so the GPU can evaluate descriptor sources itself. FlatMachine is the specification: every
// lowering below mirrors one case of FlatMachine::Execute, including value width, signedness,
// the 64-bit register representation and the conditions under which a step fails.
//
// Register representation: FlatMachine keeps one uint64_t per register and stores narrow results
// zero-extended into it. The lowering does exactly the same, holding every register as a scalar
// 64-bit unsigned value and going through 32-bit only where FlatMachine casts to uint32_t.
//
// Failure: FlatMachine::Run returns false and the caller falls back. A shader cannot return
// "no value", so each register carries a companion bool that is false exactly where FlatMachine
// would have failed, and a root reports the conjunction over its results' cones. Two deliberate
// deviations are documented at their emission sites: a guest read outside the BDA page table's
// 40-bit space, and a read of a page the page table does not map.
//
// Nothing here is wired into production shader emission yet; only the unit tests call it.

#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

// FlatMachine masks guest addresses to 48 bits.
constexpr uint64_t SrtAddressMask = 0x0000ffffffffffffull;
// The BDA page table only describes a 40-bit space, so a shader cannot reach further.
constexpr uint64_t SrtBdaLimit = uint64_t {1} << 40u;

constexpr uint32_t OpIsInf = 157;

uint32_t TypeScalarS64(EmitterState& state) {
	return state.builder.Type(OpTypeInt, {64, 1});
}

uint32_t WideConstant(EmitterState& state, uint64_t value) {
	return state.builder.Constant(
	    OpConstant, TypeScalarU64(state),
	    {static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32u)});
}

// uint64_t -> uint32_t, the same truncation as static_cast<uint32_t>(register).
uint32_t Narrow(EmitterState& state, uint32_t value) {
	return Unary(state, OpUConvert, TypeU32(state), value);
}

// uint32_t -> uint64_t, the same zero extension as storing a narrow result into a register.
uint32_t Widen(EmitterState& state, uint32_t value) {
	return Unary(state, OpUConvert, TypeScalarU64(state), value);
}

uint32_t NotBool(EmitterState& state, uint32_t value) {
	return Unary(state, OpLogicalNot, TypeBool(state), value);
}

uint32_t BoolRegister(EmitterState& state, uint32_t condition) {
	return Select(state, TypeScalarU64(state), condition, WideConstant(state, 1),
	              WideConstant(state, 0));
}

uint32_t NonZero(EmitterState& state, uint32_t wide) {
	return Binary(state, OpINotEqual, TypeBool(state), wide, WideConstant(state, 0));
}

uint32_t BitcastTo(EmitterState& state, uint32_t type, uint32_t value) {
	return Unary(state, OpBitcast, type, value);
}

uint32_t GlslUnary(EmitterState& state, uint32_t type, uint32_t instruction, uint32_t value) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction({OpExtInst, type, result, GlslStd450(state), instruction, value});
	return result;
}

uint32_t GlslBinary(EmitterState& state, uint32_t type, uint32_t instruction, uint32_t lhs,
                    uint32_t rhs) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpExtInst, type, result, GlslStd450(state), instruction, lhs, rhs});
	return result;
}

bool UserDataDword(const EmitterState& state, uint32_t reg, uint32_t& dword_index) {
	const auto& registers = state.program.bindings.user_data_registers;
	const auto  found     = std::lower_bound(registers.begin(), registers.end(), reg);
	if (found == registers.end() || *found != reg) {
		return false;
	}
	dword_index = static_cast<uint32_t>(found - registers.begin());
	return true;
}

struct GuestRead {
	uint32_t value   = 0; // u32 id
	uint32_t present = 0; // bool id
};

// Emits one root's schedule. Registers persist across roots on the same emitter, matching the
// way FlatMachine skips registers another root already produced in the same run.
class RootEmitter {
public:
	RootEmitter(ValueEmitContext& ctx, const IR::SrtFlatProgram& program,
	            const SrtLoweringInputs& inputs)
	    : m_ctx(ctx), m_state(ctx.state), m_program(program), m_inputs(inputs) {
		m_values.assign(program.insts.size(), 0u);
		m_conditions.assign(program.insts.size(), 0u);
		m_written.assign(program.insts.size(), 0u);
		m_true = ConstantBool(m_state, true);
	}

	bool Run(const IR::SrtFlatRoot& root) {
		if (!root.valid || root.first + root.count > m_program.schedule.size()) {
			return false;
		}
		for (uint32_t step = 0; step < root.count; step++) {
			const auto index = m_program.schedule[root.first + step];
			if (index >= m_program.insts.size()) {
				return false;
			}
			if (m_written[index] != 0u) {
				continue;
			}
			uint32_t value     = 0;
			uint32_t condition = m_true;
			if (!Execute(m_program.insts[index], value, condition)) {
				return false;
			}
			m_values[index]     = value;
			m_conditions[index] = condition;
			m_written[index]    = 1u;
		}
		return true;
	}

	[[nodiscard]] bool Produced(uint32_t reg) const {
		return reg < m_written.size() && m_written[reg] != 0u;
	}
	[[nodiscard]] uint32_t Value(uint32_t reg) const { return m_values[reg]; }
	[[nodiscard]] uint32_t Condition(uint32_t reg) const { return m_conditions[reg]; }
	[[nodiscard]] uint32_t True() const { return m_true; }

	uint32_t And(uint32_t lhs, uint32_t rhs) {
		if (lhs == m_true) {
			return rhs;
		}
		if (rhs == m_true) {
			return lhs;
		}
		return Binary(m_state, OpLogicalAnd, TypeBool(m_state), lhs, rhs);
	}

private:
	uint32_t Arg(const IR::SrtFlatInst& inst, size_t index) const {
		return m_values[inst.args[index]];
	}

	uint32_t ArgCondition(const IR::SrtFlatInst& inst, size_t index) const {
		return m_conditions[inst.args[index]];
	}

	uint32_t ArgLow(const IR::SrtFlatInst& inst, size_t index) {
		return Narrow(m_state, Arg(inst, index));
	}

	uint32_t Operands(const IR::SrtFlatInst& inst) {
		uint32_t condition = m_true;
		for (uint32_t index = 0; index < inst.arg_count; index++) {
			condition = And(condition, ArgCondition(inst, index));
		}
		return condition;
	}

	// FlatMachine reads guest memory through the runtime's reader; the shader reads through the
	// BDA page table. Two differences the caller must know about:
	//   - The page table describes 40 bits, FlatMachine masks to 48. Anything at or above the
	//     40-bit limit is reported as a failed read rather than indexing the table out of range.
	//   - A page the table does not map yields zero and records a fault bit for the host to
	//     service, where FlatMachine's reader simply fails. The read reports "not present" so a
	//     root's validity still agrees; the fault bit is an extra shader-side side effect.
	GuestRead ReadGuestDword(uint32_t address, uint32_t condition) {
		auto&      state    = m_state;
		const auto in_range = Binary(state, OpULessThan, TypeBool(state), address,
		                             WideConstant(state, SrtBdaLimit));
		const auto reachable = And(condition, in_range);

		const auto then_label  = state.builder.AllocateId();
		const auto then_exit   = state.builder.AllocateId();
		const auto else_label  = state.builder.AllocateId();
		const auto merge_label = state.builder.AllocateId();
		state.builder.AddFunction({OpSelectionMerge, merge_label, SelectionControlNone});
		state.builder.AddFunction({OpBranchConditional, reachable, then_label, else_label});

		EmitLabel(state, then_label);
		const auto bda = EmitBdaPointer(m_ctx, address);
		const auto mapped =
		    Binary(state, OpINotEqual, TypeBool(state), bda, WideConstant(state, 0));
		const auto loaded = EmitValueOrZeroIfCondition(state, mapped, [&]() {
			const auto pointer = state.builder.AllocateId();
			state.builder.AddFunction(
			    {OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer, bda});
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction({OpLoad, TypeU32(state), value, pointer,
			                           MemoryAccessAlignedMask, sizeof(uint32_t)});
			return value;
		});
		state.builder.AddFunction({OpBranch, then_exit});
		EmitLabel(state, then_exit);
		state.builder.AddFunction({OpBranch, merge_label});

		EmitLabel(state, else_label);
		state.builder.AddFunction({OpBranch, merge_label});

		EmitLabel(state, merge_label);
		GuestRead read;
		read.value = state.builder.AllocateId();
		state.builder.AddFunction({OpPhi, TypeU32(state), read.value, loaded, then_exit,
		                           ConstantU32(state, 0), else_label});
		read.present = state.builder.AllocateId();
		state.builder.AddFunction({OpPhi, TypeBool(state), read.present, mapped, then_exit,
		                           ConstantBool(state, false), else_label});
		return read;
	}

	// base = ((high << 32) | (uint32_t)low) & AddressMask, over full 64-bit registers.
	uint32_t GuestBase(const IR::SrtFlatInst& inst) {
		auto&      state  = m_state;
		const auto wide   = TypeScalarU64(state);
		const auto high   = Binary(state, OpShiftLeftLogical, wide, Arg(inst, 1),
		                           WideConstant(state, 32));
		const auto merged = Binary(state, OpBitwiseOr, wide, high, Widen(state, ArgLow(inst, 0)));
		return Binary(state, OpBitwiseAnd, wide, merged, WideConstant(state, SrtAddressMask));
	}

	// AddSignedAddress over a runtime 64-bit signed displacement. The base is already inside the
	// address mask, so the walker's "base > AddressMask" rejection can never fire here.
	void AddSignedAddress(uint32_t base, uint32_t relative, uint32_t& address, uint32_t& ok) {
		auto&      state = m_state;
		const auto wide  = TypeScalarU64(state);
		const auto negative =
		    Binary(state, OpUGreaterThanEqual, TypeBool(state), relative,
		           WideConstant(state, uint64_t {1} << 63u));
		const auto magnitude = Binary(state, OpISub, wide, WideConstant(state, 0), relative);
		const auto under = Binary(state, OpUGreaterThan, TypeBool(state), magnitude, base);
		const auto below = Binary(state, OpISub, wide, base, magnitude);
		const auto room =
		    Binary(state, OpISub, wide, WideConstant(state, SrtAddressMask), base);
		const auto over  = Binary(state, OpUGreaterThan, TypeBool(state), relative, room);
		const auto above = Binary(state, OpIAdd, wide, base, relative);
		address          = Select(state, wide, negative, below, above);
		ok = Select(state, TypeBool(state), negative, NotBool(state, under), NotBool(state, over));
	}

	bool Execute(const IR::SrtFlatInst& inst, uint32_t& result, uint32_t& condition) {
		auto&      state = m_state;
		const auto wide  = TypeScalarU64(state);
		const auto u32   = TypeU32(state);
		const auto boolean = TypeBool(state);
		condition          = Operands(inst);

		const auto narrow_binary = [&](uint32_t opcode) {
			result = Widen(state, Binary(state, opcode, u32, ArgLow(inst, 0), ArgLow(inst, 1)));
		};
		const auto wide_binary = [&](uint32_t opcode) {
			result = Binary(state, opcode, wide, Arg(inst, 0), Arg(inst, 1));
		};
		const auto compare32 = [&](uint32_t opcode) {
			result = BoolRegister(
			    state, Binary(state, opcode, boolean, ArgLow(inst, 0), ArgLow(inst, 1)));
		};
		const auto float_compare = [&](uint32_t opcode) {
			result = BoolRegister(state,
			                      Binary(state, opcode, boolean, BitcastTo(state, TypeF32(state),
			                                                               ArgLow(inst, 0)),
			                             BitcastTo(state, TypeF32(state), ArgLow(inst, 1))));
		};
		// FlatMachine shifts uint32_t by (amount & 31) and uint64_t by (amount & 63).
		const auto shift32 = [&](uint32_t opcode, bool arithmetic) {
			const auto amount =
			    Binary(state, OpBitwiseAnd, u32, ArgLow(inst, 1), ConstantU32(state, 31));
			if (!arithmetic) {
				result = Widen(state, Binary(state, opcode, u32, ArgLow(inst, 0), amount));
				return;
			}
			const auto signed_value = BitcastTo(state, TypeI32(state), ArgLow(inst, 0));
			const auto shifted = Binary(state, opcode, TypeI32(state), signed_value, amount);
			result             = Widen(state, BitcastTo(state, u32, shifted));
		};
		const auto shift64 = [&](uint32_t opcode, bool arithmetic) {
			const auto amount =
			    Binary(state, OpBitwiseAnd, wide, Arg(inst, 1), WideConstant(state, 63));
			if (!arithmetic) {
				result = Binary(state, opcode, wide, Arg(inst, 0), amount);
				return;
			}
			const auto signed_type  = TypeScalarS64(state);
			const auto signed_value = BitcastTo(state, signed_type, Arg(inst, 0));
			const auto shifted = Binary(state, opcode, signed_type, signed_value, amount);
			result             = BitcastTo(state, wide, shifted);
		};
		// Both bitfield extracts and the insert share the walker's operand rejection.
		const auto field_bounds = [&](uint32_t offset, uint32_t width) {
			const auto offset_ok = Binary(state, OpULessThanEqual, boolean, offset,
			                              ConstantU32(state, 32));
			const auto room = Binary(state, OpISub, u32, ConstantU32(state, 32), offset);
			const auto width_ok = Binary(state, OpULessThanEqual, boolean, width, room);
			return And(offset_ok, width_ok);
		};
		// width == 32 ? UINT32_MAX : (1 << width) - 1, with width == 0 giving zero.
		const auto field_mask = [&](uint32_t width) {
			const auto safe = Binary(state, OpBitwiseAnd, u32, width, ConstantU32(state, 31));
			const auto bit  = Binary(state, OpShiftLeftLogical, u32, ConstantU32(state, 1), safe);
			const auto low  = Binary(state, OpISub, u32, bit, ConstantU32(state, 1));
			const auto full = Binary(state, OpUGreaterThanEqual, boolean, width,
			                         ConstantU32(state, 32));
			return Select(state, u32, full, ConstantU32(state, UINT32_MAX), low);
		};

		switch (inst.op) {
			case IR::SrtFlatOp::Imm: result = WideConstant(state, inst.imm); return true;
			case IR::SrtFlatOp::UserData: {
				// The walker rejects a register outside the runtime's user-data span; the shader
				// only reaches registers the binding layout packed, which is the same set.
				uint32_t dword = 0;
				if (inst.imm > UINT32_MAX ||
				    !UserDataDword(state, state.program.user_data_base +
				                              static_cast<uint32_t>(inst.imm),
				                   dword)) {
					return false;
				}
				result = Widen(state, EmitShaderDataDwordLoad(state, dword));
				return true;
			}
			case IR::SrtFlatOp::ShaderBase:
				result = WideConstant(state, m_inputs.shader_base);
				return true;
			case IR::SrtFlatOp::ReadAddress: {
				if (inst.clean != 0u) {
					// The clean context reads through the host specialization reader, which has
					// no shader-side equivalent.
					return false;
				}
				const auto base = Binary(state, OpBitwiseAnd, wide, GuestBase(inst),
				                         WideConstant(state, ~uint64_t {3}));
				const auto immediate =
				    static_cast<int64_t>(inst.imm) & ~int64_t {3};
				const auto offset =
				    Binary(state, OpBitwiseAnd, u32, ArgLow(inst, 2), ConstantU32(state, ~3u));
				const auto relative =
				    Binary(state, OpIAdd, wide,
				           WideConstant(state, static_cast<uint64_t>(immediate)),
				           Widen(state, offset));
				uint32_t address = 0;
				uint32_t ok      = 0;
				AddSignedAddress(base, relative, address, ok);
				const auto read = ReadGuestDword(address, And(condition, ok));
				condition       = And(And(condition, ok), read.present);
				result          = Widen(state, read.value);
				return true;
			}
			case IR::SrtFlatOp::ReadBuffer: {
				if (inst.clean != 0u) {
					return false;
				}
				const auto base = GuestBase(inst);
				const auto byte_offset = Binary(state, OpIAdd, wide, WideConstant(state, inst.imm),
				                                Widen(state, ArgLow(inst, 3)));
				const auto aligned     = Binary(state, OpBitwiseAnd, wide, byte_offset,
				                                WideConstant(state, ~uint64_t {3}));
				const auto stride      = Binary(
				         state, OpBitwiseAnd, u32,
				         Binary(state, OpShiftRightLogical, u32, ArgLow(inst, 1),
				                ConstantU32(state, 16)),
				         ConstantU32(state, 0x3fffu));
				const auto records   = Widen(state, ArgLow(inst, 2));
				const auto strided   = Binary(state, OpIMul, wide, Widen(state, stride), records);
				const auto unstrided = Binary(state, OpIEqual, boolean, stride,
				                              ConstantU32(state, 0));
				const auto size      = Select(state, wide, unstrided, records, strided);
				const auto fits = Binary(state, OpULessThanEqual, boolean, aligned, size);
				const auto room = Binary(state, OpISub, wide, size, aligned);
				const auto whole =
				    Binary(state, OpUGreaterThanEqual, boolean, room, WideConstant(state, 4));
				const auto ok      = And(fits, whole);
				const auto address = Binary(
				    state, OpBitwiseAnd, wide,
				    Binary(state, OpIAdd, wide,
				           Binary(state, OpBitwiseAnd, wide, base, WideConstant(state, ~uint64_t {3})),
				           byte_offset),
				    WideConstant(state, ~uint64_t {3}));
				const auto read = ReadGuestDword(address, And(condition, ok));
				condition       = And(And(condition, ok), read.present);
				result          = Widen(state, read.value);
				return true;
			}
			case IR::SrtFlatOp::ExtractU64:
				result = Widen(state, Narrow(state, Binary(state, OpShiftRightLogical, wide,
				                                           Arg(inst, 0),
				                                           WideConstant(state, inst.imm * 32u))));
				return true;
			case IR::SrtFlatOp::AddCarry: {
				const auto sum = Binary(state, OpIAdd, wide, Widen(state, ArgLow(inst, 0)),
				                        Widen(state, ArgLow(inst, 1)));
				const auto selected =
				    inst.imm == 0u
				        ? sum
				        : Binary(state, OpShiftRightLogical, wide, sum, WideConstant(state, 32));
				result = Widen(state, Narrow(state, selected));
				return true;
			}
			case IR::SrtFlatOp::ConstructU64:
				result = Binary(state, OpBitwiseOr, wide, Widen(state, ArgLow(inst, 0)),
				                Binary(state, OpShiftLeftLogical, wide,
				                       Widen(state, ArgLow(inst, 1)), WideConstant(state, 32)));
				return true;
			case IR::SrtFlatOp::IAdd32: narrow_binary(OpIAdd); return true;
			case IR::SrtFlatOp::IAdd64: wide_binary(OpIAdd); return true;
			case IR::SrtFlatOp::ISub32: narrow_binary(OpISub); return true;
			case IR::SrtFlatOp::ISub64: wide_binary(OpISub); return true;
			case IR::SrtFlatOp::IMul32: narrow_binary(OpIMul); return true;
			case IR::SrtFlatOp::IMul64: wide_binary(OpIMul); return true;
			case IR::SrtFlatOp::UMin32:
				result = Widen(state, GlslBinary(state, u32, GlslUMin, ArgLow(inst, 0),
				                                 ArgLow(inst, 1)));
				return true;
			case IR::SrtFlatOp::ConvertF32U32:
				result = Widen(state, BitcastTo(state, u32,
				                                Unary(state, OpConvertUToF, TypeF32(state),
				                                      ArgLow(inst, 0))));
				return true;
			case IR::SrtFlatOp::ConvertU32F32: {
				const auto value = BitcastTo(state, TypeF32(state), ArgLow(inst, 0));
				const auto nan   = Unary(state, OpIsNan, boolean, value);
				const auto infinite = Unary(state, OpIsInf, boolean, value);
				const auto below    = Binary(state, OpFOrdLessThan, boolean, value,
				                             ConstantF32(state, 0u));
				// (double)value > UINT32_MAX is true for every float at or above 2^32.
				const auto above = Binary(state, OpFOrdGreaterThanEqual, boolean, value,
				                          ConstantF32(state, FloatBits(4294967296.0f)));
				const auto ok    = NotBool(
				       state, Binary(state, OpLogicalOr, boolean,
				                     Binary(state, OpLogicalOr, boolean, nan, infinite),
				                     Binary(state, OpLogicalOr, boolean, below, above)));
				condition = And(condition, ok);
				// Out-of-range inputs would trap OpConvertFToU, so clamp the converted value.
				const auto safe =
				    Select(state, TypeF32(state), ok, value, ConstantF32(state, 0u));
				result = Widen(state, Unary(state, OpConvertFToU, u32, safe));
				return true;
			}
			case IR::SrtFlatOp::FPMul32:
				result = Widen(
				    state, BitcastTo(state, u32,
				                     Binary(state, OpFMul, TypeF32(state),
				                            BitcastTo(state, TypeF32(state), ArgLow(inst, 0)),
				                            BitcastTo(state, TypeF32(state), ArgLow(inst, 1)))));
				return true;
			case IR::SrtFlatOp::FPTrunc32:
				result = Widen(
				    state, BitcastTo(state, u32,
				                     GlslUnary(state, TypeF32(state), GlslTrunc,
				                               BitcastTo(state, TypeF32(state), ArgLow(inst, 0)))));
				return true;
			case IR::SrtFlatOp::FPIsNan32:
				result = BoolRegister(state,
				                      Unary(state, OpIsNan, boolean,
				                            BitcastTo(state, TypeF32(state), ArgLow(inst, 0))));
				return true;
			case IR::SrtFlatOp::FPOrdLessThanEqual32:
				float_compare(OpFOrdLessThanEqual);
				return true;
			case IR::SrtFlatOp::FPOrdGreaterThanEqual32:
				float_compare(OpFOrdGreaterThanEqual);
				return true;
			case IR::SrtFlatOp::BitwiseAnd32: narrow_binary(OpBitwiseAnd); return true;
			case IR::SrtFlatOp::BitwiseAnd64: wide_binary(OpBitwiseAnd); return true;
			case IR::SrtFlatOp::BitwiseOr32: narrow_binary(OpBitwiseOr); return true;
			case IR::SrtFlatOp::BitwiseXor32: narrow_binary(OpBitwiseXor); return true;
			case IR::SrtFlatOp::BitwiseNot32:
				result = Widen(state, Unary(state, OpNot, u32, ArgLow(inst, 0)));
				return true;
			case IR::SrtFlatOp::BitCount32:
				result = Widen(state, Unary(state, OpBitCount, u32, ArgLow(inst, 0)));
				return true;
			case IR::SrtFlatOp::FindILsb32:
				// GLSL FindILsb yields -1 for zero, matching the walker's UINT32_MAX.
				result = Widen(state, GlslUnary(state, u32, GlslFindILsb, ArgLow(inst, 0)));
				return true;
			case IR::SrtFlatOp::FindUMsb32:
				result = Widen(state, GlslUnary(state, u32, GlslFindUMsb, ArgLow(inst, 0)));
				return true;
			case IR::SrtFlatOp::ShiftLeftLogical32: shift32(OpShiftLeftLogical, false); return true;
			case IR::SrtFlatOp::ShiftLeftLogical64: shift64(OpShiftLeftLogical, false); return true;
			case IR::SrtFlatOp::ShiftRightLogical32:
				shift32(OpShiftRightLogical, false);
				return true;
			case IR::SrtFlatOp::ShiftRightLogical64:
				shift64(OpShiftRightLogical, false);
				return true;
			case IR::SrtFlatOp::ShiftRightArithmetic32:
				shift32(OpShiftRightArithmetic, true);
				return true;
			case IR::SrtFlatOp::ShiftRightArithmetic64:
				shift64(OpShiftRightArithmetic, true);
				return true;
			case IR::SrtFlatOp::BitFieldUExtract: {
				const auto offset = ArgLow(inst, 1);
				const auto width  = ArgLow(inst, 2);
				condition         = And(condition, field_bounds(offset, width));
				const auto safe_offset =
				    Binary(state, OpBitwiseAnd, u32, offset, ConstantU32(state, 31));
				const auto shifted =
				    Binary(state, OpShiftRightLogical, u32, ArgLow(inst, 0), safe_offset);
				const auto bits =
				    Binary(state, OpBitwiseAnd, u32, shifted, field_mask(width));
				const auto empty = Binary(state, OpIEqual, boolean, width, ConstantU32(state, 0));
				result = Widen(state, Select(state, u32, empty, ConstantU32(state, 0), bits));
				return true;
			}
			case IR::SrtFlatOp::BitFieldSExtract: {
				const auto offset = ArgLow(inst, 1);
				const auto width  = ArgLow(inst, 2);
				condition         = And(condition, field_bounds(offset, width));
				const auto safe_offset =
				    Binary(state, OpBitwiseAnd, u32, offset, ConstantU32(state, 31));
				const auto mask    = field_mask(width);
				const auto shifted =
				    Binary(state, OpShiftRightLogical, u32, ArgLow(inst, 0), safe_offset);
				const auto bits  = Binary(state, OpBitwiseAnd, u32, shifted, mask);
				const auto empty = Binary(state, OpIEqual, boolean, width, ConstantU32(state, 0));
				const auto top   = Binary(
				      state, OpShiftLeftLogical, u32, ConstantU32(state, 1),
				      Binary(state, OpBitwiseAnd, u32,
				             Binary(state, OpISub, u32, width, ConstantU32(state, 1)),
				             ConstantU32(state, 31)));
				const auto narrower =
				    Binary(state, OpULessThan, boolean, width, ConstantU32(state, 32));
				const auto set = Binary(state, OpINotEqual, boolean,
				                        Binary(state, OpBitwiseAnd, u32, bits, top),
				                        ConstantU32(state, 0));
				const auto extend = Binary(state, OpLogicalAnd, boolean, narrower, set);
				const auto extended =
				    Binary(state, OpBitwiseOr, u32, bits, Unary(state, OpNot, u32, mask));
				const auto value = Select(state, u32, extend, extended, bits);
				result = Widen(state, Select(state, u32, empty, ConstantU32(state, 0), value));
				return true;
			}
			case IR::SrtFlatOp::BitFieldInsert: {
				const auto offset = ArgLow(inst, 2);
				const auto width  = ArgLow(inst, 3);
				condition         = And(condition, field_bounds(offset, width));
				const auto safe_offset =
				    Binary(state, OpBitwiseAnd, u32, offset, ConstantU32(state, 31));
				const auto full = Binary(state, OpUGreaterThanEqual, boolean, width,
				                         ConstantU32(state, 32));
				const auto shifted_mask = Binary(state, OpShiftLeftLogical, u32,
				                                 field_mask(width), safe_offset);
				const auto mask =
				    Select(state, u32, full, ConstantU32(state, UINT32_MAX), shifted_mask);
				const auto kept = Binary(state, OpBitwiseAnd, u32, ArgLow(inst, 0),
				                         Unary(state, OpNot, u32, mask));
				const auto inserted = Binary(
				    state, OpBitwiseAnd, u32,
				    Binary(state, OpShiftLeftLogical, u32, ArgLow(inst, 1), safe_offset), mask);
				const auto merged = Binary(state, OpBitwiseOr, u32, kept, inserted);
				const auto empty  = Binary(state, OpIEqual, boolean, width, ConstantU32(state, 0));
				result = Widen(state, Select(state, u32, empty, ArgLow(inst, 0), merged));
				return true;
			}
			case IR::SrtFlatOp::Select:
				// The walker tests the whole 64-bit register against zero.
				result = Select(state, wide, NonZero(state, Arg(inst, 0)), Arg(inst, 1),
				                Arg(inst, 2));
				return true;
			case IR::SrtFlatOp::IEqual32: compare32(OpIEqual); return true;
			case IR::SrtFlatOp::INotEqual32: compare32(OpINotEqual); return true;
			case IR::SrtFlatOp::ULessThan32: compare32(OpULessThan); return true;
			case IR::SrtFlatOp::UGreaterThan32: compare32(OpUGreaterThan); return true;
			case IR::SrtFlatOp::LogicalAnd:
				result = BoolRegister(state, Binary(state, OpLogicalAnd, boolean,
				                                    NonZero(state, Arg(inst, 0)),
				                                    NonZero(state, Arg(inst, 1))));
				return true;
			case IR::SrtFlatOp::LogicalOr:
				result = BoolRegister(state, Binary(state, OpLogicalOr, boolean,
				                                    NonZero(state, Arg(inst, 0)),
				                                    NonZero(state, Arg(inst, 1))));
				return true;
			case IR::SrtFlatOp::LogicalXor:
				result = BoolRegister(state, Binary(state, OpLogicalNotEqual, boolean,
				                                    NonZero(state, Arg(inst, 0)),
				                                    NonZero(state, Arg(inst, 1))));
				return true;
			case IR::SrtFlatOp::LogicalNot:
				result = BoolRegister(state, NotBool(state, NonZero(state, Arg(inst, 0))));
				return true;
		}
		return false;
	}

	ValueEmitContext&         m_ctx;
	EmitterState&             m_state;
	const IR::SrtFlatProgram& m_program;
	SrtLoweringInputs         m_inputs;
	std::vector<uint32_t>     m_values;
	std::vector<uint32_t>     m_conditions;
	std::vector<uint8_t>      m_written;
	uint32_t                  m_true = 0;
};

// One root on an emitter that may already hold the steps of the roots before it.
SrtLoweredRoot RunRoot(RootEmitter& emitter, ValueEmitContext& ctx, const IR::SrtFlatRoot& root,
                       std::span<const uint32_t> results) {
	SrtLoweredRoot lowered;
	lowered.count = static_cast<uint32_t>(std::min(results.size(), lowered.results.size()));
	if (!emitter.Run(root)) {
		lowered.supported = false;
		lowered.valid     = ConstantBool(ctx.state, false);
		return lowered;
	}
	lowered.valid = emitter.True();
	for (uint32_t index = 0; index < lowered.count; index++) {
		// The schedule is the closure of the results, so every result register must have been
		// produced; anything else means the root and the requested results disagree.
		if (!emitter.Produced(results[index])) {
			lowered           = {};
			lowered.supported = false;
			lowered.valid     = ConstantBool(ctx.state, false);
			return lowered;
		}
		lowered.results[index] = emitter.Value(results[index]);
		lowered.valid          = emitter.And(lowered.valid, emitter.Condition(results[index]));
	}
	return lowered;
}

} // namespace

void EmitSrtFlatRoots(ValueEmitContext& ctx, const IR::SrtFlatProgram& program,
                      std::span<const SrtRootRequest> requests, const SrtLoweringInputs& inputs,
                      std::span<SrtLoweredRoot> lowered) {
	RootEmitter emitter(ctx, program, inputs);
	for (size_t index = 0; index < requests.size() && index < lowered.size(); index++) {
		const auto& request = requests[index];
		if (request.root == nullptr) {
			lowered[index]           = {};
			lowered[index].supported = false;
			lowered[index].valid     = ConstantBool(ctx.state, false);
			continue;
		}
		lowered[index] = RunRoot(emitter, ctx, *request.root, request.results);
	}
}

SrtLoweredRoot EmitSrtFlatRoot(ValueEmitContext& ctx, const IR::SrtFlatProgram& program,
                               const IR::SrtFlatRoot& root, std::span<const uint32_t> results,
                               const SrtLoweringInputs& inputs) {
	SrtLoweredRoot             lowered;
	const SrtRootRequest       request {&root, results};
	EmitSrtFlatRoots(ctx, program, {&request, 1}, inputs, {&lowered, 1});
	return lowered;
}

// Test-only module: one compute entry point that runs every descriptor-source root of `flat` and
// stores its dwords into storage buffer 0 at a fixed stride, followed by the root's validity.
std::vector<uint32_t> EmitSrtFlatProgramTestModule(const IR::Program& program,
                                                   ShaderStageInputInfo      input_info,
                                                   const IR::SrtFlatProgram& flat,
                                                   std::span<const uint32_t> dword_counts,
                                                   const SrtLoweringInputs&  inputs,
                                                   uint32_t                  source_stride,
                                                   uint32_t                  read_origin,
                                                   uint32_t                  read_stride) {
	EmitterState state(program, input_info);
	state.stage      = program.stage;
	state.lane_count = 1;
	state.inputs.reserve(program.info.inputs.size());
	state.outputs.reserve(program.info.outputs.size());
	state.interface_variables.reserve(program.info.inputs.size() + program.info.outputs.size());
	CopyProgramInputsAndOutputs(state, program);
	AllocateInputVariables(state);
	AllocateOutputVariables(state);
	DefineModule(state);

	ValueEmitContext ctx(state);
	DefineGetBdaPointer(state);
	DefineGetBdaProloguePointer(state);
	state.builder.AddFunction({OpFunction, TypeVoid(state), state.main_func, FunctionControlNone,
	                           TypeFunction(state)});
	EmitLabel(state, state.entry_label);
	EmitMemoryOffsets(state);

	IR::MemoryInfo output {};
	output.kind         = IR::ResourceKind::Buffer;
	output.resource     = 0;
	const auto resource = PrepareMemoryResourceAccess(state, output);
	const auto store    = [&](uint32_t dword, uint32_t value) {
        const auto index =
            EmitMemoryElementIndex(state, resource, ConstantU32(state, dword));
        EmitIfCondition(state, EmitMemoryElementInBounds(state, resource, index), [&]() {
            const auto pointer = EmitMemoryElementPointer(state, resource, index);
            state.builder.AddFunction({OpStore, pointer, value});
        });
	};

	for (uint32_t source = 0; source < flat.sources.size(); source++) {
		const auto count = source < dword_counts.size() ? dword_counts[source] : 0u;
		const auto& root = flat.sources[source];
		std::vector<uint32_t> registers(root.results.begin(),
		                                root.results.begin() + std::min<size_t>(count, 8));
		const auto lowered = EmitSrtFlatRoot(ctx, flat, root, registers, inputs);
		const auto base    = source * source_stride;
		for (uint32_t dword = 0; dword < lowered.count; dword++) {
			store(base + dword, Narrow(state, lowered.results[dword]));
		}
		store(base + 8u, Select(state, TypeU32(state), lowered.valid, ConstantU32(state, 1),
		                        ConstantU32(state, 0)));
		store(base + 9u, ConstantU32(state, lowered.supported ? 1u : 0u));
	}

	// Stage 1b: the same treatment for the flat SRT read roots, whose single result is what
	// EmitGpuFetchDescriptors parks for ReadConst. An invalid root reads zero, as it does there.
	for (uint32_t slot = 0; read_stride != 0u && slot < flat.flat_reads.size(); slot++) {
		const auto&                   root = flat.flat_reads[slot];
		const std::array<uint32_t, 1> registers {root.result};
		const auto lowered = EmitSrtFlatRoot(ctx, flat, root, registers, inputs);
		const auto base    = read_origin + slot * read_stride;
		const auto value   = lowered.count == 1u
		                         ? Select(state, TypeU32(state), lowered.valid,
		                                  Narrow(state, lowered.results[0]), ConstantU32(state, 0))
		                         : ConstantU32(state, 0);
		store(base, value);
		store(base + 1u, Select(state, TypeU32(state), lowered.valid, ConstantU32(state, 1),
		                        ConstantU32(state, 0)));
		store(base + 2u, ConstantU32(state, lowered.supported ? 1u : 0u));
	}

	state.builder.AddFunction({OpReturn});
	state.builder.AddFunction({OpFunctionEnd});
	state.builder.AddEntryPoint(ExecutionModelForStage(state.stage), state.main_func, "main",
	                            state.interface_variables);
	return state.builder.Build();
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
