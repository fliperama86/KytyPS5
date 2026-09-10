// Standalone microbenchmark for the per-draw SRT/descriptor evaluation hot path.
//
// EvaluateRuntimeSources runs once per shader stage per draw, so its cost is multiplied by
// millions of calls per capture. This target exercises it over synthetic plans shaped like real
// graphics/compute SRTs so evaluator changes can be measured without a game run.

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "SrtEvaluatorBench: failed: %s\n", text);
		std::abort();
	}
}

Block& AddValueBlock(Program& program) {
	auto  block  = std::make_unique<Block>();
	auto* result = block.get();
	program.blocks.push_back(result);
	program.block_info.push_back({.id = 0});
	program.block_storage.push_back(std::move(block));
	return *result;
}

// Backing store for the guest SRT table the synthetic plans read through. The production runtime
// leaves read_memory null, so the evaluator dereferences the address directly; a host allocation
// stands in for guest direct memory.
struct SrtMemory {
	explicit SrtMemory(size_t dwords): words(dwords, 0u) {
		for (size_t index = 0; index < words.size(); index++) {
			words[index] = static_cast<uint32_t>(0x1000u + index * 4u);
		}
	}

	[[nodiscard]] uint64_t Base() const { return reinterpret_cast<uint64_t>(words.data()); }

	std::vector<uint32_t> words;
};

struct PlanShape {
	const char* name            = "";
	uint32_t    buffers         = 0;
	uint32_t    images          = 0;
	uint32_t    samplers        = 0;
	// Extra arithmetic applied to every descriptor dword, standing in for the address math real
	// shaders perform between the SRT read and the descriptor operand.
	uint32_t    arithmetic_ops  = 0;
	// Number of second-level indirections: each level reads a pointer out of the table and reads
	// the descriptors through that pointer instead.
	uint32_t    indirection     = 0;
	bool        control_flow    = false;
	bool        read_first_lane = false;
};

struct BuiltPlan {
	ResourcePlan          plan;
	std::unique_ptr<SrtMemory> memory;
	std::vector<uint32_t> sources;
	std::vector<uint32_t> user_data;
};

// Builds a post-planning plan directly: srt_reads hold the raw loads, and descriptor dwords
// reference them through ReadConst(GetSrtResource, slot) exactly as PlanBuilder rewrites them.
BuiltPlan BuildPlan(const PlanShape& shape) {
	const uint32_t descriptor_count = shape.buffers + shape.images + shape.samplers;
	const uint32_t dword_total =
	    shape.buffers * 4u + shape.images * 8u + shape.samplers * 4u;

	BuiltPlan built;
	built.memory = std::make_unique<SrtMemory>(static_cast<size_t>(dword_total) * 4u + 64u);

	Program program;
	program.stage                     = Libs::Graphics::ShaderType::Pixel;
	program.srt_plan_complete         = true;
	program.resource_tracking_complete = true;
	program.user_data_base            = 0;
	auto& value_block                 = AddValueBlock(program);

	MemoryInfo memory;
	memory.kind          = ResourceKind::ScalarAddress;
	memory.planning_only = true;
	program.memory_info.push_back(memory);

	// user_data[0..1] carry the SRT table pointer, matching the common PS5 layout.
	auto& low  = value_block.AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(0))});
	auto& high = value_block.AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(1))});
	auto& root = value_block.AppendNewInst(ValueOpcode::GetAddressResource,
	                                       {Value(&low), Value(&high)});

	// The chained pointer lives past every descriptor slot so the checksummed words stay
	// independent of where the host allocation lands.
	const uint32_t pointer_offset = dword_total * 4u + 16u;

	Value handle(&root);
	for (uint32_t level = 0; level < shape.indirection; level++) {
		// Read a pointer pair out of the current table and continue through it.
		auto& next_low = value_block.AppendNewInst(
		    ValueOpcode::LoadAddressU32, {handle, Value(pointer_offset), Value(0u), Value(true)});
		next_low.SetFlags(MemoryFlags {.index = 0, .pc = 0x100u + level});
		auto& next_high = value_block.AppendNewInst(
		    ValueOpcode::LoadAddressU32,
		    {handle, Value(pointer_offset + 4u), Value(0u), Value(true)});
		next_high.SetFlags(MemoryFlags {.index = 0, .pc = 0x200u + level});
		auto& next = value_block.AppendNewInst(ValueOpcode::GetAddressResource,
		                                       {Value(&next_low), Value(&next_high)});
		handle = Value(&next);
	}

	auto& srt = value_block.AppendNewInst(ValueOpcode::GetSrtResource);

	// One raw load per descriptor dword becomes one flat slot.
	std::vector<Value> slot_values;
	slot_values.reserve(dword_total);
	for (uint32_t slot = 0; slot < dword_total; slot++) {
		auto& raw = value_block.AppendNewInst(
		    ValueOpcode::LoadAddressU32, {handle, Value(slot * 4u), Value(0u), Value(true)});
		raw.SetFlags(MemoryFlags {.index = 0, .pc = 0x1000u + slot});
		program.srt_reads.push_back({Value(&raw), slot});

		auto& flat =
		    value_block.AppendNewInst(ValueOpcode::ReadConst, {Value(&srt), Value(slot)});
		Value dword(&flat);
		for (uint32_t op = 0; op < shape.arithmetic_ops; op++) {
			auto& shifted = value_block.AppendNewInst(ValueOpcode::ShiftRightLogical32,
			                                          {dword, Value(op & 7u)});
			auto& masked  = value_block.AppendNewInst(ValueOpcode::BitwiseAnd32,
			                                          {Value(&shifted), Value(0xffffffffu)});
			dword         = Value(&masked);
		}
		if (shape.read_first_lane && (slot % 8u) == 0u) {
			auto& lane = value_block.AppendNewInst(ValueOpcode::ReadFirstLane,
			                                       {dword, Value(&low)});
			dword      = Value(&lane);
		}
		slot_values.push_back(dword);
	}

	uint32_t cursor = 0;
	const auto add_source = [&](uint32_t dwords) {
		DescriptorSource source;
		source.dword_count = dwords;
		for (uint32_t index = 0; index < dwords; index++) {
			source.dwords[index] = slot_values[cursor++];
		}
		program.descriptor_sources.push_back(source);
	};
	for (uint32_t index = 0; index < shape.buffers; index++) {
		add_source(4);
	}
	for (uint32_t index = 0; index < shape.images; index++) {
		add_source(8);
	}
	for (uint32_t index = 0; index < shape.samplers; index++) {
		add_source(4);
	}

	if (shape.control_flow) {
		// Two blocks whose sources are all reachable; the condition is a plain SRT-derived value.
		ResourceBlock entry;
		entry.condition = slot_values.front();
		entry.successors = {1u, 2u};
		program.control_flow.push_back(entry);
		ResourceBlock taken;
		ResourceBlock other;
		for (uint32_t index = 0; index < descriptor_count; index++) {
			(index % 2u == 0u ? taken : other).sources.push_back(index);
		}
		program.control_flow.push_back(taken);
		program.control_flow.push_back(other);
	}

	built.plan = ExtractResourcePlan(program);
	// ExtractResourcePlan derives control flow from the translated CFG, which the synthetic
	// program does not carry; restore the shape's blocks against the cloned conditions.
	if (shape.control_flow && built.plan.control_flow.empty()) {
		built.plan.control_flow = program.control_flow;
		for (auto& block: built.plan.control_flow) {
			block.condition = {};
		}
	}
	built.plan.clean_flat_slots.assign(built.plan.srt_reads.size(), 0u);

	built.sources.resize(built.plan.descriptor_sources.size());
	for (uint32_t index = 0; index < built.sources.size(); index++) {
		built.sources[index] = index;
	}

	const auto base = built.memory->Base();
	built.user_data.assign(16, 0u);
	built.user_data[0] = static_cast<uint32_t>(base);
	built.user_data[1] = static_cast<uint32_t>(base >> 32u);
	// Second-level tables point back into the same allocation.
	built.memory->words[pointer_offset / 4u]      = static_cast<uint32_t>(base);
	built.memory->words[pointer_offset / 4u + 1u] = static_cast<uint32_t>(base >> 32u);
	return built;
}

struct Result {
	double   ns_per_call = 0.0;
	uint64_t checksum    = 0;
	uint64_t calls       = 0;
};

Result Measure(const BuiltPlan& built, uint64_t iterations) {
	const SrtRuntime runtime {
	    .user_data   = built.user_data,
	    .shader_base = 0,
	    // Production leaves read_memory null so raw loads dereference guest memory directly.
	    .read_memory = nullptr,
	};

	std::vector<DescriptorValue> results;
	std::vector<uint32_t>        flat;
	std::vector<uint8_t>         active;
	uint64_t                     checksum = 0;

	// Warm up so the first-touch page faults and branch predictors do not skew the sample.
	for (uint64_t index = 0; index < 64; index++) {
		Check(EvaluateRuntimeSources(built.plan, built.sources, runtime, results, flat,
		                             built.plan.clean_flat_slots, active),
		      "warm-up evaluation failed");
	}

	const auto start = std::chrono::steady_clock::now();
	for (uint64_t index = 0; index < iterations; index++) {
		if (!EvaluateRuntimeSources(built.plan, built.sources, runtime, results, flat,
		                            built.plan.clean_flat_slots, active)) {
			Check(false, "evaluation failed");
		}
		checksum += results.empty() ? 0u : results.front().dwords[0];
		checksum += flat.empty() ? 0u : flat.back();
	}
	const auto elapsed = std::chrono::steady_clock::now() - start;

	Result result;
	result.calls    = iterations;
	result.ns_per_call =
	    static_cast<double>(
	        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
	    static_cast<double>(iterations);
	result.checksum = checksum;
	return result;
}

const PlanShape Shapes[] = {
    {.name = "small-vs", .buffers = 2, .images = 0, .samplers = 0, .arithmetic_ops = 1},
    {.name = "typical-ps",
     .buffers        = 4,
     .images         = 6,
     .samplers       = 4,
     .arithmetic_ops = 2},
    {.name = "heavy-ps",
     .buffers        = 8,
     .images         = 12,
     .samplers       = 8,
     .arithmetic_ops = 3},
    {.name = "chained-cs",
     .buffers        = 6,
     .images         = 4,
     .samplers       = 2,
     .arithmetic_ops = 2,
     .indirection    = 2},
    {.name = "control-flow-ps",
     .buffers        = 4,
     .images         = 6,
     .samplers       = 4,
     .arithmetic_ops = 2,
     .control_flow   = true},
    {.name            = "waterfall-ps",
     .buffers         = 4,
     .images          = 6,
     .samplers        = 4,
     .arithmetic_ops  = 2,
     .read_first_lane = true},
};

} // namespace

namespace Common {

int DbgExitHandler(const char*, int, std::string_view) { std::abort(); }

int DbgExitHandler(const char*, int, fmt::text_style, std::string_view) { std::abort(); }

int DbgExitIfHandler(const char*, const char*, int) { return 1; }

void DbgExit(int) { std::abort(); }

} // namespace Common

int main(int argc, char** argv) {
	uint64_t    iterations = 200000;
	std::string only;
	bool        sweep = false;
	for (int index = 1; index < argc; index++) {
		const std::string arg = argv[index];
		if (arg.rfind("--iterations=", 0) == 0) {
			iterations = std::strtoull(arg.c_str() + 13, nullptr, 10);
		} else if (arg.rfind("--only=", 0) == 0) {
			only = arg.substr(7);
		} else if (arg == "--sweep") {
			sweep = true;
		} else {
			std::fprintf(
			    stderr,
			    "usage: srt_evaluator_bench [--iterations=N] [--only=NAME] [--sweep]\n");
			return 2;
		}
	}

	uint64_t checksum = 0;
	if (sweep) {
		// Varying the per-slot instruction count separates the fixed per-slot cost (the guest read
		// plus the memo and descriptor stores) from the marginal cost of one more IR instruction.
		std::printf("%-10s %12s %10s %10s\n", "shape", "flat_slots", "ops/slot", "ns/call");
		for (const uint32_t slots: {40u, 80u, 160u}) {
			for (uint32_t ops = 0; ops <= 6u; ops += 2u) {
				PlanShape shape {};
				shape.name           = "sweep";
				shape.buffers        = slots / 4u;
				shape.arithmetic_ops = ops;
				auto       built  = BuildPlan(shape);
				const auto result = Measure(built, iterations);
				checksum += result.checksum;
				std::printf("%-10s %12zu %10u %10.1f\n", "sweep",
				            built.plan.srt_reads.size(), ops, result.ns_per_call);
			}
		}
		std::printf("checksum %llu\n", static_cast<unsigned long long>(checksum));
		return 0;
	}

	std::printf("%-18s %10s %12s %10s\n", "shape", "sources", "flat_slots", "ns/call");
	for (const auto& shape: Shapes) {
		if (!only.empty() && only != shape.name) {
			continue;
		}
		auto       built  = BuildPlan(shape);
		const auto result = Measure(built, iterations);
		checksum += result.checksum;
		std::printf("%-18s %10zu %12zu %10.1f\n", shape.name, built.sources.size(),
		            built.plan.srt_reads.size(), result.ns_per_call);
	}
	std::printf("checksum %llu\n", static_cast<unsigned long long>(checksum));
	return 0;
}

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
