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
#include <immintrin.h>
#include <memory>
#include <span>
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
	// Flat SRT slots no descriptor source consumes: values the shader body reads for itself. The
	// game's mix is about 21.5 slots an event against 8.5 descriptor dwords, so most slots are
	// these. Stage 1b of docs/gpu-descriptor-fetch.md only removes host work for them, because a
	// slot a descriptor source also reads is the same instruction in the flat program.
	uint32_t    body_slots      = 0;
};

struct BuiltPlan {
	ResourcePlan          plan;
	std::unique_ptr<SrtMemory> memory;
	std::vector<uint32_t> sources;
	std::vector<uint32_t> user_data;
	// One byte per flat slot, non-zero for a slot no descriptor source consumes.
	std::vector<uint8_t>  body_mask;
	std::vector<uint8_t>  all_mask;
	// The guest table this copy chases through, wherever it lives.
	uint32_t*             table        = nullptr;
	size_t                table_dwords = 0;
};

// Builds a post-planning plan directly: srt_reads hold the raw loads, and descriptor dwords
// reference them through ReadConst(GetSrtResource, slot) exactly as PlanBuilder rewrites them.
BuiltPlan BuildPlan(const PlanShape& shape, uint32_t* external = nullptr,
                    size_t external_dwords = 0) {
	const uint32_t descriptor_count = shape.buffers + shape.images + shape.samplers;
	const uint32_t descriptor_dwords =
	    shape.buffers * 4u + shape.images * 8u + shape.samplers * 4u;
	const uint32_t dword_total = descriptor_dwords + shape.body_slots;

	BuiltPlan    built;
	const size_t table_dwords = static_cast<size_t>(dword_total) * 4u + 64u;
	// A cold run places every copy's table inside one large image; otherwise each plan owns its
	// own small allocation, which is what the hot measurement wants.
	if (external != nullptr) {
		Check(external_dwords >= table_dwords, "external guest image slice is too small");
		built.table = external;
		for (size_t index = 0; index < table_dwords; index++) {
			built.table[index] = static_cast<uint32_t>(0x1000u + index * 4u);
		}
	} else {
		built.memory = std::make_unique<SrtMemory>(table_dwords);
		built.table  = built.memory->words.data();
	}
	built.table_dwords = table_dwords;

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
	// The plan was patched after extraction, so lower it again.
	CompileSrtPlan(built.plan);

	built.all_mask.assign(built.plan.srt_reads.size(), 1u);
	built.body_mask.assign(built.plan.srt_reads.size(), 0u);
	for (size_t slot = descriptor_dwords; slot < built.body_mask.size(); slot++) {
		built.body_mask[slot] = 1u;
	}

	built.sources.resize(built.plan.descriptor_sources.size());
	for (uint32_t index = 0; index < built.sources.size(); index++) {
		built.sources[index] = index;
	}

	const auto base = reinterpret_cast<uint64_t>(built.table);
	built.user_data.assign(16, 0u);
	built.user_data[0] = static_cast<uint32_t>(base);
	built.user_data[1] = static_cast<uint32_t>(base >> 32u);
	// Second-level tables point back into the same allocation.
	built.table[pointer_offset / 4u]      = static_cast<uint32_t>(base);
	built.table[pointer_offset / 4u + 1u] = static_cast<uint32_t>(base >> 32u);
	return built;
}

struct Result {
	double   ns_per_call = 0.0;
	uint64_t checksum    = 0;
	uint64_t calls       = 0;
};

// use_flat selects the compiled program or the IR walker; both must produce the same values.
// skip names the flat slots the shader evaluates for itself (docs/gpu-descriptor-fetch.md, stage
// 1b), which the evaluator leaves zero; the checksum is then not comparable across masks.
Result Measure(BuiltPlan& built, uint64_t iterations, bool use_flat,
               std::span<const uint8_t> skip = {}) {
	built.plan.flat.compiled = use_flat;
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
		                             built.plan.clean_flat_slots, active, {}, skip),
		      "warm-up evaluation failed");
	}

	const auto start = std::chrono::steady_clock::now();
	for (uint64_t index = 0; index < iterations; index++) {
		if (!EvaluateRuntimeSources(built.plan, built.sources, runtime, results, flat,
		                            built.plan.clean_flat_slots, active, {}, skip)) {
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

// Cold-locality machinery (docs/gpu-descriptor-fetch.md, "Where the evaluator's 1.5 us goes").
// The hot measurement above keeps one plan and one small table, so every line it touches is in L1
// after the first iteration. The renderer never does: 493 plans and their guest tables are cycled
// across a loop, with the whole rest of the render thread between two evaluations of the same
// shader. These helpers reproduce the two halves of that separately -- plan data and guest table --
// so the cost can be attributed instead of guessed.

constexpr size_t CacheLine = 64;

void FlushRange(const void* data, size_t bytes) {
	if (data == nullptr || bytes == 0) {
		return;
	}
	auto       address = reinterpret_cast<uintptr_t>(data) & ~uintptr_t {CacheLine - 1};
	const auto end     = reinterpret_cast<uintptr_t>(data) + bytes;
	for (; address < end; address += CacheLine) {
		_mm_clflush(reinterpret_cast<const void*>(address));
	}
}

// Everything an evaluation reads out of the plan: the compiled program, the roots it walks and the
// plan fields the entry points check.
void FlushPlan(const BuiltPlan& built) {
	const auto& plan = built.plan;
	const auto& flat = plan.flat;
	FlushRange(&plan, sizeof(ResourcePlan));
	FlushRange(&built, sizeof(BuiltPlan));
	FlushRange(flat.insts.data(), flat.insts.size() * sizeof(SrtFlatInst));
	FlushRange(flat.schedule.data(), flat.schedule.size() * sizeof(uint32_t));
	FlushRange(flat.sources.data(), flat.sources.size() * sizeof(SrtFlatSourceRoot));
	FlushRange(flat.flat_reads.data(), flat.flat_reads.size() * sizeof(SrtFlatValueRoot));
	FlushRange(plan.srt_reads.data(), plan.srt_reads.size() * sizeof(SrtRead));
	FlushRange(plan.clean_flat_slots.data(), plan.clean_flat_slots.size());
	FlushRange(plan.descriptor_sources.data(),
	           plan.descriptor_sources.size() * sizeof(DescriptorSource));
	FlushRange(built.sources.data(), built.sources.size() * sizeof(uint32_t));
	FlushRange(built.user_data.data(), built.user_data.size() * sizeof(uint32_t));
}

void FlushGuest(const BuiltPlan& built) {
	FlushRange(built.table, built.table_dwords * sizeof(uint32_t));
}

enum FlushKind : uint32_t {
	FlushNone  = 0,
	FlushPlanOnly  = 1,
	FlushGuestOnly = 2,
	FlushBoth      = 3,
};

// Bytes of plan and guest table one evaluation can touch, for the footprint column.
size_t PlanBytes(const BuiltPlan& built) {
	const auto& flat = built.plan.flat;
	return sizeof(ResourcePlan) + flat.insts.size() * sizeof(SrtFlatInst) +
	       flat.schedule.size() * sizeof(uint32_t) +
	       flat.sources.size() * sizeof(SrtFlatSourceRoot) +
	       flat.flat_reads.size() * sizeof(SrtFlatValueRoot) +
	       built.plan.srt_reads.size() * sizeof(SrtRead) +
	       built.plan.descriptor_sources.size() * sizeof(DescriptorSource);
}

// One guest image, one plan copy per page-aligned slice of it.
struct ColdSet {
	std::vector<uint32_t>  image;
	std::vector<BuiltPlan> plans;
};

ColdSet BuildColdSet(const PlanShape& shape, size_t copies, size_t stride_bytes) {
	ColdSet      set;
	const size_t stride = stride_bytes / sizeof(uint32_t);
	set.image.assign(stride * copies + 1024u, 0u);
	set.plans.reserve(copies);
	for (size_t index = 0; index < copies; index++) {
		set.plans.push_back(BuildPlan(shape, set.image.data() + index * stride, stride));
	}
	return set;
}

// Cycles the copies in a pseudo-random order so consecutive evaluations touch unrelated lines and
// no prefetcher can follow. skip_eval runs the same cycle and the same flushes without the
// evaluation, which is the baseline the flushed rows subtract.
Result MeasureCycle(std::vector<BuiltPlan>& plans, uint64_t iterations, bool fresh_vectors,
                    uint32_t flush, bool skip_eval = false) {
	std::vector<DescriptorValue> results;
	std::vector<uint32_t>        flat;
	std::vector<uint8_t>         active;
	uint64_t                     checksum = 0;
	uint64_t                     state    = 0x243f6a8885a308d3ull;

	const auto evaluate = [&](BuiltPlan& built, std::vector<DescriptorValue>& r,
	                          std::vector<uint32_t>& f, std::vector<uint8_t>& a) {
		const SrtRuntime runtime {
		    .user_data   = built.user_data,
		    .shader_base = 0,
		    .read_memory = nullptr,
		};
		if (!EvaluateRuntimeSources(built.plan, built.sources, runtime, r, f,
		                            built.plan.clean_flat_slots, a)) {
			Check(false, "evaluation failed");
		}
		checksum += r.empty() ? 0u : r.front().dwords[0];
		checksum += f.empty() ? 0u : f.back();
	};
	for (auto& built: plans) {
		built.plan.flat.compiled = true;
		evaluate(built, results, flat, active);
	}

	const auto start = std::chrono::steady_clock::now();
	for (uint64_t index = 0; index < iterations; index++) {
		state       = state * 6364136223846793005ull + 1442695040888963407ull;
		auto& built = plans[(state >> 33u) % plans.size()];
		if (flush != FlushNone) {
			if ((flush & FlushPlanOnly) != 0u) {
				FlushPlan(built);
			}
			if ((flush & FlushGuestOnly) != 0u) {
				FlushGuest(built);
			}
			_mm_mfence();
		}
		if (skip_eval) {
			checksum += built.user_data[0];
			continue;
		}
		if (fresh_vectors) {
			// What the renderer does: MaterializeSnapshot declares these three per call, so the
			// evaluator's scratch is handed empty buffers and reallocates every time.
			std::vector<DescriptorValue> local_results;
			std::vector<uint32_t>        local_flat;
			std::vector<uint8_t>         local_active;
			evaluate(built, local_results, local_flat, local_active);
		} else {
			evaluate(built, results, flat, active);
		}
	}
	const auto elapsed = std::chrono::steady_clock::now() - start;

	Result result;
	result.calls       = iterations;
	result.ns_per_call = static_cast<double>(
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
    // The parked Nexus average out of the KYTY_DEBUG_SRT_STATS dump: 1.97 descriptor sources and
    // 8.5 descriptor dwords an event against 21.5 flat slots, so 13 slots are body-only.
    {.name           = "nexus-mix",
     .buffers        = 2,
     .images         = 0,
     .samplers       = 1,
     .arithmetic_ops = 1,
     .body_slots     = 13},
    // The same mix with no address arithmetic, which is what the dump's flat programs actually
    // are: 23.7 instructions an event of which 21.5 are reads, so the operands are a shared
    // user-data pair and the reads hang straight off it.
    {.name           = "nexus-flat",
     .buffers        = 2,
     .images         = 0,
     .samplers       = 1,
     .arithmetic_ops = 0,
     .body_slots     = 9},
};

} // namespace

namespace Common {

int DbgExitHandler(const char*, int, std::string_view) { std::abort(); }

int DbgExitHandler(const char*, int, fmt::text_style, std::string_view) { std::abort(); }

int DbgExitIfHandler(const char*, const char*, int) { return 1; }

void DbgExit(int) { std::abort(); }

} // namespace Common

int main(int argc, char** argv) {
	uint64_t    checksum   = 0;
	uint64_t    iterations = 200000;
	std::string only;
	bool        sweep  = false;
	bool        cold   = false;
	size_t      copies = 512;
	for (int index = 1; index < argc; index++) {
		const std::string arg = argv[index];
		if (arg.rfind("--iterations=", 0) == 0) {
			iterations = std::strtoull(arg.c_str() + 13, nullptr, 10);
		} else if (arg.rfind("--only=", 0) == 0) {
			only = arg.substr(7);
		} else if (arg.rfind("--copies=", 0) == 0) {
			copies = std::strtoull(arg.c_str() + 9, nullptr, 10);
		} else if (arg == "--sweep") {
			sweep = true;
		} else if (arg == "--cold") {
			cold = true;
		} else {
			std::fprintf(stderr, "usage: srt_evaluator_bench [--iterations=N] [--only=NAME] "
			                     "[--sweep] [--cold] [--copies=N]\n");
			return 2;
		}
	}

	if (cold) {
		// Where the per-call cost goes once the data is not in L1. Every row is the same
		// evaluation; only the locality of the plan and of the guest table changes. The flushed
		// columns subtract a baseline loop that performs the same flushes and no evaluation.
		std::printf("%-12s %7s %6s %6s %9s %9s %9s %9s %9s %9s %9s %9s\n", "shape", "copies",
		            "insts", "reads", "plan_KB", "cycle_MB", "hot_ns", "fresh_ns", "cycle_ns",
		            "coldplan", "coldguest", "coldboth");
		for (const auto& shape: Shapes) {
			if (!only.empty() && only != shape.name) {
				continue;
			}
			// One copy and one small table: everything stays in L1 between iterations.
			auto       one   = BuildColdSet(shape, 1, 4096);
			const auto hot   = MeasureCycle(one.plans, iterations, false, FlushNone);
			const auto fresh = MeasureCycle(one.plans, iterations, true, FlushNone);
			const auto insts = one.plans.front().plan.flat.insts.size();
			uint32_t   reads = 0;
			for (const auto& inst: one.plans.front().plan.flat.insts) {
				if (inst.op == SrtFlatOp::ReadAddress || inst.op == SrtFlatOp::ReadBuffer) {
					reads++;
				}
			}
			const auto plan_bytes = PlanBytes(one.plans.front());

			auto       many  = BuildColdSet(shape, copies, 4096);
			const auto cycle = MeasureCycle(many.plans, iterations, false, FlushNone);
			// A short cycle is enough once every line is flushed, and it keeps the flush cost
			// predictable; the baseline pays the same flushes without evaluating.
			auto       few        = BuildColdSet(shape, 64, 4096);
			const auto base_plan  = MeasureCycle(few.plans, iterations, false, FlushPlanOnly, true);
			const auto cold_plan  = MeasureCycle(few.plans, iterations, false, FlushPlanOnly);
			const auto base_guest = MeasureCycle(few.plans, iterations, false, FlushGuestOnly, true);
			const auto cold_guest = MeasureCycle(few.plans, iterations, false, FlushGuestOnly);
			const auto base_both  = MeasureCycle(few.plans, iterations, false, FlushBoth, true);
			const auto cold_both  = MeasureCycle(few.plans, iterations, false, FlushBoth);
			checksum += hot.checksum + fresh.checksum + cycle.checksum + cold_plan.checksum +
			            cold_guest.checksum + cold_both.checksum;
			std::printf("%-12s %7zu %6zu %6u %9.2f %9.2f %9.1f %9.1f %9.1f %9.1f %9.1f %9.1f\n",
			            shape.name, copies, insts, reads,
			            static_cast<double>(plan_bytes) / 1024.0,
			            static_cast<double>(copies) *
			                (static_cast<double>(plan_bytes) + 4096.0) / (1024.0 * 1024.0),
			            hot.ns_per_call, fresh.ns_per_call, cycle.ns_per_call,
			            cold_plan.ns_per_call - base_plan.ns_per_call,
			            cold_guest.ns_per_call - base_guest.ns_per_call,
			            cold_both.ns_per_call - base_both.ns_per_call);
			std::printf("%-12s flush-only baselines: plan %.1f ns, guest %.1f ns, both %.1f ns\n",
			            "", base_plan.ns_per_call, base_guest.ns_per_call, base_both.ns_per_call);
		}
		std::printf("checksum %llu\n", static_cast<unsigned long long>(checksum));
		return 0;
	}

	if (sweep) {
		// Varying the per-slot instruction count separates the fixed per-slot cost (the guest read
		// plus the memo and descriptor stores) from the marginal cost of one more IR instruction.
		std::printf("%-10s %12s %10s %12s %12s\n", "shape", "flat_slots", "ops/slot",
		            "walker_ns", "flat_ns");
		for (const uint32_t slots: {40u, 80u, 160u}) {
			for (uint32_t ops = 0; ops <= 6u; ops += 2u) {
				PlanShape shape {};
				shape.name           = "sweep";
				shape.buffers        = slots / 4u;
				shape.arithmetic_ops = ops;
				auto       built  = BuildPlan(shape);
				const auto walker = Measure(built, iterations, false);
				const auto flat   = Measure(built, iterations, true);
				Check(walker.checksum == flat.checksum, "flat program disagrees with walker");
				checksum += flat.checksum;
				std::printf("%-10s %12zu %10u %12.1f %12.1f\n", "sweep",
				            built.plan.srt_reads.size(), ops, walker.ns_per_call,
				            flat.ns_per_call);
			}
		}
		std::printf("checksum %llu\n", static_cast<unsigned long long>(checksum));
		return 0;
	}

	std::printf("%-18s %10s %12s %12s %12s %8s %12s %12s\n", "shape", "sources", "flat_slots",
	            "walker_ns", "flat_ns", "speedup", "body_skip_ns", "all_skip_ns");
	for (const auto& shape: Shapes) {
		if (!only.empty() && only != shape.name) {
			continue;
		}
		auto       built  = BuildPlan(shape);
		const auto walker = Measure(built, iterations, false);
		const auto flat   = Measure(built, iterations, true);
		Check(walker.checksum == flat.checksum, "flat program disagrees with walker");
		// What stage 1b can remove: the slots no descriptor source consumes, then every slot.
		const auto body_skip = Measure(built, iterations, true, built.body_mask);
		const auto all_skip  = Measure(built, iterations, true, built.all_mask);
		checksum += flat.checksum + body_skip.checksum + all_skip.checksum;
		std::printf("%-18s %10zu %12zu %12.1f %12.1f %7.2fx %12.1f %12.1f\n", shape.name,
		            built.sources.size(), built.plan.srt_reads.size(), walker.ns_per_call,
		            flat.ns_per_call, walker.ns_per_call / flat.ns_per_call,
		            body_skip.ns_per_call, all_skip.ns_per_call);
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
