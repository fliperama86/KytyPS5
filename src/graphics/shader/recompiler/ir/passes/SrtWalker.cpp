#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/assert.h"
#if defined(TRACY_ENABLE)
#include "common/profiler.h"
#endif
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <initializer_list>
#include <memory_resource>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

// Stage 1 diagnostics (docs/gpu-descriptor-fetch.md): the last failed evaluation on this thread.
// Only the failure path touches it.
SrtFailure& ThreadFailure() {
	static thread_local SrtFailure failure;
	return failure;
}

const char* SrtFlatOpName(SrtFlatOp op) {
	switch (op) {
		case SrtFlatOp::Imm: return "Imm";
		case SrtFlatOp::UserData: return "UserData";
		case SrtFlatOp::ShaderBase: return "ShaderBase";
		case SrtFlatOp::ReadAddress: return "ReadAddress";
		case SrtFlatOp::ReadBuffer: return "ReadBuffer";
		case SrtFlatOp::ExtractU64: return "ExtractU64";
		case SrtFlatOp::AddCarry: return "AddCarry";
		case SrtFlatOp::ConstructU64: return "ConstructU64";
		case SrtFlatOp::IAdd32: return "IAdd32";
		case SrtFlatOp::IAdd64: return "IAdd64";
		case SrtFlatOp::ISub32: return "ISub32";
		case SrtFlatOp::ISub64: return "ISub64";
		case SrtFlatOp::IMul32: return "IMul32";
		case SrtFlatOp::IMul64: return "IMul64";
		case SrtFlatOp::UMin32: return "UMin32";
		case SrtFlatOp::ConvertF32U32: return "ConvertF32U32";
		case SrtFlatOp::ConvertU32F32: return "ConvertU32F32";
		case SrtFlatOp::FPMul32: return "FPMul32";
		case SrtFlatOp::FPTrunc32: return "FPTrunc32";
		case SrtFlatOp::FPIsNan32: return "FPIsNan32";
		case SrtFlatOp::FPOrdLessThanEqual32: return "FPOrdLessThanEqual32";
		case SrtFlatOp::FPOrdGreaterThanEqual32: return "FPOrdGreaterThanEqual32";
		case SrtFlatOp::BitwiseAnd32: return "BitwiseAnd32";
		case SrtFlatOp::BitwiseAnd64: return "BitwiseAnd64";
		case SrtFlatOp::BitwiseOr32: return "BitwiseOr32";
		case SrtFlatOp::BitwiseXor32: return "BitwiseXor32";
		case SrtFlatOp::BitwiseNot32: return "BitwiseNot32";
		case SrtFlatOp::BitCount32: return "BitCount32";
		case SrtFlatOp::FindILsb32: return "FindILsb32";
		case SrtFlatOp::FindUMsb32: return "FindUMsb32";
		case SrtFlatOp::ShiftLeftLogical32: return "ShiftLeftLogical32";
		case SrtFlatOp::ShiftLeftLogical64: return "ShiftLeftLogical64";
		case SrtFlatOp::ShiftRightLogical32: return "ShiftRightLogical32";
		case SrtFlatOp::ShiftRightLogical64: return "ShiftRightLogical64";
		case SrtFlatOp::ShiftRightArithmetic32: return "ShiftRightArithmetic32";
		case SrtFlatOp::ShiftRightArithmetic64: return "ShiftRightArithmetic64";
		case SrtFlatOp::BitFieldUExtract: return "BitFieldUExtract";
		case SrtFlatOp::BitFieldSExtract: return "BitFieldSExtract";
		case SrtFlatOp::BitFieldInsert: return "BitFieldInsert";
		case SrtFlatOp::Select: return "Select";
		case SrtFlatOp::IEqual32: return "IEqual32";
		case SrtFlatOp::INotEqual32: return "INotEqual32";
		case SrtFlatOp::ULessThan32: return "ULessThan32";
		case SrtFlatOp::UGreaterThan32: return "UGreaterThan32";
		case SrtFlatOp::LogicalAnd: return "LogicalAnd";
		case SrtFlatOp::LogicalOr: return "LogicalOr";
		case SrtFlatOp::LogicalXor: return "LogicalXor";
		case SrtFlatOp::LogicalNot: return "LogicalNot";
	}
	return "?";
}

void ResetFailure(SrtFailure& failure) {
	failure.valid         = false;
	failure.flat          = false;
	failure.kind          = "";
	failure.index         = UINT32_MAX;
	failure.root_first    = 0;
	failure.root_count    = 0;
	failure.root_valid    = false;
	failure.inst_index    = UINT32_MAX;
	failure.op_name       = "";
	failure.arg_count     = 0;
	failure.args          = {};
	failure.arg_values    = {};
	failure.arg_defined   = {};
	failure.imm           = 0;
	failure.clean         = false;
	failure.address_valid = false;
	failure.address       = 0;
	failure.memory_op     = false;
	failure.base          = 0;
	failure.byte_offset   = 0;
	failure.records       = 0;
	failure.stride        = 0;
	failure.bound         = 0;
	failure.user_data.clear();
	failure.user_data_size = 0;
	failure.trace.clear();
}

// Names the failing slot. The flat machine has already filled the instruction detail; the IR
// walker has none, so its record carries the slot only.
void TagFailure(const char* kind, uint32_t index, bool flat) {
	auto& failure = ThreadFailure();
	if (!flat) {
		ResetFailure(failure);
	}
	failure.valid = true;
	failure.flat  = flat;
	failure.kind  = kind;
	failure.index = index;
}

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

std::string Diagnostic(const ResourcePlan& program, uint32_t pc, const std::string& message) {
	return fmt::format("shader SRT: hash=0x{:016x} stage={} pc=0x{:08x} {}", program.shader_hash,
	                   StageName(program.stage), pc, message);
}

bool AddSignedAddress(uint64_t base, int64_t offset, uint64_t& result) {
	if (base > AddressMask) {
		return false;
	}
	if (offset < 0) {
		const auto magnitude = uint64_t {0} - static_cast<uint64_t>(offset);
		if (magnitude > base) {
			return false;
		}
		result = base - magnitude;
		return true;
	}
	const auto magnitude = static_cast<uint64_t>(offset);
	if (magnitude > AddressMask - base) {
		return false;
	}
	result = base + magnitude;
	return true;
}

bool IsRawRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= values.memory_info.size()) {
		return false;
	}
	const auto kind = values.memory_info[index].kind;
	return (op == ValueOpcode::LoadAddressU32 && kind == ResourceKind::ScalarAddress) ||
	       (op == ValueOpcode::ReadConstBuffer && kind == ResourceKind::ScalarBuffer);
}

bool IsDescriptorHandle(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetBufferResource:
		case ValueOpcode::GetAddressResource:
		case ValueOpcode::GetImageResource:
		case ValueOpcode::GetSamplerResource: return true;
		default: return false;
	}
}

bool IsRuntimeSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
	       op == ValueOpcode::SelectF32;
}

bool IsRuntimeUniformOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::BitCount32:
		case ValueOpcode::FindILsb32:
		case ValueOpcode::FindUMsb32:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::IAddCarry32:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::UMin32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPTrunc32: return true;
		default: return false;
	}
}

class RuntimeValidator {
public:
	explicit RuntimeValidator(const ResourcePlan& program, RuntimeValueType type)
	    : m_program(program), m_type(type) {}

	bool Run(Value value) { return Validate(value); }

	const std::string& Reason() const { return m_reason; }

private:
	bool Reject(std::string reason) {
		if (m_reason.empty()) {
			m_reason = std::move(reason);
		}
		return false;
	}

	std::string Describe(Value value, uint32_t depth = 0) const {
		value            = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return value.IsImmediate() && value.GetType() == Type::U32
			           ? fmt::format("0x{:08x}", value.U32())
			           : std::string("opaque");
		}
		const auto op   = inst->GetOpcode();
		auto       text = std::string(ValueOpcodeName(op));
		if (op == ValueOpcode::GetUserData && inst->NumArgs() == 1 &&
		    inst->Arg(0).GetType() == Type::ScalarReg) {
			return text + fmt::format(" s{}", RegIndex(inst->Arg(0).ScalarRegister()));
		}
		if (op == ValueOpcode::ReadConst && inst->NumArgs() == 2 &&
		    inst->Arg(1).Resolve().IsImmediate()) {
			return text + fmt::format(" slot={}", inst->Arg(1).Resolve().U32());
		}
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
			const auto flags = inst->Flags<MemoryFlags>();
			text += fmt::format(" pc=0x{:08x}", flags.pc);
			if (flags.index < m_program.memory_info.size()) {
				text += fmt::format(" offset=0x{:x}", m_program.memory_info[flags.index].offset);
			}
			return text;
		}
		if (inst->NumArgs() == 0 || depth >= 3u) {
			return text;
		}
		text += '(';
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			text += index == 0 ? "" : ", ";
			text += Describe(inst->Arg(index), depth + 1u);
		}
		return text + ')';
	}

	std::string DescribePhi(Value value) const {
		std::vector<Value>              leaves;
		std::vector<Value>              pending {value};
		std::unordered_set<const Inst*> seen;
		while (!pending.empty()) {
			const auto current = pending.back().Resolve();
			pending.pop_back();
			const auto* inst = current.TryInstruction();
			if (inst != nullptr && inst->GetOpcode() == ValueOpcode::Phi) {
				if (seen.insert(inst).second) {
					for (size_t index = 0; index < inst->NumArgs(); index++) {
						pending.push_back(inst->Arg(index));
					}
				}
				continue;
			}
			if (std::ranges::none_of(leaves, [&](Value known) {
				    return EquivalentValue(m_program, known, current);
			    })) {
				leaves.push_back(current);
			}
		}
		auto text = fmt::format("Phi merges {} unequal values:", leaves.size());
		for (size_t index = 0; index < leaves.size() && index < 6u; index++) {
			text += fmt::format(" [{}] {}", index, Describe(leaves[index]));
		}
		return text;
	}

	bool ValidateArguments(const Inst& inst, bool require_uniform) {
		for (size_t index = 0; index < inst.NumArgs(); index++) {
			if (!Validate(inst.Arg(index), require_uniform)) return false;
		}
		return true;
	}

	bool Validate(Value value, bool require_uniform = true) {
		value = value.Resolve();
		// Host floating-point evaluation does not model shader rounding/denormal modes.
		if (m_type == RuntimeValueType::Integer &&
		    TypesOverlap(value.GetType(), Type::F16 | Type::F32 | Type::F32x2)) {
			return false;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			if (!require_uniform) return true;
			switch (value.GetType()) {
				case Type::U1:
				case Type::U8:
				case Type::U16:
				case Type::U32:
				case Type::U64:
				case Type::F32: return true;
				default: return Reject("operand is not an integer");
			}
		}
		// Integer-only dependency checks do not depend on the active EXEC mask.
		if (!require_uniform && m_validated_dependencies.contains(inst)) return true;
		if (!m_visiting.insert(inst).second) {
			if (require_uniform) {
				Reject(fmt::format("{} is cyclic", ValueOpcodeName(inst->GetOpcode())));
			}
			return !require_uniform;
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			if (valid && !require_uniform) m_validated_dependencies.insert(inst);
			if (!valid && require_uniform) {
				Reject(fmt::format("{} cannot be evaluated before the draw",
				                   ValueOpcodeName(inst->GetOpcode())));
			}
			return valid;
		};
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::ReadConst) {
			const auto slot = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (inst->NumArgs() != 2 || inst->Arg(0).Resolve().TryInstruction() == nullptr ||
			    inst->Arg(0).Resolve().TryInstruction()->GetOpcode() !=
			        ValueOpcode::GetSrtResource ||
			    !slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) {
				return finish(false);
			}
			if (m_type == RuntimeValueType::Integer) {
				const auto active_mask = m_active_mask;
				m_active_mask          = {};
				const bool valid       = Validate(m_program.srt_reads[slot.U32()].value);
				m_active_mask          = active_mask;
				if (!valid) return finish(false);
			}
		}
		if (!require_uniform) return finish(ValidateArguments(*inst, false));
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(op) && inst->NumArgs() == 3 &&
		    inst->Arg(0).Resolve() == m_active_mask) {
			// Empty EXEC reads lane zero, so ignored operands still require integer types.
			if (m_type == RuntimeValueType::Integer && !Validate(inst->Arg(2), false)) {
				return finish(false);
			}
			return finish(Validate(inst->Arg(1)));
		}
		if (op == ValueOpcode::UndefU1 || op == ValueOpcode::UndefU8 ||
		    op == ValueOpcode::UndefU16 || op == ValueOpcode::UndefU32 ||
		    op == ValueOpcode::UndefU64 || op == ValueOpcode::Void) {
			return finish(false);
		}
		if (op == ValueOpcode::GetUserData) {
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::ScalarReg) {
				return finish(false);
			}
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg < m_program.user_data_base ||
			    reg - m_program.user_data_base >= m_program.user_data_count) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::GetShaderBase) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::Phi) {
			if (m_type == RuntimeValueType::Integer && !ValidateArguments(*inst, false)) {
				return finish(false);
			}
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (invariant.IsEmpty()) {
				Reject(DescribePhi(value));
				return finish(false);
			}
			return finish(Validate(invariant));
		}
		if (op == ValueOpcode::ReadFirstLane) {
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U1) {
				return finish(false);
			}
			if (m_type == RuntimeValueType::Integer && !Validate(inst->Arg(1), false)) {
				return finish(false);
			}
			const auto active_mask = m_active_mask;
			m_active_mask          = inst->Arg(1).Resolve();
			const bool valid       = Validate(inst->Arg(0));
			m_active_mask          = active_mask;
			return finish(valid);
		}
		if (op == ValueOpcode::GetSrtResource) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
			const auto  expected = op == ValueOpcode::LoadAddressU32
			                           ? ValueOpcode::GetAddressResource
			                           : ValueOpcode::GetBufferResource;
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsRawRead(m_program, *inst) || handle == nullptr ||
			    handle->GetOpcode() != expected) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU64) {
			const auto index = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU32x2) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto  index  = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 2u ||
			    (source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 &&
			     source->GetOpcode() != ValueOpcode::IAddCarry32)) {
				return finish(false);
			}
		}
		if (IsDescriptorHandle(op)) {
			size_t expected = 4u;
			if (op == ValueOpcode::GetImageResource) {
				expected = 8u;
			} else if (op == ValueOpcode::GetAddressResource) {
				expected = 2u;
			}
			if (inst->NumArgs() != expected) {
				return finish(false);
			}
		} else if (op != ValueOpcode::ReadConst && op != ValueOpcode::ReadConstBuffer &&
		           op != ValueOpcode::LoadAddressU32 && !IsRuntimeUniformOp(op)) {
			return finish(false);
		}
		return finish(ValidateArguments(*inst, true));
	}

	const ResourcePlan&             m_program;
	RuntimeValueType                m_type;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
	std::string                     m_reason;
};

class PlanBuilder {
public:
	explicit PlanBuilder(Program& program): m_program(program) {}

	void Run() {
		m_program.srt_reads.clear();
		m_program.dynamic_reads.clear();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				const auto op = inst.GetOpcode();
				if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
					const auto flags = inst.Flags<MemoryFlags>();
					if (flags.index < m_program.memory_info.size()) {
						const auto kind       = m_program.memory_info[flags.index].kind;
						const bool crosswired = (op == ValueOpcode::LoadAddressU32 &&
						                         kind == ResourceKind::ScalarBuffer) ||
						                        (op == ValueOpcode::ReadConstBuffer &&
						                         kind == ResourceKind::ScalarAddress);
						if (crosswired) {
							Fail(flags.pc,
							     fmt::format("{} has incompatible scalar memory metadata",
							                 ValueOpcodeName(op)));
						}
					}
				}
				if (IsDescriptorHandle(inst.GetOpcode())) {
					for (size_t index = 0; index < inst.NumArgs(); index++) {
						Collect(inst.Arg(index), 0);
					}
				}
			}
		}
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 && IsRawRead(m_program, inst) &&
				    inst.Arg(1).Resolve().IsImmediate() &&
				    ValidateRuntimeValue(m_program, Value(&inst))) {
					Collect(Value(&inst), inst.Flags<MemoryFlags>().pc);
				}
			}
		}
		PatchReads();
	}

private:
	struct Patch {
		Inst*    inst = nullptr;
		uint32_t slot = 0;
		bool     keep = false;
	};

	[[noreturn]] void Fail(uint32_t pc, const std::string& message) const {
		const auto diagnostic = Diagnostic(m_program, pc, message);
		EXIT("shader SRT planning failed: %s", diagnostic.c_str());
		std::abort();
	}

	void Collect(Value value, uint32_t use_pc) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return;
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			Fail(use_pc, "invalid typed planning value");
		}
		const auto cycle = std::ranges::find(m_visiting, inst);
		if (cycle != m_visiting.end()) {
			const auto contains_phi = std::any_of(cycle, m_visiting.end(), [](const Inst* value) {
				return value->GetOpcode() == ValueOpcode::Phi;
			});
			if (contains_phi) {
				return;
			}
			Fail(use_pc, fmt::format("cyclic typed planning value {} without a phi",
			                         ValueOpcodeName(inst->GetOpcode())));
		}
		if (std::ranges::find(m_visited, inst) != m_visited.end()) {
			return;
		}
		m_visiting.push_back(inst);
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			Collect(inst->Arg(index), use_pc);
		}
		m_visiting.pop_back();
		m_visited.push_back(inst);
		if (!IsRawRead(m_program, *inst)) {
			return;
		}
		const auto offset = inst->Arg(1).Resolve();
		if (!offset.IsImmediate() || offset.GetType() != Type::U32) {
			if (std::ranges::find(m_program.dynamic_reads, value) ==
			    m_program.dynamic_reads.end()) {
				m_program.dynamic_reads.push_back(value);
			}
			return;
		}
		for (uint32_t slot = 0; slot < m_program.srt_reads.size(); slot++) {
			if (EquivalentValue(m_program, value, m_program.srt_reads[slot].value)) {
				m_patches.push_back({inst, slot, false});
				return;
			}
		}
		const auto slot = static_cast<uint32_t>(m_program.srt_reads.size());
		m_program.srt_reads.push_back({value, slot});
		m_patches.push_back({inst, slot, true});
	}

	void PatchReads() {
		for (const auto& patch: m_patches) {
			auto* block = patch.inst->Parent();
			auto& list  = block->Instructions();
			auto  where =
			    std::ranges::find_if(list, [&](const Inst& inst) { return &inst == patch.inst; });
			const auto resource =
			    Value(&*block->PrependNewInst(where, ValueOpcode::GetSrtResource));
			const auto flat = Value(&*block->PrependNewInst(where, ValueOpcode::ReadConst,
			                                                {resource, Value(patch.slot)}));
			const auto uses = patch.inst->Uses();
			for (const auto& use: uses) {
				use.user->SetArg(use.operand, flat);
			}
			for (auto& info: m_program.block_info) {
				if (info.condition.Resolve() == Value(patch.inst)) {
					info.condition = flat;
				}
				if (info.indirect_target.Resolve() == Value(patch.inst)) {
					info.indirect_target = flat;
				}
			}
			if (patch.keep) {
				const auto memory = patch.inst->Flags<MemoryFlags>().index;
				if (memory < m_program.memory_info.size()) {
					m_program.memory_info[memory].planning_only = true;
				}
				block->AppendNewInst(ValueOpcode::ReferenceU32, {Value(patch.inst)});
			}
		}
	}

	Program&           m_program;
	std::vector<Inst*> m_visiting;
	std::vector<Inst*> m_visited;
	std::vector<Patch> m_patches;
};

// Per-thread memo storage for runtime evaluation. Instructions of an extracted plan carry dense
// indices, so results live in a flat array stamped with a per-evaluator epoch: no hashing, no
// per-node allocation and no clearing between draws. Epochs are 64-bit and never wrap, so a stamp
// left by an earlier evaluator can never be mistaken for a live one.
class ValueMemo {
public:
	// stamp == epoch marks a memoized result; stamp == Busy(epoch) marks an evaluation in progress,
	// which is how cycles are detected. Epochs stay below the busy bit, so the two never collide.
	struct Slot {
		uint64_t value = 0;
		uint64_t stamp = 0;
	};

	static constexpr uint64_t BusyBit = uint64_t {1} << 63u;
	static constexpr uint64_t Busy(uint64_t epoch) { return epoch | BusyBit; }

	static ValueMemo& Thread() {
		static thread_local ValueMemo memo;
		return memo;
	}

	// Grows to the plan and hands out a private epoch. Nested evaluators of the same plan share the
	// storage; distinct epochs keep their memoized values apart.
	uint64_t Open(uint32_t count) {
		if (m_slots.size() < count) {
			m_slots.resize(count);
		}
		EXIT_IF(m_epoch >= BusyBit);
		return ++m_epoch;
	}

	Slot& At(uint32_t index) { return m_slots[index]; }

private:
	std::vector<Slot> m_slots;
	uint64_t          m_epoch = 0;
};

// Result buffers reused across draws. A successful evaluation swaps them into the caller's
// vectors, which hands the caller's previous buffers back here for the next call, so the steady
// state allocates nothing. Failure leaves the destinations untouched, preserving the transaction.
struct SourceScratch {
	std::vector<DescriptorValue> evaluated;
	std::vector<uint32_t>        flattened;
	std::vector<uint8_t>         active;
	// Control-flow walk state for the flat evaluator; the IR walker keeps its own on its arena.
	std::vector<uint8_t>         visited;
	std::vector<uint32_t>        pending;
	bool                         in_use = false;
};

class Evaluator {
public:
	Evaluator(const ResourcePlan& program, const SrtRuntime& runtime,
	          std::pmr::memory_resource* memory, std::span<const uint8_t> clean_flat_slots = {}, Evaluator* clean_evaluator = nullptr,
	          Value active_mask = {})
	    : m_program(program), m_runtime(runtime), m_clean_flat_slots(clean_flat_slots),
	      m_clean_evaluator(clean_evaluator), m_active_mask(active_mask.Resolve()),
	      m_memo(ValueMemo::Thread()), m_epoch(m_memo.Open(program.value_count)),
	      m_indexed_count(program.value_count), m_cache(memory), m_visiting(memory) {}

	bool Evaluate(Value value, uint32_t& result) {
		uint64_t wide = 0;
		if (!EvaluateWide(value, wide)) {
			return false;
		}
		result = static_cast<uint32_t>(wide);
		return true;
	}

private:
	static float Float32(uint64_t bits) {
		return std::bit_cast<float>(static_cast<uint32_t>(bits));
	}

	static uint64_t Float32Bits(float value) { return std::bit_cast<uint32_t>(value); }

	bool EvaluateWide(Value value, uint64_t& result) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			switch (value.GetType()) {
				case Type::U1: result = value.U1(); return true;
				case Type::U8: result = value.U8(); return true;
				case Type::U16: result = value.U16(); return true;
				case Type::U32: result = value.U32(); return true;
				case Type::U64: result = value.U64(); return true;
				case Type::F32: result = Float32Bits(value.F32Value()); return true;
				default: return false;
			}
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(inst->GetOpcode()) &&
		    inst->NumArgs() == 3 && inst->Arg(0).Resolve() == m_active_mask) {
			return EvaluateWide(inst->Arg(1), result);
		}
		const auto index = inst->PlanIndex();
		if (index < m_indexed_count) {
			return EvaluateIndexed(*inst, index, result);
		}
		return EvaluateMapped(*inst, result);
	}

	// Extracted plans number every instruction, so memoization and cycle detection are both direct
	// array accesses.
	bool EvaluateIndexed(const Inst& inst, uint32_t index, uint64_t& result) {
		{
			const auto& slot = m_memo.At(index);
			if (slot.stamp == m_epoch) {
				result = slot.value;
				return true;
			}
			if (slot.stamp == ValueMemo::Busy(m_epoch)) {
				return false;
			}
		}
		m_memo.At(index).stamp = ValueMemo::Busy(m_epoch);
		uint64_t out = 0;
		const bool evaluated = EvaluateInst(inst, out);
		// A nested evaluator may have grown the shared storage, so re-address the slot instead of
		// holding a reference across the recursive call.
		auto& slot = m_memo.At(index);
		if (!evaluated) {
			slot.stamp = 0;
			return false;
		}
		slot.stamp = m_epoch;
		slot.value = out;
		result = out;
		return true;
	}

	// Programs that were never extracted into a plan carry no dense indices; only unit fixtures
	// evaluate those, so they keep the original pointer-keyed memo.
	bool EvaluateMapped(const Inst& inst, uint64_t& result) {
		const auto* key = &inst;
		if (const auto found = m_cache.find(key); found != m_cache.end()) {
			result = found->second;
			return true;
		}
		if (std::ranges::find(m_visiting, key) != m_visiting.end()) {
			return false;
		}
		m_visiting.push_back(key);
		uint64_t out = 0;
		const bool evaluated = EvaluateInst(inst, out);
		m_visiting.pop_back();
		if (!evaluated) {
			return false;
		}
		m_cache.emplace(key, out);
		result = out;
		return true;
	}

	bool Arg(const Inst& inst, size_t index, uint64_t& result) {
		return EvaluateWide(inst.Arg(index), result);
	}

	bool EvaluatePhi(const Inst& inst, uint64_t& result) {
		const auto value = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
		return !value.IsEmpty() && EvaluateWide(value, result);
	}

	bool EvaluateExtract(const Inst& inst, uint64_t& result) {
		const auto index = inst.Arg(1).Resolve();
		if (!index.IsImmediate() || index.GetType() != Type::U32) {
			return false;
		}
		const auto component = index.U32();
		if (component >= 2u) {
			return false;
		}
		if (inst.GetOpcode() == ValueOpcode::CompositeExtractU64) {
			uint64_t packed = 0;
			if (!Arg(inst, 0, packed)) {
				return false;
			}
			result = static_cast<uint32_t>(packed >> (component * 32u));
			return true;
		}
		const auto* source = inst.Arg(0).ResolveInstruction();
		if (source == nullptr) {
			return false;
		}
		if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
			return EvaluateWide(source->Arg(component), result);
		}
		if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
			uint64_t lhs = 0;
			uint64_t rhs = 0;
			if (!Arg(*source, 0, lhs) || !Arg(*source, 1, rhs)) {
				return false;
			}
			const auto sum =
			    static_cast<uint64_t>(static_cast<uint32_t>(lhs)) + static_cast<uint32_t>(rhs);
			result =
			    component == 0u ? static_cast<uint32_t>(sum) : static_cast<uint32_t>(sum >> 32u);
			return true;
		}
		return false;
	}

	bool EvaluateRawRead(const Inst& inst, uint64_t& result) {
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) {
			return false;
		}
		const auto& mem    = m_program.memory_info[flags.index];
		const auto* handle = inst.Arg(0).ResolveInstruction();
		if (handle == nullptr) {
			return false;
		}
		uint64_t low    = 0;
		uint64_t high   = 0;
		uint64_t offset = 0;
		if (!Arg(*handle, 0, low) || !Arg(*handle, 1, high) || !Arg(inst, 1, offset)) {
			return false;
		}
		const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
		const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
		uint64_t   address   = 0;
		if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
			uint64_t records = 0;
			uint64_t word3   = 0;
			if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
				return false;
			}
			if (immediate < 0) {
				return false;
			}
			const auto byte_offset =
			    static_cast<uint64_t>(immediate) + static_cast<uint32_t>(offset);
			const auto aligned = byte_offset & ~uint64_t {3};
			const auto stride  = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
			const auto size = stride == 0u
			                      ? static_cast<uint64_t>(static_cast<uint32_t>(records))
			                      : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
			if (aligned > size || size - aligned < sizeof(uint32_t)) {
				return false;
			}
			address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
		} else {
			const auto relative = (immediate & ~int64_t {3}) +
			                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
			if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
				return false;
			}
		}
		uint32_t word = 0;
		if (m_runtime.read_memory != nullptr) {
			if (!m_runtime.read_memory(m_runtime.userdata, address, &word)) {
				return false;
			}
		} else {
			std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
		}
		result = word;
		return true;
	}

	bool EvaluateInst(const Inst& inst, uint64_t& result) {
		uint64_t   a       = 0;
		uint64_t   b       = 0;
		uint64_t   c       = 0;
		const auto binary  = [&]() { return Arg(inst, 0, a) && Arg(inst, 1, b); };
		const auto ternary = [&]() {
			return Arg(inst, 0, a) && Arg(inst, 1, b) && Arg(inst, 2, c);
		};
		switch (inst.GetOpcode()) {
			case ValueOpcode::GetUserData: {
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base ||
				    reg - m_program.user_data_base >= m_runtime.user_data.size()) {
					return false;
				}
				result = m_runtime.user_data[reg - m_program.user_data_base];
				return true;
			}
			case ValueOpcode::GetShaderBase: result = m_runtime.shader_base; return true;
			case ValueOpcode::Phi: return EvaluatePhi(inst, result);
			case ValueOpcode::ReadFirstLane: {
				Evaluator active(m_program, m_runtime, m_cache.get_allocator().resource(),
				                 m_clean_flat_slots, m_clean_evaluator, inst.Arg(1));
				return active.EvaluateWide(inst.Arg(0), result);
			}
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32: return Arg(inst, 0, result);
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: return EvaluateExtract(inst, result);
			case ValueOpcode::CompositeConstructU64:
				if (!binary()) {
					return false;
				}
				result = static_cast<uint32_t>(a) |
				         (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
				return true;
			case ValueOpcode::ReadConst: {
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size()) {
					return false;
				}
				if (slot.U32() < m_clean_flat_slots.size() &&
				    m_clean_flat_slots[slot.U32()] != 0u && m_clean_evaluator != nullptr) {
					return m_clean_evaluator->EvaluateWide(m_program.srt_reads[slot.U32()].value,
					                                       result);
				}
				return EvaluateWide(m_program.srt_reads[slot.U32()].value, result);
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
				if (IsRawRead(m_program, inst)) {
					return EvaluateRawRead(inst, result);
				}
				break;
			case ValueOpcode::IAdd32:
				if (binary()) {
					result = static_cast<uint32_t>(a + b);
					return true;
				}
				return false;
			case ValueOpcode::IAdd64:
				if (binary()) {
					result = a + b;
					return true;
				}
				return false;
			case ValueOpcode::ISub32:
				if (binary()) {
					result = static_cast<uint32_t>(a - b);
					return true;
				}
				return false;
			case ValueOpcode::ISub64:
				if (binary()) {
					result = a - b;
					return true;
				}
				return false;
			case ValueOpcode::IMul32:
				if (binary()) {
					result = static_cast<uint32_t>(a * b);
					return true;
				}
				return false;
			case ValueOpcode::IMul64:
				if (binary()) {
					result = a * b;
					return true;
				}
				return false;
			case ValueOpcode::UMin32:
				if (binary()) {
					result = std::min(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
					return true;
				}
				return false;
			case ValueOpcode::ConvertF32U32:
				if (Arg(inst, 0, a)) {
					result = Float32Bits(static_cast<float>(static_cast<uint32_t>(a)));
					return true;
				}
				return false;
			case ValueOpcode::ConvertU32F32:
				if (Arg(inst, 0, a)) {
					const auto value = Float32(a);
					if (!std::isfinite(value) || value < 0.0f ||
					    static_cast<double>(value) > UINT32_MAX) {
						return false;
					}
					result = static_cast<uint32_t>(value);
					return true;
				}
				return false;
			case ValueOpcode::FPMul32:
				if (binary()) {
					result = Float32Bits(Float32(a) * Float32(b));
					return true;
				}
				return false;
			case ValueOpcode::FPTrunc32:
				if (Arg(inst, 0, a)) {
					result = Float32Bits(std::trunc(Float32(a)));
					return true;
				}
				return false;
			case ValueOpcode::FPIsNan32:
				if (Arg(inst, 0, a)) {
					result = std::isnan(Float32(a));
					return true;
				}
				return false;
			case ValueOpcode::FPOrdLessThanEqual32:
				if (binary()) {
					result = Float32(a) <= Float32(b);
					return true;
				}
				return false;
			case ValueOpcode::FPOrdGreaterThanEqual32:
				if (binary()) {
					result = Float32(a) >= Float32(b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseAnd32:
				if (binary()) {
					result = static_cast<uint32_t>(a & b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseAnd64:
				if (binary()) {
					result = a & b;
					return true;
				}
				return false;
			case ValueOpcode::BitwiseOr32:
				if (binary()) {
					result = static_cast<uint32_t>(a | b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseXor32:
				if (binary()) {
					result = static_cast<uint32_t>(a ^ b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseNot32:
				if (Arg(inst, 0, a)) {
					result = ~static_cast<uint32_t>(a);
					return true;
				}
				return false;
			case ValueOpcode::BitCount32:
				if (Arg(inst, 0, a)) {
					result = static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(a)));
					return true;
				}
				return false;
			case ValueOpcode::FindILsb32:
				if (Arg(inst, 0, a)) {
					const auto bits = static_cast<uint32_t>(a);
					result          = bits == 0u ? UINT32_MAX
					                             : static_cast<uint32_t>(std::countr_zero(bits));
					return true;
				}
				return false;
			case ValueOpcode::FindUMsb32:
				if (Arg(inst, 0, a)) {
					const auto bits = static_cast<uint32_t>(a);
					result          = bits == 0u ? UINT32_MAX
					                             : static_cast<uint32_t>(31 - std::countl_zero(bits));
					return true;
				}
				return false;
			case ValueOpcode::ShiftLeftLogical32:
				if (binary()) {
					result = static_cast<uint32_t>(a) << (b & 31u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftLeftLogical64:
				if (binary()) {
					result = a << (b & 63u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightLogical32:
				if (binary()) {
					result = static_cast<uint32_t>(a) >> (b & 31u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightLogical64:
				if (binary()) {
					result = a >> (b & 63u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightArithmetic32:
				if (binary()) {
					result = static_cast<uint32_t>(
					    std::bit_cast<int32_t>(static_cast<uint32_t>(a)) >> (b & 31u));
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightArithmetic64:
				if (binary()) {
					result = static_cast<uint64_t>(std::bit_cast<int64_t>(a) >> (b & 63u));
					return true;
				}
				return false;
			case ValueOpcode::BitFieldUExtract:
				if (ternary()) {
					const auto offset = static_cast<uint32_t>(b);
					const auto width  = static_cast<uint32_t>(c);
					if (offset > 32u || width > 32u - offset) {
						return false;
					}
					const auto mask = width == 32u  ? UINT32_MAX
					                  : width == 0u ? 0u
					                                : (uint32_t {1} << width) - 1u;
					result = width == 0u ? 0u : (static_cast<uint32_t>(a) >> offset) & mask;
					return true;
				}
				return false;
			case ValueOpcode::BitFieldSExtract:
				if (ternary()) {
					const auto offset = static_cast<uint32_t>(b);
					const auto width  = static_cast<uint32_t>(c);
					if (offset > 32u || width > 32u - offset) {
						return false;
					}
					if (width == 0u) {
						result = 0;
						return true;
					}
					const auto mask = width == 32u ? UINT32_MAX : (uint32_t {1} << width) - 1u;
					auto       bits = (static_cast<uint32_t>(a) >> offset) & mask;
					if (width < 32u && (bits & (uint32_t {1} << (width - 1u))) != 0u) {
						bits |= ~mask;
					}
					result = bits;
					return true;
				}
				return false;
			case ValueOpcode::BitFieldInsert: {
				uint64_t d = 0;
				if (!ternary() || !Arg(inst, 3, d)) {
					return false;
				}
				const auto offset = static_cast<uint32_t>(c);
				const auto width  = static_cast<uint32_t>(d);
				if (offset > 32u || width > 32u - offset) {
					return false;
				}
				if (width == 0u) {
					result = static_cast<uint32_t>(a);
					return true;
				}
				const auto mask =
				    width == 32u ? UINT32_MAX : ((uint32_t {1} << width) - 1u) << offset;
				result = (static_cast<uint32_t>(a) & ~mask) |
				         ((static_cast<uint32_t>(b) << offset) & mask);
				return true;
			}
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectF32:
				if (ternary()) {
					result = a != 0u ? b : c;
					return true;
				}
				return false;
			case ValueOpcode::IEqual32:
				if (binary()) {
					result = static_cast<uint32_t>(a) == static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::INotEqual32:
				if (binary()) {
					result = static_cast<uint32_t>(a) != static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::ULessThan32:
				if (binary()) {
					result = static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::UGreaterThan32:
				if (binary()) {
					result = static_cast<uint32_t>(a) > static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::LogicalAnd:
				if (binary()) {
					result = (a != 0u) && (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalOr:
				if (binary()) {
					result = (a != 0u) || (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalXor:
				if (binary()) {
					result = (a != 0u) != (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalNot:
				if (Arg(inst, 0, a)) {
					result = a == 0u;
					return true;
				}
				return false;
			case ValueOpcode::UndefU1:
			case ValueOpcode::UndefU8:
			case ValueOpcode::UndefU16:
			case ValueOpcode::UndefU32:
			case ValueOpcode::UndefU64: return false;
			default: break;
		}
		return false;
	}

	const ResourcePlan&                       m_program;
	const SrtRuntime&                         m_runtime;
	std::span<const uint8_t>                  m_clean_flat_slots;
	Evaluator*                                m_clean_evaluator = nullptr;
	Value                                     m_active_mask;
	ValueMemo&                                m_memo;
	uint64_t                                  m_epoch = 0;
	uint32_t                                  m_indexed_count = 0;
	std::pmr::unordered_map<const Inst*, uint64_t> m_cache;
	std::pmr::vector<const Inst*>                  m_visiting;
};

const DescriptorSource* Source(const ResourcePlan& program, uint32_t source) {
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
}

// Flat program -------------------------------------------------------------------------------
//
// The walk above is exact but pays per draw for work that depends only on the plan: resolving
// identities, probing the memo, fetching operands out of std::vector and recursing. Extracted
// plans are immutable, so FlatCompiler performs that walk once, at extraction, and records it as
// a straight-line program that FlatMachine replays per draw. The lowering mirrors Evaluator step
// for step, including the context a value is evaluated under (clean reader or not, active EXEC
// mask), so values and failures agree with the walker. Anything the walker rejects structurally
// (unsupported opcode, cycle, malformed operand) becomes a root that fails before running.

constexpr uint32_t FlatFailed     = UINT32_MAX;
constexpr uint32_t FlatUnassigned = UINT32_MAX - 1u;

class FlatCompiler {
public:
	explicit FlatCompiler(ResourcePlan& plan): m_plan(plan) {}

	void Run() {
		auto& out       = m_plan.flat;
		out             = {};
		out.value_count = m_plan.value_count;
		m_contexts.clear();
		m_state.clear();
		const auto normal = Context(false, {});
		const auto clean  = Context(true, {});
		EXIT_IF(normal != NormalContext || clean != CleanContext);

		out.sources.resize(m_plan.descriptor_sources.size());
		for (size_t index = 0; index < m_plan.descriptor_sources.size(); index++) {
			const auto& source = m_plan.descriptor_sources[index];
			auto&       root   = out.sources[index];
			bool        ok     = source.dword_count <= root.results.size();
			for (uint32_t dword = 0; ok && dword < source.dword_count; dword++) {
				root.results[dword] = Flatten(source.dwords[dword], NormalContext);
				ok                  = root.results[dword] != FlatFailed;
			}
			if (ok) {
				Schedule(root, std::span<const uint32_t>(root.results).first(source.dword_count));
			}
		}
		out.flat_reads.resize(m_plan.srt_reads.size());
		out.clean_flat_reads.resize(m_plan.srt_reads.size());
		for (size_t index = 0; index < m_plan.srt_reads.size(); index++) {
			const auto& read = m_plan.srt_reads[index];
			ValueRoot(out.flat_reads[index], read.value, NormalContext);
			if (IsCleanSlot(read.flat_offset)) {
				ValueRoot(out.clean_flat_reads[index], read.value, CleanContext);
			}
		}
		out.conditions.resize(m_plan.control_flow.size());
		for (size_t index = 0; index < m_plan.control_flow.size(); index++) {
			const auto& condition = m_plan.control_flow[index].condition;
			if (!condition.IsEmpty()) {
				ValueRoot(out.conditions[index], condition, CleanContext);
			}
		}
		const auto& fill = m_plan.uniform_fill;
		for (uint32_t index = 0; index < fill.fill.words && index < out.uniform_values.size();
		     index++) {
			ValueRoot(out.uniform_values[index], fill.values[index], CleanContext);
		}
		out.compiled = m_supported;
	}

private:
	static constexpr uint32_t NormalContext = 0;
	static constexpr uint32_t CleanContext  = 1;

	// Evaluation context. The clean context reads memory through the specialization reader and
	// never routes ReadConst through the clean table; a non-empty mask is the active EXEC mask
	// inside a ReadFirstLane, which short-circuits selects on that mask.
	struct FlatContext {
		bool  clean = false;
		Value mask;
	};

	struct Slot {
		uint32_t reg      = FlatUnassigned;
		bool     visiting = false;
	};

	bool IsCleanSlot(uint32_t slot) const {
		return slot < m_plan.clean_flat_slots.size() && m_plan.clean_flat_slots[slot] != 0u;
	}

	uint32_t Context(bool clean, Value mask) {
		for (uint32_t index = 0; index < m_contexts.size(); index++) {
			if (m_contexts[index].clean == clean && m_contexts[index].mask == mask) {
				return index;
			}
		}
		m_contexts.push_back({clean, mask});
		m_state.emplace_back(m_plan.value_count);
		return static_cast<uint32_t>(m_contexts.size() - 1u);
	}

	uint32_t Emit(SrtFlatOp op, std::initializer_list<uint32_t> args, uint64_t imm = 0,
	              bool clean = false) {
		SrtFlatInst inst;
		inst.op    = op;
		inst.imm   = imm;
		inst.clean = clean ? 1u : 0u;
		for (const auto arg: args) {
			if (arg == FlatFailed) {
				return FlatFailed;
			}
			inst.args[inst.arg_count++] = arg;
		}
		m_plan.flat.insts.push_back(inst);
		return static_cast<uint32_t>(m_plan.flat.insts.size() - 1u);
	}

	uint32_t Imm(uint64_t bits) {
		if (const auto found = m_immediates.find(bits); found != m_immediates.end()) {
			return found->second;
		}
		const auto reg = Emit(SrtFlatOp::Imm, {}, bits);
		m_immediates.emplace(bits, reg);
		return reg;
	}

	void ValueRoot(SrtFlatValueRoot& root, Value value, uint32_t context) {
		root.result = Flatten(value, context);
		if (root.result != FlatFailed) {
			Schedule(root, std::span<const uint32_t>(&root.result, 1));
		}
	}

	// Lists the closure of the results in dependency order. Each root carries its whole closure,
	// so callers may evaluate any subset of roots in any order.
	void Schedule(SrtFlatRoot& root, std::span<const uint32_t> results) {
		auto& out = m_plan.flat;
		m_marks.resize(out.insts.size(), 0u);
		m_mark_epoch++;
		root.first = static_cast<uint32_t>(out.schedule.size());
		for (const auto result: results) {
			Visit(result);
		}
		root.count = static_cast<uint32_t>(out.schedule.size()) - root.first;
		root.valid = true;
	}

	void Visit(uint32_t index) {
		if (m_marks[index] == m_mark_epoch) {
			return;
		}
		m_marks[index]   = m_mark_epoch;
		const auto& inst = m_plan.flat.insts[index];
		for (uint32_t arg = 0; arg < inst.arg_count; arg++) {
			Visit(inst.args[arg]);
		}
		m_plan.flat.schedule.push_back(index);
	}

	uint32_t Arg(const Inst& inst, size_t index, uint32_t context) {
		return index < inst.NumArgs() ? Flatten(inst.Arg(index), context) : FlatFailed;
	}

	uint32_t Flatten(Value value, uint32_t context) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			switch (value.GetType()) {
				case Type::U1: return Imm(value.U1() ? 1u : 0u);
				case Type::U8: return Imm(value.U8());
				case Type::U16: return Imm(value.U16());
				case Type::U32: return Imm(value.U32());
				case Type::U64: return Imm(value.U64());
				case Type::F32: return Imm(std::bit_cast<uint32_t>(value.F32Value()));
				default: return FlatFailed;
			}
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return FlatFailed;
		}
		const auto op = inst->GetOpcode();
		{
			// Copied: nested contexts may grow m_contexts during the recursion below.
			const auto mask = m_contexts[context].mask;
			if (!mask.IsEmpty() && IsRuntimeSelect(op) && inst->NumArgs() == 3 &&
			    inst->Arg(0).Resolve() == mask) {
				return Flatten(inst->Arg(1), context);
			}
		}
		const auto index = inst->PlanIndex();
		if (index >= m_plan.value_count) {
			// Only extracted plans number their instructions; anything else keeps the walker.
			m_supported = false;
			return FlatFailed;
		}
		{
			const auto& slot = m_state[context][index];
			if (slot.reg != FlatUnassigned) {
				return slot.reg;
			}
			if (slot.visiting) {
				// A cycle, which the walker detects through its busy stamp.
				return FlatFailed;
			}
		}
		m_state[context][index].visiting = true;
		const auto reg = FlattenInst(*inst, context);
		auto& slot     = m_state[context][index];
		slot.visiting  = false;
		slot.reg       = reg;
		return reg;
	}

	uint32_t FlattenExtract(const Inst& inst, uint32_t context) {
		if (inst.NumArgs() != 2) {
			return FlatFailed;
		}
		const auto index = inst.Arg(1).Resolve();
		if (!index.IsImmediate() || index.GetType() != Type::U32) {
			return FlatFailed;
		}
		const auto component = index.U32();
		if (component >= 2u) {
			return FlatFailed;
		}
		if (inst.GetOpcode() == ValueOpcode::CompositeExtractU64) {
			return Emit(SrtFlatOp::ExtractU64, {Arg(inst, 0, context)}, component);
		}
		const auto* source = inst.Arg(0).Resolve().TryInstruction();
		if (source == nullptr) {
			return FlatFailed;
		}
		if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
			return Arg(*source, component, context);
		}
		if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
			const auto lhs = Arg(*source, 0, context);
			if (lhs == FlatFailed) {
				return FlatFailed;
			}
			return Emit(SrtFlatOp::AddCarry, {lhs, Arg(*source, 1, context)}, component);
		}
		return FlatFailed;
	}

	uint32_t FlattenRawRead(const Inst& inst, uint32_t context) {
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_plan.memory_info.size() || inst.NumArgs() < 2) {
			return FlatFailed;
		}
		const auto& mem    = m_plan.memory_info[flags.index];
		const auto* handle = inst.Arg(0).Resolve().TryInstruction();
		if (handle == nullptr || handle->NumArgs() < 2) {
			return FlatFailed;
		}
		const auto low = Arg(*handle, 0, context);
		if (low == FlatFailed) {
			return FlatFailed;
		}
		const auto high = Arg(*handle, 1, context);
		if (high == FlatFailed) {
			return FlatFailed;
		}
		const auto offset = Arg(inst, 1, context);
		if (offset == FlatFailed) {
			return FlatFailed;
		}
		const bool clean     = m_contexts[context].clean;
		const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
		if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
			if (handle->NumArgs() != 4u) {
				return FlatFailed;
			}
			const auto records = Arg(*handle, 2, context);
			if (records == FlatFailed) {
				return FlatFailed;
			}
			// Word 3 is evaluated for its failure only; it is listed so its ops are scheduled.
			const auto word3 = Arg(*handle, 3, context);
			if (word3 == FlatFailed || immediate < 0) {
				return FlatFailed;
			}
			return Emit(SrtFlatOp::ReadBuffer, {low, high, records, offset, word3},
			            static_cast<uint64_t>(immediate), clean);
		}
		return Emit(SrtFlatOp::ReadAddress, {low, high, offset},
		            std::bit_cast<uint64_t>(immediate), clean);
	}

	uint32_t FlattenInst(const Inst& inst, uint32_t context) {
		const auto unary = [&](SrtFlatOp op) { return Emit(op, {Arg(inst, 0, context)}); };
		const auto binary = [&](SrtFlatOp op) {
			const auto a = Arg(inst, 0, context);
			if (a == FlatFailed) {
				return FlatFailed;
			}
			return Emit(op, {a, Arg(inst, 1, context)});
		};
		const auto ternary = [&](SrtFlatOp op) {
			const auto a = Arg(inst, 0, context);
			if (a == FlatFailed) {
				return FlatFailed;
			}
			const auto b = Arg(inst, 1, context);
			if (b == FlatFailed) {
				return FlatFailed;
			}
			return Emit(op, {a, b, Arg(inst, 2, context)});
		};
		switch (inst.GetOpcode()) {
			case ValueOpcode::GetUserData: {
				if (inst.NumArgs() != 1 || inst.Arg(0).GetType() != Type::ScalarReg) {
					return FlatFailed;
				}
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_plan.user_data_base) {
					return FlatFailed;
				}
				return Emit(SrtFlatOp::UserData, {}, reg - m_plan.user_data_base);
			}
			case ValueOpcode::GetShaderBase: return Emit(SrtFlatOp::ShaderBase, {});
			case ValueOpcode::Phi: {
				const auto invariant =
				    ResolveInvariantPhi(m_plan, Value(const_cast<Inst*>(&inst)));
				return invariant.IsEmpty() ? FlatFailed : Flatten(invariant, context);
			}
			case ValueOpcode::ReadFirstLane: {
				if (inst.NumArgs() != 2) {
					return FlatFailed;
				}
				const auto nested = Context(m_contexts[context].clean, inst.Arg(1).Resolve());
				return Flatten(inst.Arg(0), nested);
			}
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32: return Arg(inst, 0, context);
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: return FlattenExtract(inst, context);
			case ValueOpcode::CompositeConstructU64: return binary(SrtFlatOp::ConstructU64);
			case ValueOpcode::ReadConst: {
				if (inst.NumArgs() != 2) {
					return FlatFailed;
				}
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_plan.srt_reads.size()) {
					return FlatFailed;
				}
				const bool routed = !m_contexts[context].clean && IsCleanSlot(slot.U32());
				return Flatten(m_plan.srt_reads[slot.U32()].value,
				               routed ? CleanContext : context);
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
				return IsRawRead(m_plan, inst) ? FlattenRawRead(inst, context) : FlatFailed;
			case ValueOpcode::IAdd32: return binary(SrtFlatOp::IAdd32);
			case ValueOpcode::IAdd64: return binary(SrtFlatOp::IAdd64);
			case ValueOpcode::ISub32: return binary(SrtFlatOp::ISub32);
			case ValueOpcode::ISub64: return binary(SrtFlatOp::ISub64);
			case ValueOpcode::IMul32: return binary(SrtFlatOp::IMul32);
			case ValueOpcode::IMul64: return binary(SrtFlatOp::IMul64);
			case ValueOpcode::UMin32: return binary(SrtFlatOp::UMin32);
			case ValueOpcode::ConvertF32U32: return unary(SrtFlatOp::ConvertF32U32);
			case ValueOpcode::ConvertU32F32: return unary(SrtFlatOp::ConvertU32F32);
			case ValueOpcode::FPMul32: return binary(SrtFlatOp::FPMul32);
			case ValueOpcode::FPTrunc32: return unary(SrtFlatOp::FPTrunc32);
			case ValueOpcode::FPIsNan32: return unary(SrtFlatOp::FPIsNan32);
			case ValueOpcode::FPOrdLessThanEqual32: return binary(SrtFlatOp::FPOrdLessThanEqual32);
			case ValueOpcode::FPOrdGreaterThanEqual32:
				return binary(SrtFlatOp::FPOrdGreaterThanEqual32);
			case ValueOpcode::BitwiseAnd32: return binary(SrtFlatOp::BitwiseAnd32);
			case ValueOpcode::BitwiseAnd64: return binary(SrtFlatOp::BitwiseAnd64);
			case ValueOpcode::BitwiseOr32: return binary(SrtFlatOp::BitwiseOr32);
			case ValueOpcode::BitwiseXor32: return binary(SrtFlatOp::BitwiseXor32);
			case ValueOpcode::BitwiseNot32: return unary(SrtFlatOp::BitwiseNot32);
			case ValueOpcode::BitCount32: return unary(SrtFlatOp::BitCount32);
			case ValueOpcode::FindILsb32: return unary(SrtFlatOp::FindILsb32);
			case ValueOpcode::FindUMsb32: return unary(SrtFlatOp::FindUMsb32);
			case ValueOpcode::ShiftLeftLogical32: return binary(SrtFlatOp::ShiftLeftLogical32);
			case ValueOpcode::ShiftLeftLogical64: return binary(SrtFlatOp::ShiftLeftLogical64);
			case ValueOpcode::ShiftRightLogical32: return binary(SrtFlatOp::ShiftRightLogical32);
			case ValueOpcode::ShiftRightLogical64: return binary(SrtFlatOp::ShiftRightLogical64);
			case ValueOpcode::ShiftRightArithmetic32:
				return binary(SrtFlatOp::ShiftRightArithmetic32);
			case ValueOpcode::ShiftRightArithmetic64:
				return binary(SrtFlatOp::ShiftRightArithmetic64);
			case ValueOpcode::BitFieldUExtract: return ternary(SrtFlatOp::BitFieldUExtract);
			case ValueOpcode::BitFieldSExtract: return ternary(SrtFlatOp::BitFieldSExtract);
			case ValueOpcode::BitFieldInsert: {
				const auto a = Arg(inst, 0, context);
				if (a == FlatFailed) {
					return FlatFailed;
				}
				const auto b = Arg(inst, 1, context);
				if (b == FlatFailed) {
					return FlatFailed;
				}
				const auto c = Arg(inst, 2, context);
				if (c == FlatFailed) {
					return FlatFailed;
				}
				return Emit(SrtFlatOp::BitFieldInsert, {a, b, c, Arg(inst, 3, context)});
			}
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectF32: return ternary(SrtFlatOp::Select);
			case ValueOpcode::IEqual32: return binary(SrtFlatOp::IEqual32);
			case ValueOpcode::INotEqual32: return binary(SrtFlatOp::INotEqual32);
			case ValueOpcode::ULessThan32: return binary(SrtFlatOp::ULessThan32);
			case ValueOpcode::UGreaterThan32: return binary(SrtFlatOp::UGreaterThan32);
			case ValueOpcode::LogicalAnd: return binary(SrtFlatOp::LogicalAnd);
			case ValueOpcode::LogicalOr: return binary(SrtFlatOp::LogicalOr);
			case ValueOpcode::LogicalXor: return binary(SrtFlatOp::LogicalXor);
			case ValueOpcode::LogicalNot: return unary(SrtFlatOp::LogicalNot);
			default: return FlatFailed;
		}
	}

	ResourcePlan&                          m_plan;
	std::vector<FlatContext>               m_contexts;
	std::vector<std::vector<Slot>>         m_state;
	std::unordered_map<uint64_t, uint32_t> m_immediates;
	std::vector<uint32_t>                  m_marks;
	uint32_t                               m_mark_epoch = 0;
	bool                                   m_supported  = true;
};

// Per-thread register file for FlatMachine. Registers are stamped with the epoch of the run that
// wrote them, so nothing is cleared between draws and the file only grows to the largest plan.
struct FlatRegisters {
	std::vector<uint64_t> values;
	std::vector<uint64_t> stamps;
	uint64_t              epoch  = 0;
	bool                  in_use = false;

	static FlatRegisters& Thread() {
		static thread_local FlatRegisters registers;
		return registers;
	}
};

class FlatMachine {
public:
	FlatMachine(const SrtFlatProgram& program, const SrtRuntime& runtime, FlatRegisters& registers)
	    : m_program(program), m_runtime(runtime) {
		const auto count = program.insts.size();
		if (registers.values.size() < count) {
			registers.values.resize(count);
			registers.stamps.resize(count);
		}
		EXIT_IF(registers.epoch == UINT64_MAX);
		m_epoch  = ++registers.epoch;
		m_values = registers.values.data();
		m_stamps = registers.stamps.data();
	}

	[[nodiscard]] uint32_t Result(uint32_t reg) const { return static_cast<uint32_t>(m_values[reg]); }

	// Computes every register the root needs. Registers already produced in this run are skipped,
	// so shared work between roots is done once; a failure leaves earlier registers valid.
	bool Run(const SrtFlatRoot& root) {
		if (!root.valid) {
			RecordFailure(root, UINT32_MAX);
			return false;
		}
		const auto* schedule = m_program.schedule.data() + root.first;
		for (uint32_t step = 0; step < root.count; step++) {
			const auto index = schedule[step];
			if (m_stamps[index] == m_epoch) {
				continue;
			}
			uint64_t result = 0;
			m_address_valid = false;
			if (!Execute(m_program.insts[index], result)) {
				RecordFailure(root, index);
				return false;
			}
			m_values[index] = result;
			m_stamps[index] = m_epoch;
		}
		return true;
	}

private:
	static float Float32(uint64_t bits) {
		return std::bit_cast<float>(static_cast<uint32_t>(bits));
	}

	static uint64_t Float32Bits(float value) { return std::bit_cast<uint32_t>(value); }

	bool Read(const SrtFlatInst& inst, uint64_t address, uint64_t& result) const {
		// Kept for the failure record; overwritten on every read and never looked at on success.
		m_address       = address;
		m_address_valid = true;
		uint32_t word   = 0;
		if (inst.clean != 0u) {
			if (m_runtime.read_specialization_memory == nullptr ||
			    !m_runtime.read_specialization_memory(m_runtime.userdata, address, &word)) {
				return false;
			}
		} else if (m_runtime.read_memory != nullptr) {
			if (!m_runtime.read_memory(m_runtime.userdata, address, &word)) {
				return false;
			}
		} else {
			std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
		}
		result = word;
		return true;
	}

	bool Execute(const SrtFlatInst& inst, uint64_t& result) const {
		const auto arg = [&](size_t index) { return m_values[inst.args[index]]; };
		switch (inst.op) {
			case SrtFlatOp::Imm: result = inst.imm; return true;
			case SrtFlatOp::UserData:
				if (inst.imm >= m_runtime.user_data.size()) {
					return false;
				}
				result = m_runtime.user_data[inst.imm];
				return true;
			case SrtFlatOp::ShaderBase: result = m_runtime.shader_base; return true;
			case SrtFlatOp::ReadAddress: {
				const auto low       = arg(0);
				const auto high      = arg(1);
				const auto offset    = arg(2);
				const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
				const auto immediate = static_cast<int64_t>(inst.imm);
				const auto relative  = (immediate & ~int64_t {3}) +
				                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
				uint64_t address = 0;
				if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
					return false;
				}
				return Read(inst, address, result);
			}
			case SrtFlatOp::ReadBuffer: {
				const auto low         = arg(0);
				const auto high        = arg(1);
				const auto records     = arg(2);
				const auto offset      = arg(3);
				const auto base        = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
				const auto byte_offset = inst.imm + static_cast<uint32_t>(offset);
				const auto aligned     = byte_offset & ~uint64_t {3};
				const auto stride      = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
				const auto size        = stride == 0u
				                             ? static_cast<uint64_t>(static_cast<uint32_t>(records))
				                             : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
				if (aligned > size || size - aligned < sizeof(uint32_t)) {
					return false;
				}
				const auto address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
				return Read(inst, address, result);
			}
			case SrtFlatOp::ExtractU64:
				result = static_cast<uint32_t>(arg(0) >> (inst.imm * 32u));
				return true;
			case SrtFlatOp::AddCarry: {
				const auto sum = static_cast<uint64_t>(static_cast<uint32_t>(arg(0))) +
				                 static_cast<uint32_t>(arg(1));
				result = inst.imm == 0u ? static_cast<uint32_t>(sum)
				                        : static_cast<uint32_t>(sum >> 32u);
				return true;
			}
			case SrtFlatOp::ConstructU64:
				result = static_cast<uint32_t>(arg(0)) |
				         (static_cast<uint64_t>(static_cast<uint32_t>(arg(1))) << 32u);
				return true;
			case SrtFlatOp::IAdd32: result = static_cast<uint32_t>(arg(0) + arg(1)); return true;
			case SrtFlatOp::IAdd64: result = arg(0) + arg(1); return true;
			case SrtFlatOp::ISub32: result = static_cast<uint32_t>(arg(0) - arg(1)); return true;
			case SrtFlatOp::ISub64: result = arg(0) - arg(1); return true;
			case SrtFlatOp::IMul32: result = static_cast<uint32_t>(arg(0) * arg(1)); return true;
			case SrtFlatOp::IMul64: result = arg(0) * arg(1); return true;
			case SrtFlatOp::UMin32:
				result = std::min(static_cast<uint32_t>(arg(0)), static_cast<uint32_t>(arg(1)));
				return true;
			case SrtFlatOp::ConvertF32U32:
				result = Float32Bits(static_cast<float>(static_cast<uint32_t>(arg(0))));
				return true;
			case SrtFlatOp::ConvertU32F32: {
				const auto value = Float32(arg(0));
				if (!std::isfinite(value) || value < 0.0f ||
				    static_cast<double>(value) > UINT32_MAX) {
					return false;
				}
				result = static_cast<uint32_t>(value);
				return true;
			}
			case SrtFlatOp::FPMul32:
				result = Float32Bits(Float32(arg(0)) * Float32(arg(1)));
				return true;
			case SrtFlatOp::FPTrunc32: result = Float32Bits(std::trunc(Float32(arg(0)))); return true;
			case SrtFlatOp::FPIsNan32: result = std::isnan(Float32(arg(0))) ? 1u : 0u; return true;
			case SrtFlatOp::FPOrdLessThanEqual32:
				result = Float32(arg(0)) <= Float32(arg(1)) ? 1u : 0u;
				return true;
			case SrtFlatOp::FPOrdGreaterThanEqual32:
				result = Float32(arg(0)) >= Float32(arg(1)) ? 1u : 0u;
				return true;
			case SrtFlatOp::BitwiseAnd32: result = static_cast<uint32_t>(arg(0) & arg(1)); return true;
			case SrtFlatOp::BitwiseAnd64: result = arg(0) & arg(1); return true;
			case SrtFlatOp::BitwiseOr32: result = static_cast<uint32_t>(arg(0) | arg(1)); return true;
			case SrtFlatOp::BitwiseXor32: result = static_cast<uint32_t>(arg(0) ^ arg(1)); return true;
			case SrtFlatOp::BitwiseNot32: result = ~static_cast<uint32_t>(arg(0)); return true;
			case SrtFlatOp::BitCount32:
				result = static_cast<uint32_t>(std::popcount(static_cast<uint32_t>(arg(0))));
				return true;
			case SrtFlatOp::FindILsb32: {
				const auto bits = static_cast<uint32_t>(arg(0));
				result = bits == 0u ? UINT32_MAX : static_cast<uint32_t>(std::countr_zero(bits));
				return true;
			}
			case SrtFlatOp::FindUMsb32: {
				const auto bits = static_cast<uint32_t>(arg(0));
				result          = bits == 0u ? UINT32_MAX
				                             : static_cast<uint32_t>(31 - std::countl_zero(bits));
				return true;
			}
			case SrtFlatOp::ShiftLeftLogical32:
				result = static_cast<uint32_t>(arg(0)) << (arg(1) & 31u);
				return true;
			case SrtFlatOp::ShiftLeftLogical64: result = arg(0) << (arg(1) & 63u); return true;
			case SrtFlatOp::ShiftRightLogical32:
				result = static_cast<uint32_t>(arg(0)) >> (arg(1) & 31u);
				return true;
			case SrtFlatOp::ShiftRightLogical64: result = arg(0) >> (arg(1) & 63u); return true;
			case SrtFlatOp::ShiftRightArithmetic32:
				result = static_cast<uint32_t>(
				    std::bit_cast<int32_t>(static_cast<uint32_t>(arg(0))) >> (arg(1) & 31u));
				return true;
			case SrtFlatOp::ShiftRightArithmetic64:
				result = static_cast<uint64_t>(std::bit_cast<int64_t>(arg(0)) >> (arg(1) & 63u));
				return true;
			case SrtFlatOp::BitFieldUExtract: {
				const auto offset = static_cast<uint32_t>(arg(1));
				const auto width  = static_cast<uint32_t>(arg(2));
				if (offset > 32u || width > 32u - offset) {
					return false;
				}
				const auto mask = width == 32u  ? UINT32_MAX
				                  : width == 0u ? 0u
				                                : (uint32_t {1} << width) - 1u;
				result = width == 0u ? 0u : (static_cast<uint32_t>(arg(0)) >> offset) & mask;
				return true;
			}
			case SrtFlatOp::BitFieldSExtract: {
				const auto offset = static_cast<uint32_t>(arg(1));
				const auto width  = static_cast<uint32_t>(arg(2));
				if (offset > 32u || width > 32u - offset) {
					return false;
				}
				if (width == 0u) {
					result = 0;
					return true;
				}
				const auto mask = width == 32u ? UINT32_MAX : (uint32_t {1} << width) - 1u;
				auto       bits = (static_cast<uint32_t>(arg(0)) >> offset) & mask;
				if (width < 32u && (bits & (uint32_t {1} << (width - 1u))) != 0u) {
					bits |= ~mask;
				}
				result = bits;
				return true;
			}
			case SrtFlatOp::BitFieldInsert: {
				const auto offset = static_cast<uint32_t>(arg(2));
				const auto width  = static_cast<uint32_t>(arg(3));
				if (offset > 32u || width > 32u - offset) {
					return false;
				}
				if (width == 0u) {
					result = static_cast<uint32_t>(arg(0));
					return true;
				}
				const auto mask =
				    width == 32u ? UINT32_MAX : ((uint32_t {1} << width) - 1u) << offset;
				result = (static_cast<uint32_t>(arg(0)) & ~mask) |
				         ((static_cast<uint32_t>(arg(1)) << offset) & mask);
				return true;
			}
			case SrtFlatOp::Select: result = arg(0) != 0u ? arg(1) : arg(2); return true;
			case SrtFlatOp::IEqual32:
				result = static_cast<uint32_t>(arg(0)) == static_cast<uint32_t>(arg(1)) ? 1u : 0u;
				return true;
			case SrtFlatOp::INotEqual32:
				result = static_cast<uint32_t>(arg(0)) != static_cast<uint32_t>(arg(1)) ? 1u : 0u;
				return true;
			case SrtFlatOp::ULessThan32:
				result = static_cast<uint32_t>(arg(0)) < static_cast<uint32_t>(arg(1)) ? 1u : 0u;
				return true;
			case SrtFlatOp::UGreaterThan32:
				result = static_cast<uint32_t>(arg(0)) > static_cast<uint32_t>(arg(1)) ? 1u : 0u;
				return true;
			case SrtFlatOp::LogicalAnd: result = (arg(0) != 0u) && (arg(1) != 0u) ? 1u : 0u; return true;
			case SrtFlatOp::LogicalOr: result = (arg(0) != 0u) || (arg(1) != 0u) ? 1u : 0u; return true;
			case SrtFlatOp::LogicalXor: result = (arg(0) != 0u) != (arg(1) != 0u) ? 1u : 0u; return true;
			case SrtFlatOp::LogicalNot: result = arg(0) == 0u ? 1u : 0u; return true;
		}
		return false;
	}

	// Address a memory op used, recomputed from the operand values the run produced.
	static bool MemoryAddress(const SrtFlatInst& inst, const SrtTraceEntry& entry,
	                          uint64_t& address) {
		if (inst.op == SrtFlatOp::ReadBuffer && inst.arg_count >= 4) {
			const auto base = ((entry.arg_values[1] << 32u) |
			                   static_cast<uint32_t>(entry.arg_values[0])) &
			                  AddressMask;
			const auto byte_offset = inst.imm + static_cast<uint32_t>(entry.arg_values[3]);
			address                = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
			return true;
		}
		if (inst.op == SrtFlatOp::ReadAddress && inst.arg_count >= 3) {
			const auto base = ((entry.arg_values[1] << 32u) |
			                   static_cast<uint32_t>(entry.arg_values[0])) &
			                  AddressMask;
			const auto immediate = static_cast<int64_t>(inst.imm);
			const auto relative  = (immediate & ~int64_t {3}) +
			                      static_cast<int64_t>(static_cast<uint32_t>(entry.arg_values[2]) &
			                                           ~3u);
			address = (base & ~uint64_t {3}) + static_cast<uint64_t>(relative);
			return true;
		}
		return false;
	}

	// The failing root's schedule, so the chain that produced a zero descriptor is visible.
	void RecordTrace(SrtFailure& failure, const SrtFlatRoot& root, uint32_t failing) const {
		if (!root.valid) {
			return;
		}
		constexpr uint32_t MaxTrace = 128;
		const auto*        schedule = m_program.schedule.data() + root.first;
		for (uint32_t step = 0; step < root.count && failure.trace.size() < MaxTrace; step++) {
			const auto  index = schedule[step];
			const auto& inst  = m_program.insts[index];
			SrtTraceEntry entry;
			entry.inst_index = index;
			entry.op_name    = SrtFlatOpName(inst.op);
			entry.arg_count  = inst.arg_count;
			entry.imm        = inst.imm;
			entry.clean      = inst.clean != 0u;
			entry.failing    = index == failing;
			for (uint32_t arg = 0; arg < inst.arg_count && arg < SrtFlatInst::MaxArgs; arg++) {
				const auto reg         = inst.args[arg];
				const bool defined     = reg < m_program.insts.size() && m_stamps[reg] == m_epoch;
				entry.args[arg]        = reg;
				entry.arg_defined[arg] = defined ? 1u : 0u;
				entry.arg_values[arg]  = defined ? m_values[reg] : 0u;
			}
			entry.defined = m_stamps[index] == m_epoch;
			entry.value   = entry.defined ? m_values[index] : 0u;
			entry.memory  = inst.op == SrtFlatOp::ReadBuffer || inst.op == SrtFlatOp::ReadAddress;
			if (entry.memory) {
				entry.address_valid = MemoryAddress(inst, entry, entry.address);
			}
			failure.trace.push_back(entry);
		}
	}

	// Fills the thread's failure record. Runs once, after a root has already failed.
	void RecordFailure(const SrtFlatRoot& root, uint32_t index) const {
		auto& failure = ThreadFailure();
		ResetFailure(failure);
		failure.valid          = true;
		failure.flat           = true;
		failure.root_first     = root.first;
		failure.root_count     = root.count;
		failure.root_valid     = root.valid;
		failure.user_data_size = static_cast<uint32_t>(m_runtime.user_data.size());
		if (root.valid) {
			const auto* schedule = m_program.schedule.data() + root.first;
			for (uint32_t step = 0; step < root.count; step++) {
				const auto& scheduled = m_program.insts[schedule[step]];
				if (scheduled.op != SrtFlatOp::UserData) {
					continue;
				}
				const auto reg = static_cast<uint32_t>(scheduled.imm);
				failure.user_data.emplace_back(
				    reg, reg < m_runtime.user_data.size() ? m_runtime.user_data[reg] : 0u);
			}
		}
		RecordTrace(failure, root, index);
		if (index >= m_program.insts.size()) {
			return;
		}
		const auto& inst   = m_program.insts[index];
		failure.inst_index = index;
		failure.op_name    = SrtFlatOpName(inst.op);
		failure.arg_count  = inst.arg_count;
		failure.imm        = inst.imm;
		failure.clean      = inst.clean != 0u;
		for (uint32_t arg = 0; arg < inst.arg_count && arg < SrtFlatInst::MaxArgs; arg++) {
			const auto reg           = inst.args[arg];
			const bool defined       = reg < m_program.insts.size() && m_stamps[reg] == m_epoch;
			failure.args[arg]        = reg;
			failure.arg_defined[arg] = defined ? 1u : 0u;
			failure.arg_values[arg]  = defined ? m_values[reg] : 0u;
		}
		failure.address_valid = m_address_valid;
		failure.address       = m_address;
		if (inst.op == SrtFlatOp::ReadBuffer && inst.arg_count >= 4) {
			const auto low      = failure.arg_values[0];
			const auto high     = failure.arg_values[1];
			const auto records  = failure.arg_values[2];
			const auto offset   = failure.arg_values[3];
			failure.memory_op   = true;
			failure.base        = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
			failure.byte_offset = inst.imm + static_cast<uint32_t>(offset);
			failure.stride      = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
			failure.records     = static_cast<uint32_t>(records);
			failure.bound =
			    failure.stride == 0u ? failure.records : failure.stride * failure.records;
			if (!failure.address_valid) {
				failure.address =
				    ((failure.base & ~uint64_t {3}) + failure.byte_offset) & ~uint64_t {3};
				failure.address_valid = true;
			}
		} else if (inst.op == SrtFlatOp::ReadAddress && inst.arg_count >= 3) {
			const auto low       = failure.arg_values[0];
			const auto high      = failure.arg_values[1];
			const auto offset    = failure.arg_values[2];
			failure.memory_op    = true;
			failure.base         = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
			const auto immediate = static_cast<int64_t>(inst.imm);
			const auto relative  = (immediate & ~int64_t {3}) +
			                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
			failure.byte_offset = static_cast<uint64_t>(relative);
			if (!failure.address_valid) {
				failure.address       = (failure.base & ~uint64_t {3}) + failure.byte_offset;
				failure.address_valid = true;
			}
		}
	}

	const SrtFlatProgram& m_program;
	const SrtRuntime&     m_runtime;
	uint64_t*             m_values = nullptr;
	uint64_t*             m_stamps = nullptr;
	uint64_t              m_epoch  = 0;
	// Address of the most recent read attempt, cleared before every instruction.
	mutable uint64_t m_address       = 0;
	mutable bool     m_address_valid = false;
};

// The compiled normal context routes ReadConst through the plan's own clean table, so a caller
// asking for a different clean set must take the walker.
bool SameCleanSet(std::span<const uint8_t> requested, std::span<const uint8_t> compiled) {
	const auto count = std::max(requested.size(), compiled.size());
	for (size_t index = 0; index < count; index++) {
		const bool wanted = index < requested.size() && requested[index] != 0u;
		const bool have   = index < compiled.size() && compiled[index] != 0u;
		if (wanted != have) {
			return false;
		}
	}
	return true;
}

bool FlatPlanUsable(const ResourcePlan& program, std::span<const uint8_t> clean_flat_slots) {
	const auto& flat = program.flat;
	return flat.compiled && flat.value_count == program.value_count &&
	       flat.sources.size() == program.descriptor_sources.size() &&
	       flat.flat_reads.size() == program.srt_reads.size() &&
	       flat.conditions.size() == program.control_flow.size() &&
	       SameCleanSet(clean_flat_slots, program.clean_flat_slots);
}

bool FlatUniformValuesUsable(const ResourcePlan& program, std::span<const Value> values) {
	const auto& flat = program.flat;
	const auto& fill = program.uniform_fill;
	if (!flat.compiled || flat.value_count != program.value_count ||
	    values.size() > fill.fill.words || values.size() > flat.uniform_values.size()) {
		return false;
	}
	for (size_t index = 0; index < values.size(); index++) {
		if (!(values[index] == fill.values[index])) {
			return false;
		}
	}
	return true;
}

struct FlatRegistersLease {
	FlatRegisters  local;
	FlatRegisters& registers;

	FlatRegistersLease(): registers(FlatRegisters::Thread().in_use ? local : FlatRegisters::Thread()) {
		registers.in_use = true;
	}
	~FlatRegistersLease() { registers.in_use = false; }
	FlatRegistersLease(const FlatRegistersLease&)            = delete;
	FlatRegistersLease& operator=(const FlatRegistersLease&) = delete;
};

bool EvaluateWithWalker(const ResourcePlan& program, std::span<const uint32_t> sources,
                        const SrtRuntime& runtime, bool evaluate_flat,
                        std::span<const uint8_t> clean_flat_slots,
                        std::span<const uint8_t> skip_sources, SourceScratch& scratch) {
	SrtRuntime clean_runtime  = runtime;
	clean_runtime.read_memory = runtime.read_specialization_memory;
	std::array<std::byte, 4096> evaluator_storage;
	std::pmr::monotonic_buffer_resource evaluator_memory(
	    evaluator_storage.data(), evaluator_storage.size(), std::pmr::new_delete_resource());
	Evaluator clean_evaluator(program, clean_runtime, &evaluator_memory);
	Evaluator evaluator(program, runtime, &evaluator_memory, clean_flat_slots, &clean_evaluator);

	auto& active = scratch.active;
	active.clear();
	if (evaluate_flat) {
		active.assign(program.descriptor_sources.size(), 1u);
	}
	if (evaluate_flat && !program.control_flow.empty()) {
		for (const auto& block: program.control_flow) {
			for (const auto source: block.sources) {
				active.at(source) = 0u;
			}
		}
		std::pmr::vector<uint8_t>  visited(program.control_flow.size(), &evaluator_memory);
		std::pmr::vector<uint32_t> pending(&evaluator_memory);
		pending.push_back(0);
		while (!pending.empty()) {
			const auto index = pending.back();
			pending.pop_back();
			if (visited.at(index)) {
				continue;
			}
			visited[index]    = 1u;
			const auto& block = program.control_flow[index];
			for (const auto source: block.sources) {
				active[source] = 1u;
			}
			uint32_t condition = 0;
			// A missing clean reader must never fall through to the evaluator's raw-memory path.
			if (!block.condition.IsEmpty() && runtime.read_specialization_memory != nullptr &&
			    clean_evaluator.Evaluate(block.condition, condition)) {
				pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
			} else {
				pending.insert(pending.end(), block.successors.begin(), block.successors.end());
			}
		}
	}
	auto& evaluated = scratch.evaluated;
	evaluated.clear();
	evaluated.reserve(sources.size());
	for (const auto source_index: sources) {
		const auto* source = Source(program, source_index);
		if (source == nullptr) {
			return false;
		}
		DescriptorValue value;
		value.dword_count = source->dword_count;
		// A source the shader evaluates itself (gpu_fetch) is left zeroed.
		const bool skipped =
		    source_index < skip_sources.size() && skip_sources[source_index] != 0u;
		if (!skipped && (!evaluate_flat || active[source_index])) {
			for (uint32_t index = 0; index < source->dword_count; index++) {
				if (!evaluator.Evaluate(source->dwords[index], value.dwords[index])) {
					TagFailure("source", source_index, false);
					return false;
				}
			}
		}
		evaluated.push_back(value);
	}
	auto& flattened = scratch.flattened;
	flattened.clear();
	if (evaluate_flat) {
		flattened.resize(program.srt_reads.size());
		for (const auto& read: program.srt_reads) {
			const bool clean    = read.flat_offset < clean_flat_slots.size() &&
			                      clean_flat_slots[read.flat_offset] != 0u;
			auto&      selected = clean ? clean_evaluator : evaluator;
			if (read.flat_offset >= flattened.size() ||
			    !selected.Evaluate(read.value, flattened[read.flat_offset])) {
				TagFailure("flat-read", read.flat_offset, false);
				return false;
			}
		}
	}
	return true;
}

// Same contract as EvaluateWithWalker over the compiled program: same activity walk, same
// per-source and per-slot evaluation order, same transactional failure.
bool EvaluateWithFlatProgram(const ResourcePlan& program, std::span<const uint32_t> sources,
                             const SrtRuntime& runtime, bool evaluate_flat,
                             std::span<const uint8_t> clean_flat_slots,
                             std::span<const uint8_t> skip_sources, SourceScratch& scratch) {
	const auto&        flat = program.flat;
	FlatRegistersLease lease;
	FlatMachine        machine(flat, runtime, lease.registers);

	auto& active = scratch.active;
	active.clear();
	if (evaluate_flat) {
		active.assign(program.descriptor_sources.size(), 1u);
	}
	if (evaluate_flat && !program.control_flow.empty()) {
		for (const auto& block: program.control_flow) {
			for (const auto source: block.sources) {
				active.at(source) = 0u;
			}
		}
		auto& visited = scratch.visited;
		auto& pending = scratch.pending;
		visited.assign(program.control_flow.size(), 0u);
		pending.clear();
		pending.push_back(0);
		while (!pending.empty()) {
			const auto index = pending.back();
			pending.pop_back();
			if (visited.at(index)) {
				continue;
			}
			visited[index]    = 1u;
			const auto& block = program.control_flow[index];
			for (const auto source: block.sources) {
				active[source] = 1u;
			}
			const auto& condition = flat.conditions[index];
			if (!block.condition.IsEmpty() && runtime.read_specialization_memory != nullptr &&
			    machine.Run(condition)) { // a failed condition is not fatal: both successors run
				const auto taken = machine.Result(condition.result) != 0u;
				pending.push_back(block.successors[taken ? 0u : 1u]);
			} else {
				pending.insert(pending.end(), block.successors.begin(), block.successors.end());
			}
		}
	}
	auto& evaluated = scratch.evaluated;
	evaluated.clear();
	evaluated.reserve(sources.size());
	for (const auto source_index: sources) {
		const auto* source = Source(program, source_index);
		if (source == nullptr) {
			return false;
		}
		DescriptorValue value;
		value.dword_count = source->dword_count;
		// A source the shader evaluates itself (gpu_fetch) is left zeroed.
		const bool skipped =
		    source_index < skip_sources.size() && skip_sources[source_index] != 0u;
		if (!skipped && (!evaluate_flat || active[source_index])) {
			const auto& root = flat.sources[source_index];
			if (!machine.Run(root)) {
				TagFailure("source", source_index, true);
				return false;
			}
			for (uint32_t index = 0; index < source->dword_count; index++) {
				value.dwords[index] = machine.Result(root.results[index]);
			}
		}
		evaluated.push_back(value);
	}
	auto& flattened = scratch.flattened;
	flattened.clear();
	if (evaluate_flat) {
		flattened.resize(program.srt_reads.size());
		for (size_t index = 0; index < program.srt_reads.size(); index++) {
			const auto& read  = program.srt_reads[index];
			const bool  clean = read.flat_offset < clean_flat_slots.size() &&
			                   clean_flat_slots[read.flat_offset] != 0u;
			const auto& root = clean ? flat.clean_flat_reads[index] : flat.flat_reads[index];
			if (read.flat_offset >= flattened.size() || !machine.Run(root)) {
				TagFailure("flat-read", static_cast<uint32_t>(index), true);
				return false;
			}
			flattened[read.flat_offset] = machine.Result(root.result);
		}
	}
	return true;
}

bool EvaluateRuntimeSourcesImpl(const ResourcePlan& program, std::span<const uint32_t> sources,
                                const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                                std::vector<uint32_t>& flat, bool evaluate_flat,
                                std::span<const uint8_t> clean_flat_slots,
                                std::vector<uint8_t>& active_sources,
                                std::span<const uint8_t> skip_sources) {
#if defined(TRACY_ENABLE)
	KYTY_PROFILER_BLOCK("EvaluateRuntimeSourcesImpl");
#endif
	if (!program.srt_plan_complete) {
		TagFailure("plan-incomplete", UINT32_MAX, false);
		return false;
	}
	if (runtime.read_specialization_memory == nullptr &&
	    std::ranges::any_of(clean_flat_slots, [](uint8_t clean) { return clean != 0u; })) {
		TagFailure("no-clean-reader", UINT32_MAX, false);
		return false;
	}
	// Nested use cannot happen today (materialization calls this sequentially), but fall back to
	// local buffers rather than aliasing the scratch if that ever changes.
	static thread_local SourceScratch thread_scratch;
	SourceScratch                     local_scratch;
	const bool     reuse   = !thread_scratch.in_use;
	SourceScratch& scratch = reuse ? thread_scratch : local_scratch;
	scratch.in_use         = true;
	const struct ScratchGuard {
		SourceScratch& owned;
		~ScratchGuard() { owned.in_use = false; }
	} guard {scratch};

	const bool evaluated =
	    FlatPlanUsable(program, clean_flat_slots)
	        ? EvaluateWithFlatProgram(program, sources, runtime, evaluate_flat, clean_flat_slots,
                                  skip_sources, scratch)
	        : EvaluateWithWalker(program, sources, runtime, evaluate_flat, clean_flat_slots,
                             skip_sources, scratch);
	if (!evaluated) {
		return false;
	}
	// Swapping rather than moving keeps the caller's old buffers alive in the scratch for reuse.
	results.swap(scratch.evaluated);
	active_sources.swap(scratch.active);
	if (evaluate_flat) {
		flat.swap(scratch.flattened);
	}
	return true;
}

} // namespace

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type,
                          std::string* reason) {
	RuntimeValidator validator(program, type);
	if (validator.Run(value)) {
		return true;
	}
	if (reason != nullptr) {
		*reason = validator.Reason();
	}
	return false;
}

const SrtFailure& LastSrtFailure() {
	return ThreadFailure();
}

std::string FormatSrtFailure(const SrtFailure& failure) {
	if (!failure.valid) {
		return "no recorded SRT failure";
	}
	std::string text = fmt::format("slot={}[{}] evaluator={} root=[{}..{}) root_valid={}",
	                               failure.kind, failure.index, failure.flat ? "flat" : "walker",
	                               failure.root_first, failure.root_first + failure.root_count,
	                               failure.root_valid ? 1 : 0);
	if (failure.inst_index != UINT32_MAX) {
		text += fmt::format(" inst=#{} op={} imm={:#x} clean={}", failure.inst_index,
		                    failure.op_name, failure.imm, failure.clean ? 1 : 0);
		for (uint32_t arg = 0; arg < failure.arg_count && arg < failure.args.size(); arg++) {
			text += fmt::format(" arg{}=r{}{}", arg, failure.args[arg],
			                    failure.arg_defined[arg] != 0u
			                        ? fmt::format("({:#x})", failure.arg_values[arg])
			                        : std::string("(undef)"));
		}
	}
	if (failure.memory_op) {
		text += fmt::format(" base={:#x} byte_offset={:#x} stride={} records={} bound={:#x}",
		                    failure.base, failure.byte_offset, failure.stride, failure.records,
		                    failure.bound);
	}
	if (failure.address_valid) {
		text += fmt::format(" address={:#x}", failure.address);
	}
	text += fmt::format(" user_data_size={}", failure.user_data_size);
	if (!failure.trace.empty()) {
		text += fmt::format(" trace({})", failure.trace.size());
		for (const auto& entry: failure.trace) {
			text += fmt::format("\n    {}#{} {} imm={:#x} clean={}", entry.failing ? "*" : " ",
			                    entry.inst_index, entry.op_name, entry.imm, entry.clean ? 1 : 0);
			for (uint32_t arg = 0; arg < entry.arg_count && arg < entry.args.size(); arg++) {
				text += fmt::format(" a{}=r{}{}", arg, entry.args[arg],
				                    entry.arg_defined[arg] != 0u
				                        ? fmt::format("({:#x})", entry.arg_values[arg])
				                        : std::string("(undef)"));
			}
			if (entry.address_valid) {
				text += fmt::format(" @{:#x}", entry.address);
			}
			text += entry.defined ? fmt::format(" -> {:#x}", entry.value) : std::string(" -> undef");
		}
	}
	if (!failure.user_data.empty()) {
		text += " user_data=";
		bool first = true;
		for (const auto& [reg, value]: failure.user_data) {
			text += fmt::format("{}s{}={:#x}", first ? "" : ",", reg, value);
			first = false;
		}
	}
	return text;
}

void BuildSrtPlan(Program& program) {
	if (program.resource_tracking_complete) {
		EXIT("shader SRT planning failed: cannot rebuild SRT after resource tracking");
	}
	program.srt_plan_complete = false;
	PlanBuilder(program).Run();
	program.srt_plan_complete = true;
}

void CompileSrtPlan(ResourcePlan& program) {
	FlatCompiler(program).Run();
}

bool EvaluateUniformValues(const ResourcePlan& program, std::span<const Value> values,
                            const SrtRuntime& runtime, std::span<uint32_t> results) {
#if defined(TRACY_ENABLE)
	KYTY_PROFILER_BLOCK("EvaluateUniformValues");
#endif
	if (values.size() != results.size()) {
		return false;
	}
	auto clean = runtime;
	clean.read_memory = runtime.read_specialization_memory != nullptr
	                        ? runtime.read_specialization_memory
	                        : +[](void*, uint64_t, uint32_t*) { return false; };
	if (FlatUniformValuesUsable(program, values)) {
		FlatRegistersLease lease;
		FlatMachine        machine(program.flat, clean, lease.registers);
		for (size_t i = 0; i < values.size(); ++i) {
			const auto& root = program.flat.uniform_values[i];
			if (!machine.Run(root)) {
				return false;
			}
			results[i] = machine.Result(root.result);
		}
		return true;
	}
	std::array<std::byte, 4096> evaluator_storage;
	std::pmr::monotonic_buffer_resource evaluator_memory(
	    evaluator_storage.data(), evaluator_storage.size(), std::pmr::new_delete_resource());
	Evaluator evaluator(program, clean, &evaluator_memory);
	for (size_t i = 0; i < values.size(); ++i) {
		if (!evaluator.Evaluate(values[i], results[i])) {
			return false;
		}
	}
	return true;
}

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result) {
	std::vector<DescriptorValue> results;
	if (!EvaluateDescriptorSources(program, std::span {&source, 1}, runtime, results)) {
		return false;
	}
	result = results.front();
	return true;
}

bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results) {
	std::vector<uint32_t> ignored;
	std::vector<uint8_t>  active;
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, ignored, false, {},
	                                  active, {});
}

bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::span<const uint8_t> clean_flat_slots,
                            std::vector<uint8_t>& active_sources,
                            std::span<const uint8_t> skip_sources) {
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, flat, true,
	                                  clean_flat_slots, active_sources, skip_sources);
}

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime, std::vector<uint32_t>& flat) {
	std::vector<DescriptorValue> ignored;
	std::vector<uint8_t>         active;
	return EvaluateRuntimeSources(program, {}, runtime, ignored, flat, {}, active);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
