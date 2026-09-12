#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTFLATPROGRAM_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTFLATPROGRAM_H_

#include <array>
#include <cstddef>
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

// Packed execution form of the normal-context roots (docs/gpu-descriptor-fetch.md, "Packed flat
// program").
//
// The structures above are the compile-time form: one instruction array plus a per-root schedule
// that lists each root's closure. Replaying a root walks `insts[schedule[step]]`, a random access
// into a 32-byte record, and reads the plan's own srt_reads and descriptor_sources beside it, so an
// evaluation touches 40 to 60 scattered lines of plan data.
//
// The packed form is one deduplicated topological order of every descriptor source and every
// non-clean flat read, in execution order, 16 bytes an instruction, with the roots in the same
// allocation. Arguments are positions in that order, so the value memo is a dense array indexed by
// position and there is no schedule indirection: an evaluation walks one contiguous run of about a
// dozen lines. That halves the synthetic cold cost and cuts the hot one by a third; in the renderer
// it is worth about 2 ms of a 73 ms replay loop, because there the plan is mostly warm already and
// the call waits on the first touch of the guest SRT pages instead.
struct SrtPackedInst {
	static constexpr uint32_t MaxArgs = 5;
	// Argument count in bits 0..2; Clean routes the read through the specialization reader, and
	// Wide makes `imm` an index into SrtPackedProgram::wide instead of the value itself.
	static constexpr uint8_t ArgMask = 0x07;
	static constexpr uint8_t Clean   = 0x08;
	static constexpr uint8_t Wide    = 0x10;
	// The immediate is a sign-extended 32-bit offset (ReadAddress); everything else is unsigned.
	static constexpr uint8_t Signed  = 0x20;

	// Ordered so the record is exactly 16 bytes: a 4-byte immediate, two bytes of opcode and
	// flags, then five 2-byte argument positions.
	uint32_t                              imm  = 0;
	uint8_t                               op   = 0; // SrtFlatOp
	uint8_t                               info = 0;
	std::array<uint16_t, MaxArgs>         args {};
};
static_assert(sizeof(SrtPackedInst) == 16, "the packed instruction must stay one quarter line");

// A descriptor source: the packed positions of its dwords. dword_count is carried here so the
// evaluation never touches the plan's DescriptorSource array.
struct SrtPackedSource {
	std::array<uint16_t, 8> results {};
	uint8_t                 dword_count = 0;
	uint8_t                 valid       = 0;
};

// One flat SRT read: its packed position and the slot it is written to.
struct SrtPackedRead {
	static constexpr uint8_t Valid = 0x01;
	// A clean slot reads through the specialization reader under the clean context and keeps the
	// compile-time form; the packed program does not carry its closure.
	static constexpr uint8_t Clean = 0x02;

	uint16_t result      = 0;
	uint16_t flat_offset = 0;
	uint8_t  flags       = 0;
};

struct SrtPackedProgram {
	// False when the plan cannot be packed (resource control flow, more positions than a 16-bit
	// index holds, an immediate the packed record cannot carry): evaluation keeps the form above.
	bool     compiled      = false;
	uint32_t inst_count    = 0;
	// Instructions the descriptor sources alone need; they come first, so an evaluation that does
	// not want the flat reads stops there.
	uint32_t source_prefix = 0;
	uint32_t source_count  = 0;
	uint32_t read_count    = 0;
	uint32_t wide_count    = 0;
	// Byte offsets into `blob` of the four arrays, all 8-byte aligned. One allocation keeps the
	// instructions and the roots on the same run of lines.
	uint32_t inst_offset   = 0;
	uint32_t wide_offset   = 0;
	uint32_t source_offset = 0;
	uint32_t read_offset   = 0;
	std::vector<uint64_t> blob;

	[[nodiscard]] const std::byte* Bytes() const {
		return reinterpret_cast<const std::byte*>(blob.data());
	}
	[[nodiscard]] const SrtPackedInst* Insts() const {
		return reinterpret_cast<const SrtPackedInst*>(Bytes() + inst_offset);
	}
	[[nodiscard]] const uint64_t* Wide() const {
		return reinterpret_cast<const uint64_t*>(Bytes() + wide_offset);
	}
	[[nodiscard]] const SrtPackedSource* Sources() const {
		return reinterpret_cast<const SrtPackedSource*>(Bytes() + source_offset);
	}
	[[nodiscard]] const SrtPackedRead* Reads() const {
		return reinterpret_cast<const SrtPackedRead*>(Bytes() + read_offset);
	}
	[[nodiscard]] size_t BlobBytes() const { return blob.size() * sizeof(uint64_t); }
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
	// Execution form of sources and flat_reads, built from the arrays above.
	SrtPackedProgram                packed;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTFLATPROGRAM_H_ */
