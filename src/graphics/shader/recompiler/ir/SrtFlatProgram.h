#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTFLATPROGRAM_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTFLATPROGRAM_H_

#include <array>
#include <cstdint>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Straight-line form of a ResourcePlan's runtime values.
//
// CompileSrtPlan lowers every value the evaluator can be asked for (descriptor dwords, flat SRT
// slots, resource control-flow conditions and uniform-fill words) into one instruction array,
// specialised per evaluation context, so a draw replays a linear loop instead of walking the IR
// graph. Register i holds the result of instruction i. A root lists the instructions it needs
// with every operand ahead of its user; instructions shared between roots appear in each list and
// are skipped at run time once computed.
enum class SrtFlatOp : uint8_t {
	Imm,          // imm: value
	UserData,     // imm: register relative to user_data_base
	ShaderBase,
	ReadAddress,  // args: low, high, offset; imm: signed immediate byte offset
	ReadBuffer,   // args: low, high, records, offset, word3 (evaluated, unused); imm: byte offset
	ExtractU64,   // args: packed; imm: component
	AddCarry,     // args: lhs, rhs; imm: component (0 = sum, 1 = carry)
	ConstructU64, // args: low, high
	IAdd32,
	IAdd64,
	ISub32,
	ISub64,
	IMul32,
	IMul64,
	UMin32,
	ConvertF32U32,
	ConvertU32F32,
	FPMul32,
	FPTrunc32,
	FPIsNan32,
	FPOrdLessThanEqual32,
	FPOrdGreaterThanEqual32,
	BitwiseAnd32,
	BitwiseAnd64,
	BitwiseOr32,
	BitwiseXor32,
	BitwiseNot32,
	BitCount32,
	FindILsb32,
	FindUMsb32,
	ShiftLeftLogical32,
	ShiftLeftLogical64,
	ShiftRightLogical32,
	ShiftRightLogical64,
	ShiftRightArithmetic32,
	ShiftRightArithmetic64,
	BitFieldUExtract,
	BitFieldSExtract,
	BitFieldInsert,
	Select,
	IEqual32,
	INotEqual32,
	ULessThan32,
	UGreaterThan32,
	LogicalAnd,
	LogicalOr,
	LogicalXor,
	LogicalNot,
};

struct SrtFlatInst {
	static constexpr uint32_t MaxArgs = 5;

	SrtFlatOp op        = SrtFlatOp::Imm;
	uint8_t   arg_count = 0;
	// Memory reads under the clean context go through the specialization reader.
	uint8_t   clean    = 0;
	uint8_t   reserved = 0;
	std::array<uint32_t, MaxArgs> args {};
	uint64_t                      imm = 0;
};

struct SrtFlatRoot {
	uint32_t first = 0; // offset into SrtFlatProgram::schedule
	uint32_t count = 0;
	// A root the IR walker could never evaluate (unsupported opcode, cycle, malformed operand)
	// fails before running anything.
	bool valid = false;
};

struct SrtFlatSourceRoot: SrtFlatRoot {
	std::array<uint32_t, 8> results {};
};

struct SrtFlatValueRoot: SrtFlatRoot {
	uint32_t result = 0;
};

struct SrtFlatProgram {
	bool compiled = false;
	// Plan shape at compile time. Evaluation falls back to the IR walker if the plan changed.
	uint32_t                       value_count = 0;
	std::vector<SrtFlatInst>       insts;
	std::vector<uint32_t>          schedule;
	std::vector<SrtFlatSourceRoot> sources;          // descriptor_sources, normal context
	std::vector<SrtFlatValueRoot>  flat_reads;       // srt_reads, normal context
	std::vector<SrtFlatValueRoot>  clean_flat_reads; // srt_reads flagged clean, clean context
	std::vector<SrtFlatValueRoot>  conditions;       // control_flow conditions, clean context
	std::array<SrtFlatValueRoot, 4> uniform_values {}; // uniform_fill.values, clean context
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTFLATPROGRAM_H_ */
