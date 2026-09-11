#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

class Value;

using SrtMemoryReader = bool (*)(void* userdata, uint64_t address, uint32_t* value);
using SrtMemorySync   = bool (*)(void* userdata, uint64_t address, uint64_t size);

struct SrtRuntime {
	std::span<const uint32_t> user_data;
	uint64_t                  shader_base                = 0;
	SrtMemoryReader           read_memory                = nullptr;
	void*                     userdata                   = nullptr;
	SrtMemoryReader           read_specialization_memory = nullptr;
	SrtMemorySync             sync_memory                = nullptr;
};

enum class RuntimeValueType { Any, Integer };

// Diagnostic record of the last flat-program evaluation that failed on this thread. Written only
// on the failure path, so a successful evaluation pays nothing. Stage 1 of the GPU-side descriptor
// fetch (docs/gpu-descriptor-fetch.md) turns a failure here into a fatal materialization error, and
// the renderer needs the address the walk tried to read to say what happened to that page.
// One scheduled instruction of the failing root, with the value it produced and, for a memory op,
// the address it read. Enough to see where a zero descriptor came from.
struct SrtTraceEntry {
	uint32_t    inst_index = 0;
	const char* op_name    = "";
	uint32_t    arg_count  = 0;
	std::array<uint32_t, 5> args {};
	std::array<uint64_t, 5> arg_values {};
	std::array<uint8_t, 5>  arg_defined {};
	uint64_t                imm     = 0;
	bool                    clean   = false;
	bool                    defined = false;
	uint64_t                value   = 0;
	bool                    memory  = false;
	bool                    address_valid = false;
	uint64_t                address       = 0;
	bool                    failing       = false;
};

struct SrtFailure {
	bool valid = false;
	// false when the IR walker produced the failure: only the source index is meaningful then.
	bool flat = false;
	// "source", "flat-read", "condition" or "uniform-value".
	const char* kind = "";
	// Descriptor source index for "source", srt_reads index for "flat-read", block index for
	// "condition", word index for "uniform-value".
	uint32_t index      = UINT32_MAX;
	uint32_t root_first = 0;
	uint32_t root_count = 0;
	bool     root_valid = false;

	uint32_t    inst_index = UINT32_MAX;
	const char* op_name    = "";
	uint32_t    arg_count  = 0;
	std::array<uint32_t, 5> args {};
	std::array<uint64_t, 5> arg_values {};
	std::array<uint8_t, 5>  arg_defined {};
	uint64_t                imm   = 0;
	bool                    clean = false;

	// The address the failing instruction read or would have read.
	bool     address_valid = false;
	uint64_t address       = 0;
	// Decoded operands of a ReadBuffer / ReadAddress failure.
	bool     memory_op   = false;
	uint64_t base        = 0;
	uint64_t byte_offset = 0;
	uint64_t records     = 0;
	uint64_t stride      = 0;
	uint64_t bound       = 0;

	// (register index relative to user_data_base, value) for every UserData op the failing root
	// schedules, in schedule order.
	std::vector<std::pair<uint32_t, uint32_t>> user_data;
	uint32_t                                   user_data_size = 0;

	// The failing root's whole schedule, in execution order.
	std::vector<SrtTraceEntry> trace;
};

[[nodiscard]] const SrtFailure& LastSrtFailure();
[[nodiscard]] std::string       FormatSrtFailure(const SrtFailure& failure);

// Collects reachable ReadConst values. Immediate offsets receive compact flat-buffer slots;
// dynamic offsets remain explicit and are never assigned a fake slot.
void BuildSrtPlan(Program& program);
bool ValidateRuntimeValue(const ResourcePlan& program, Value value,
                          RuntimeValueType type = RuntimeValueType::Any,
                          std::string* reason = nullptr);
bool EvaluateUniformValues(const ResourcePlan& program, std::span<const Value> values,
                            const SrtRuntime& runtime, std::span<uint32_t> results);

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result);

// Evaluates one runtime snapshot transactionally. Scalar values and ReadConst results shared by
// several descriptors are memoized once across the batch.
bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results);

// Evaluates potentially reachable descriptor sources and the flattened immediate SRT with one
// memoized scalar walk. Inactive descriptors are zero; on failure no destination is changed.
bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::span<const uint8_t> clean_flat_slots,
                            std::vector<uint8_t>& active_sources,
                            std::span<const uint8_t> skip_sources = {});

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime,
             std::vector<uint32_t>& flat);

// Lowers the plan's runtime values into ResourcePlan::flat. Call once the plan is final: every
// descriptor source, SRT read, control-flow condition and uniform-fill value is compiled against
// the plan's current shape, and evaluation falls back to the IR walker if that shape changes.
void CompileSrtPlan(ResourcePlan& program);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SRTWALKER_H_ */
