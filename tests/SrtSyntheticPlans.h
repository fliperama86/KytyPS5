#pragma once

// Synthetic SRT resource plans shared by the SRT tests.
//
// The plans are built post-planning, the way PlanBuilder leaves a real shader: descriptor sources
// reference raw LoadAddressU32/ReadConstBuffer values through typed resource handles. Every source
// is one test case for the flat program (CompileSrtPlan) and for its SPIR-V lowering.
//
// All guest memory lives in one 16 KiB page at a low fake guest address so the same backing store
// can be mapped into the BDA page table on the GPU and read through the evaluator's read_memory
// callback on the CPU. Host pointers are far above the page table's 40-bit space, so a direct
// dereference is deliberately not used here.

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SrtSynthetic {

namespace SIR = Libs::Graphics::ShaderRecompiler::IR;

// Must match BufferCache::CACHING_PAGESIZE; the test that maps this asserts it.
inline constexpr uint32_t PageBytes  = 16384u;
inline constexpr uint32_t PageDwords = PageBytes / 4u;

// Page 0 of the backing buffer is the shader's output area, page 1 is the guest page.
inline constexpr uint32_t OutputDwords     = PageDwords;
inline constexpr uint32_t GuestDwordOrigin = OutputDwords;
inline constexpr uint32_t BackingDwords    = OutputDwords + PageDwords;

// Fake guest addresses. Both are page aligned and far below the page table's 40-bit limit; only
// the first is ever mapped.
inline constexpr uint64_t GuestBase    = 0x0000000000400000ull;
inline constexpr uint64_t UnmappedBase = GuestBase + PageBytes;

// Dword slots inside the guest page.
inline constexpr uint32_t SlotPayloadA = 0;   // 8 words
inline constexpr uint32_t SlotPayloadB = 8;   // 8 words
inline constexpr uint32_t SlotChainPtr = 16;  // address pair -> SlotChainOne
inline constexpr uint32_t SlotBadPtr   = 20;  // address pair -> UnmappedBase
inline constexpr uint32_t SlotChainOne = 256; // address pair -> SlotChainTwo
inline constexpr uint32_t SlotChainTwo = 512; // address pair -> SlotPayloadC
inline constexpr uint32_t SlotPayloadC = 768; // 8 words

// Output layout: one slice per descriptor source.
inline constexpr uint32_t SourceStride  = 16u;
inline constexpr uint32_t ValidWordSlot = 8u;  // 1 when the shader root produced values
inline constexpr uint32_t LoweredSlot   = 9u;  // 1 when every step of the root had a lowering

// GetShaderBase is a per-draw constant; the shader lowering takes the same value as a literal.
inline constexpr uint64_t ShaderBase = 0x0123456789abcdefull;

struct SyntheticPlan {
	SIR::ResourcePlan          plan;
	std::vector<uint32_t>      backing;   // the whole storage buffer: output page then guest page
	std::array<uint32_t, 64>   user_data {};
	std::vector<std::string>   case_names;
	std::vector<uint32_t>      dword_counts;
};

namespace detail {

inline uint32_t Low(uint64_t value) { return static_cast<uint32_t>(value); }
inline uint32_t High(uint64_t value) { return static_cast<uint32_t>(value >> 32u); }

inline SIR::Block& AddValueBlock(SIR::Program& program) {
	auto  block  = std::make_unique<SIR::Block>();
	auto* result = block.get();
	program.blocks.push_back(result);
	program.block_info.push_back({.id = 0});
	program.block_storage.push_back(std::move(block));
	return *result;
}

// Memory metadata slots. Raw reads carry the immediate byte offset in MemoryInfo::offset, so one
// slot per distinct immediate is needed.
enum MemorySlot : uint32_t {
	AddressZero = 0,
	AddressPlus16,
	AddressMinus16,
	BufferZero,
	MemorySlotCount,
};

class Builder {
public:
	explicit Builder(SIR::Block& block): m_block(block) {}

	SIR::Value Imm(uint32_t value) { return SIR::Value(value); }

	SIR::Value UserData(uint32_t reg) {
		return SIR::Value(&m_block.AppendNewInst(SIR::ValueOpcode::GetUserData,
		                                         {SIR::Value(static_cast<SIR::ScalarReg>(reg))}));
	}

	SIR::Value ShaderBaseValue() {
		return SIR::Value(&m_block.AppendNewInst(SIR::ValueOpcode::GetShaderBase));
	}

	SIR::Value AddressHandle(SIR::Value low, SIR::Value high) {
		return SIR::Value(
		    &m_block.AppendNewInst(SIR::ValueOpcode::GetAddressResource, {low, high}));
	}

	SIR::Value BufferHandle(SIR::Value low, SIR::Value high, SIR::Value records,
	                        SIR::Value word3) {
		return SIR::Value(&m_block.AppendNewInst(SIR::ValueOpcode::GetBufferResource,
		                                         {low, high, records, word3}));
	}

	SIR::Value Load(SIR::Value handle, uint32_t offset, MemorySlot slot = AddressZero) {
		auto& inst = m_block.AppendNewInst(
		    SIR::ValueOpcode::LoadAddressU32,
		    {handle, SIR::Value(offset), SIR::Value(0u), SIR::Value(true)});
		inst.SetFlags(SIR::MemoryFlags {.index = slot, .pc = m_pc++});
		return SIR::Value(&inst);
	}

	SIR::Value LoadBuffer(SIR::Value handle, uint32_t offset, MemorySlot slot = BufferZero) {
		auto& inst =
		    m_block.AppendNewInst(SIR::ValueOpcode::ReadConstBuffer, {handle, SIR::Value(offset)});
		inst.SetFlags(SIR::MemoryFlags {.index = slot, .pc = m_pc++});
		return SIR::Value(&inst);
	}

	SIR::Value Unary(SIR::ValueOpcode op, SIR::Value a) {
		return SIR::Value(&m_block.AppendNewInst(op, {a}));
	}

	SIR::Value Binary(SIR::ValueOpcode op, SIR::Value a, SIR::Value b) {
		return SIR::Value(&m_block.AppendNewInst(op, {a, b}));
	}

	SIR::Value Ternary(SIR::ValueOpcode op, SIR::Value a, SIR::Value b, SIR::Value c) {
		return SIR::Value(&m_block.AppendNewInst(op, {a, b, c}));
	}

	SIR::Value Quaternary(SIR::ValueOpcode op, SIR::Value a, SIR::Value b, SIR::Value c,
	                      SIR::Value d) {
		return SIR::Value(&m_block.AppendNewInst(op, {a, b, c, d}));
	}

	// Follows one address pair stored in guest memory.
	SIR::Value Chase(SIR::Value handle, uint32_t dword_slot) {
		const auto low  = Load(handle, dword_slot * 4u);
		const auto high = Load(handle, dword_slot * 4u + 4u);
		return AddressHandle(low, high);
	}

private:
	SIR::Block& m_block;
	uint32_t      m_pc = 0x1000u;
};

} // namespace detail

// Builds the plan, its guest page image and the user data the plan reads.
inline SyntheticPlan BuildSyntheticPlan() {
	using Op = SIR::ValueOpcode;
	using namespace detail;

	SyntheticPlan built;
	built.backing.assign(BackingDwords, 0u);
	const auto guest_word = [&](uint32_t slot) -> uint32_t& {
		return built.backing[GuestDwordOrigin + slot];
	};
	const auto store_pointer = [&](uint32_t slot, uint64_t address) {
		guest_word(slot)      = Low(address);
		guest_word(slot + 1u) = High(address);
	};

	// Payload A is picked so the low words overflow when added to payload B.
	for (uint32_t i = 0; i < 8u; i++) {
		guest_word(SlotPayloadA + i) = 0xf0000000u + i * 0x01010101u;
		guest_word(SlotPayloadB + i) = 0x20000000u + i * 0x02030405u;
		guest_word(SlotPayloadC + i) = 0xc0ffee00u + i * 0x00010203u;
	}
	// Distinct bit patterns for the ALU and float cases.
	guest_word(SlotPayloadA + 3) = 0x8000f00fu;
	guest_word(SlotPayloadB + 3) = 0x0000000bu; // shift amount / bitfield width
	guest_word(SlotPayloadB + 4) = 0x00000004u; // bitfield offset
	guest_word(SlotPayloadB + 5) = 0x3fc00000u; // 1.5f
	guest_word(SlotPayloadB + 6) = 0x40490fdbu; // 3.14159f
	guest_word(SlotPayloadB + 7) = 0x00000000u;
	guest_word(SlotPayloadA + 5) = 0xc0490fdbu; // -3.14159f, a normal the GPU cannot flush

	store_pointer(SlotChainPtr, GuestBase + SlotChainOne * 4u);
	store_pointer(SlotChainOne, GuestBase + SlotChainTwo * 4u);
	store_pointer(SlotChainTwo, GuestBase + SlotPayloadC * 4u);
	store_pointer(SlotBadPtr, UnmappedBase);

	built.user_data[0] = Low(GuestBase);
	built.user_data[1] = High(GuestBase);
	built.user_data[2] = 0x00000010u;
	built.user_data[3] = 0xdeadbeefu;
	built.user_data[4] = Low(GuestBase);
	built.user_data[5] = High(GuestBase); // stride 0: a raw buffer sized in bytes
	built.user_data[6] = PageBytes;       // records for the in-range buffer
	built.user_data[7] = 16u;             // records for the out-of-range buffer

	SIR::Program program;
	program.stage                      = Libs::Graphics::ShaderType::Compute;
	program.srt_plan_complete          = true;
	program.resource_tracking_complete = true;
	program.user_data_base             = 0;
	auto& block                        = AddValueBlock(program);

	program.memory_info.resize(MemorySlotCount);
	const auto address_slot = [&](MemorySlot slot, uint32_t offset) {
		program.memory_info[slot].kind          = SIR::ResourceKind::ScalarAddress;
		program.memory_info[slot].offset        = offset;
		program.memory_info[slot].planning_only = true;
	};
	address_slot(AddressZero, 0u);
	address_slot(AddressPlus16, 16u);
	address_slot(AddressMinus16, static_cast<uint32_t>(-16));
	program.memory_info[BufferZero].kind          = SIR::ResourceKind::ScalarBuffer;
	program.memory_info[BufferZero].offset        = 0u;
	program.memory_info[BufferZero].planning_only = true;

	Builder b(block);

	const auto root    = b.AddressHandle(b.UserData(0), b.UserData(1));
	const auto in_buf  = b.BufferHandle(b.UserData(4), b.UserData(5), b.UserData(6), b.Imm(0u));
	const auto out_buf = b.BufferHandle(b.UserData(4), b.UserData(5), b.UserData(7), b.Imm(0u));

	const auto add_source = [&](const char* name, std::vector<SIR::Value> dwords) {
		SIR::DescriptorSource source;
		source.dword_count = static_cast<uint32_t>(dwords.size());
		for (uint32_t i = 0; i < source.dword_count; i++) {
			source.dwords[i] = dwords[i];
		}
		program.descriptor_sources.push_back(source);
		built.case_names.emplace_back(name);
		built.dword_counts.push_back(source.dword_count);
	};

	// 1. A three-deep pointer chain ending in eight loads.
	{
		const auto level1 = b.Chase(root, SlotChainPtr);
		const auto level2 = b.Chase(level1, 0);
		const auto level3 = b.Chase(level2, 0);
		std::vector<SIR::Value> dwords;
		for (uint32_t i = 0; i < 8u; i++) {
			dwords.push_back(b.Load(level3, i * 4u));
		}
		add_source("chain-3-deep", dwords);
	}

	// 2. A buffer read fully inside its record count.
	{
		std::vector<SIR::Value> dwords;
		for (uint32_t i = 0; i < 8u; i++) {
			dwords.push_back(b.LoadBuffer(in_buf, (SlotPayloadA + i) * 4u));
		}
		add_source("buffer-in-range", dwords);
	}

	// 3. The last dword that still fits a 16-byte buffer, and an unaligned offset inside it.
	{
		std::vector<SIR::Value> dwords {
		    b.LoadBuffer(out_buf, 0u),
		    b.LoadBuffer(out_buf, 12u),
		    b.LoadBuffer(out_buf, 13u), // aligned down to 12, still the last whole dword
		    b.LoadBuffer(out_buf, 8u),
		};
		add_source("buffer-edge-in-range", dwords);
	}

	// 4. Past the record count in both directions.
	{
		std::vector<SIR::Value> dwords {
		    b.LoadBuffer(out_buf, 0u),
		    b.LoadBuffer(out_buf, 16u), // size - aligned == 0, no whole dword left
		};
		add_source("buffer-out-of-range", dwords);
	}

	// 5. 64-bit addition, carry extraction and the wide integer ops.
	{
		const auto a_lo = b.Load(root, SlotPayloadA * 4u);
		const auto a_hi = b.Load(root, (SlotPayloadA + 1u) * 4u);
		const auto b_lo = b.Load(root, SlotPayloadB * 4u);
		const auto b_hi = b.Load(root, (SlotPayloadB + 1u) * 4u);
		const auto carry = b.Binary(Op::IAddCarry32, a_lo, b_lo);
		const auto sum_lo = b.Binary(Op::CompositeExtractU32x2, carry, b.Imm(0u));
		const auto carry_out = b.Binary(Op::CompositeExtractU32x2, carry, b.Imm(1u));
		const auto sum_hi =
		    b.Binary(Op::IAdd32, b.Binary(Op::IAdd32, a_hi, b_hi), carry_out);
		const auto wide_a  = b.Binary(Op::CompositeConstructU64, a_lo, a_hi);
		const auto wide_b  = b.Binary(Op::CompositeConstructU64, b_lo, b_hi);
		const auto total   = b.Binary(Op::IAdd64, wide_a, wide_b);
		const auto diff    = b.Binary(Op::ISub64, wide_a, wide_b);
		const auto product = b.Binary(Op::IMul64, wide_a, wide_b);
		std::vector<SIR::Value> dwords {
		    sum_lo,
		    sum_hi,
		    carry_out,
		    b.Binary(Op::CompositeExtractU64, total, b.Imm(0u)),
		    b.Binary(Op::CompositeExtractU64, total, b.Imm(1u)),
		    b.Binary(Op::CompositeExtractU64, diff, b.Imm(1u)),
		    b.Binary(Op::CompositeExtractU64, product, b.Imm(0u)),
		    b.Binary(Op::CompositeExtractU64, product, b.Imm(1u)),
		};
		add_source("add-carry-64", dwords);
	}

	// 6. Shifts, bitfield operations and select over loaded operands.
	{
		const auto value  = b.Load(root, (SlotPayloadA + 3u) * 4u); // 0x8000f00f
		const auto width  = b.Load(root, (SlotPayloadB + 3u) * 4u); // 11
		const auto offset = b.Load(root, (SlotPayloadB + 4u) * 4u); // 4
		const auto other  = b.Load(root, (SlotPayloadA + 1u) * 4u);
		const auto wide   = b.Binary(Op::CompositeConstructU64, value, other);
		std::vector<SIR::Value> dwords {
		    b.Binary(Op::ShiftLeftLogical32, value, offset),
		    b.Binary(Op::ShiftRightArithmetic32, value, offset),
		    b.Binary(Op::CompositeExtractU64,
		             b.Binary(Op::ShiftRightArithmetic64, wide, b.Imm(33u)), b.Imm(0u)),
		    b.Ternary(Op::BitFieldUExtract, value, offset, width),
		    b.Ternary(Op::BitFieldSExtract, value, offset, width),
		    b.Quaternary(Op::BitFieldInsert, value, other, offset, width),
		    b.Ternary(Op::SelectU32, b.Binary(Op::ULessThan32, value, other), value, other),
		    b.Binary(Op::UMin32, b.Unary(Op::BitCount32, value),
		             b.Binary(Op::IAdd32, b.Unary(Op::FindUMsb32, value),
		                      b.Unary(Op::FindILsb32, other))),
		};
		add_source("alu-shifts-bitfields", dwords);
	}

	// 7. Logical and comparison results, which the walker represents as 0 or 1.
	{
		const auto value = b.Load(root, (SlotPayloadA + 3u) * 4u);
		const auto other = b.Load(root, (SlotPayloadB + 7u) * 4u); // zero
		std::vector<SIR::Value> dwords {
		    b.Binary(Op::IEqual32, value, other),
		    b.Binary(Op::INotEqual32, value, other),
		    b.Binary(Op::UGreaterThan32, value, other),
		    b.Binary(Op::LogicalAnd, value, other),
		    b.Binary(Op::LogicalOr, value, other),
		    b.Binary(Op::LogicalXor, value, other),
		    b.Unary(Op::LogicalNot, other),
		    b.Binary(Op::BitwiseXor32, b.Unary(Op::BitwiseNot32, value),
		             b.Binary(Op::BitwiseOr32, value, other)),
		};
		add_source("logical-and-compare", dwords);
	}

	// 8. Float conversions and comparisons over well-behaved inputs.
	{
		const auto one_five = b.Load(root, (SlotPayloadB + 5u) * 4u); // 1.5f
		const auto pi       = b.Load(root, (SlotPayloadB + 6u) * 4u); // 3.14159f
		const auto four     = b.Load(root, (SlotPayloadB + 4u) * 4u); // 4
		const auto product  = b.Binary(Op::FPMul32, one_five, pi);
		std::vector<SIR::Value> dwords {
		    b.Unary(Op::ConvertF32U32, four),
		    b.Unary(Op::ConvertU32F32, pi),
		    product,
		    b.Unary(Op::FPTrunc32, product),
		    b.Unary(Op::FPIsNan32, product),
		    b.Binary(Op::FPOrdLessThanEqual32, one_five, pi),
		    b.Binary(Op::FPOrdGreaterThanEqual32, one_five, pi),
		    b.Unary(Op::BitCastF32U32, b.Unary(Op::BitCastU32F32, product)),
		};
		add_source("float-ops", dwords);
	}

	// 9. A float the walker refuses to convert, so the whole source fails.
	{
		std::vector<SIR::Value> dwords {
		    b.Unary(Op::ConvertU32F32, b.Load(root, (SlotPayloadA + 5u) * 4u)), // -3.14159f
		};
		add_source("float-convert-out-of-range", dwords);
	}

	// 10. Immediate byte offsets, one positive and one negative.
	{
		const auto base = b.AddressHandle(b.Imm(Low(GuestBase + SlotPayloadB * 4u)),
		                                  b.Imm(High(GuestBase + SlotPayloadB * 4u)));
		std::vector<SIR::Value> dwords {
		    b.Load(base, 0u, AddressPlus16),
		    b.Load(base, 0u, AddressMinus16),
		    b.Load(base, 8u, AddressPlus16),
		};
		add_source("signed-immediate-offsets", dwords);
	}

	// 11. A negative immediate larger than the base, which AddSignedAddress rejects.
	{
		const auto tiny = b.AddressHandle(b.Imm(8u), b.Imm(0u));
		add_source("address-underflow", {b.Load(tiny, 0u, AddressMinus16)});
	}

	// 12. A pointer into a page the BDA table does not map.
	{
		const auto bad = b.Chase(root, SlotBadPtr);
		std::vector<SIR::Value> dwords {b.Load(bad, 0u), b.Load(bad, 4u)};
		add_source("unmapped-page", dwords);
	}

	// 13. User data, the shader base and a select driven by a 64-bit comparison.
	{
		const auto base  = b.ShaderBaseValue();
		const auto ud2   = b.UserData(2);
		const auto ud3   = b.UserData(3);
		std::vector<SIR::Value> dwords {
		    ud2,
		    ud3,
		    b.Binary(Op::CompositeExtractU64, base, b.Imm(0u)),
		    b.Binary(Op::CompositeExtractU64, base, b.Imm(1u)),
		    b.Ternary(Op::SelectU32, b.Binary(Op::IEqual32, ud2, b.Imm(0x10u)), ud3, ud2),
		    b.Binary(Op::IMul32, ud2, ud3),
		    b.Binary(Op::ISub32, ud2, ud3),
		    b.Binary(Op::CompositeExtractU64,
		             b.Binary(Op::BitwiseAnd64, base,
		                      b.Binary(Op::CompositeConstructU64, ud3, ud2)),
		             b.Imm(0u)),
		};
		add_source("user-data-and-shader-base", dwords);
	}

	built.plan = SIR::ExtractResourcePlan(program);
	built.plan.clean_flat_slots.assign(built.plan.srt_reads.size(), 0u);
	return built;
}

// Translates a fake guest address to the backing buffer. Host pointers do not fit the BDA page
// table, so both sides of the comparison read through this instead of dereferencing directly.
inline bool ReadGuestMemory(void* userdata, uint64_t address, uint32_t* value) {
	const auto* backing = static_cast<const std::vector<uint32_t>*>(userdata);
	if (backing == nullptr || value == nullptr || address % 4u != 0u) {
		return false;
	}
	if (address < GuestBase || address >= GuestBase + PageBytes) {
		return false;
	}
	const auto index = GuestDwordOrigin + static_cast<uint32_t>((address - GuestBase) / 4u);
	if (index >= backing->size()) {
		return false;
	}
	*value = (*backing)[index];
	return true;
}

} // namespace SrtSynthetic
