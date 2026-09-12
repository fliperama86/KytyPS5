#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "SrtSyntheticPlans.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ResourceMaterializationTests: failed: %s\n", text);
    std::abort();
  }
}

bool RejectSpecializationRead(void *userdata, uint64_t, uint32_t *) {
  ++*static_cast<uint32_t *>(userdata);
  return false;
}

Libs::Graphics::ShaderRecompiler::IR::Block &
AddValueBlock(Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto block = std::make_unique<Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  program.block_info.push_back({.id = 0});
  program.block_storage.push_back(std::move(block));
  return *result;
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan SrtPlan(uint64_t address) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  program.memory_info.push_back(memory);
  const auto low = Value(static_cast<uint32_t>(address));
  const auto high = Value(static_cast<uint32_t>(address >> 32u));
  auto &handle =
      value_block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high});
  auto &raw = value_block.AppendNewInst(
      ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});

  auto &srt = value_block.AppendNewInst(ValueOpcode::GetSrtResource);
  auto &flat = value_block.AppendNewInst(ValueOpcode::ReadConst,
                                         {Value(&srt), Value(0u)});
  DescriptorSource source;
  source.dwords[0] = Value(&flat);
  source.dwords[1] = Value(0u);
  source.dword_count = 2;
  program.descriptor_sources.push_back(source);
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UnbasedFlatPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);
  program.info.uses_dma = true;
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UserDataBufferPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  auto &user_data = value_block.AppendNewInst(
      ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  DescriptorSource source;
  source.dwords[0] = Value(&user_data);
  source.dwords[1] = Value(0u);
  source.dwords[2] = Value(0u);
  source.dwords[3] = Value(0u);
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MixedSamplerPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);

  const auto AddSource = [&program](uint32_t dword_count, uint32_t first) {
    DescriptorSource source;
    source.dword_count = dword_count;
    source.dwords[0] = Value(first);
    for (uint32_t i = 1; i < dword_count; i++) {
      source.dwords[i] = Value(0u);
    }
    program.descriptor_sources.push_back(source);
    return static_cast<uint32_t>(program.descriptor_sources.size() - 1u);
  };

  const auto image0 = AddSource(8, 0);
  const auto image1 = AddSource(8, 0);
  const auto sampler0 = AddSource(4, 0x11111111u);
  const auto sampler1 = AddSource(4, 0x22222222u);
  program.info.images.push_back(
      {.source = image0,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.images.push_back(
      {.source = image1,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D,
       .conversion_format =
           Libs::Graphics::Prospero::BufferFormat::k8_8_8_8UNorm});
  program.info.samplers.push_back({.source = sampler0});
  program.info.samplers.push_back({.source = sampler1});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 0});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 1});
  program.info.sampled_pairs.push_back({.image = 1, .sampler = 1});
  return ExtractResourcePlan(program);
}

void TestMappedSrtUsesDirectReaderByDefault() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const uint32_t dword = 0x12345678;
  auto plan = SrtPlan(reinterpret_cast<uint64_t>(&dword));
  uint32_t specialization_reads = 0;
  const SrtRuntime runtime{.userdata = &specialization_reads,
                           .read_specialization_memory =
                               RejectSpecializationRead};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mapped SRT stage materialization failed");
  Check(specialization_reads == 0,
        "ordinary SRT read used the specialization reader");
  Check(snapshot.flattened_srt.size() == 1 &&
            snapshot.flattened_srt[0] == dword,
        "cache rematerialization did not use the direct reader by default");
}

void TestIntegerRuntimeValueFollowsSrtReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = SrtPlan(0x10000);
  const auto root = plan.descriptor_sources.front().dwords[0];
  Check(ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT read was rejected");

  Block values;
  auto &comparison = values.AppendNewInst(ValueOpcode::FPOrdLessThanEqual32,
                                          {Value::F32(1.f), Value::F32(0.f)});
  auto &selection = values.AppendNewInst(
      ValueOpcode::SelectU32, {Value(&comparison), Value(1u), Value(0u)});
  plan.srt_reads[0].value = Value(&selection);
  Check(ValidateRuntimeValue(plan, root),
        "ordinary SRT validation rejected a floating-point dependency");
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT validation missed a hidden floating-point dependency");

  auto &first =
      values.AppendNewInst(ValueOpcode::ReadFirstLane, {root, Value(true)});
  Check(!ValidateRuntimeValue(plan, Value(&first), RuntimeValueType::Integer),
        "read-first-lane lost integer-only SRT validation");

  auto &active = values.AppendNewInst(ValueOpcode::ReadFirstLane,
                                      {Value(&selection), Value(&comparison)});
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point execution mask was accepted as integer-only");

  auto &lane = values.AppendNewInst(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)),
       Value(0u)});
  auto &mask =
      values.AppendNewInst(ValueOpcode::INotEqual32, {Value(&lane), Value(0u)});
  selection.SetArg(0, Value(&mask));
  active.SetArg(1, Value(&mask));
  Check(ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "nonuniform integer execution mask was rejected");
  auto &float_value =
      values.AppendNewInst(ValueOpcode::BitCastU32F32, {Value::F32(1.f)});
  selection.SetArg(2, Value(&float_value));
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point inactive arm was accepted as integer-only");

  plan.srt_reads[0].value = Value(&first);
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "cyclic SRT read-first-lane dependency was accepted");
}

void TestUnbasedFlatCacheHitMaterializes() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UnbasedFlatPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "unbased FLAT stage materialization failed");
  Check(snapshot.buffers.empty() && snapshot.images.empty(),
        "unbased FLAT plan produced unexpected descriptors");
}

void TestFailedMaterializationPreservesPriorStage() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UserDataBufferPlan();
  ResourceSnapshot snapshot;
  snapshot.user_data.push_back(0xfeedbeefu);
  ResourceSpecialization specialization;
  specialization.buffers.push_back({.packed_stride = 7});
  Check(!MaterializeResources(plan, {}, snapshot, specialization),
        "missing runtime user data did not reject the cached stage");
  Check(snapshot.user_data == std::vector<uint32_t>{0xfeedbeefu} &&
            specialization.buffers.size() == 1 &&
            specialization.buffers[0].packed_stride == 7,
        "failed cache materialization changed its destinations");
}

void TestMixedSamplerDuplicatesTheCorrectSnapshot() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = MixedSamplerPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "mixed sampler materialization failed");
  Check(snapshot.samplers.size() == 3,
        "mixed sampler materialization appended unrelated samplers");
  Check(snapshot.samplers[2] == snapshot.samplers[1] &&
            snapshot.samplers[2] != snapshot.samplers[0],
        "point sampler variant duplicated the wrong runtime descriptor");
}

// The packed execution form of the flat program against the form it is built from, over the
// shared synthetic plans (docs/gpu-descriptor-fetch.md, "Packed flat program"). Every descriptor
// source and every flat SRT read of those plans, valid and invalid, has to produce the same value
// and the same validity through all three: packed, the compile-time schedule, and the IR walker.
namespace PackedParity {

using namespace Libs::Graphics::ShaderRecompiler::IR;

enum class Form { Packed, Schedule, Walker };

const char *FormName(Form form) {
  switch (form) {
  case Form::Packed:
    return "packed";
  case Form::Schedule:
    return "schedule";
  default:
    return "walker";
  }
}

void Select(ResourcePlan &plan, Form form) {
  plan.flat.compiled = form != Form::Walker;
  plan.flat.packed.compiled = form == Form::Packed;
}

struct Evaluation {
  bool ok = false;
  std::vector<DescriptorValue> results;
  std::vector<uint32_t> flat;
  std::vector<uint8_t> active;

  bool operator==(const Evaluation &other) const {
    return ok == other.ok &&
           (!ok || (results == other.results && flat == other.flat &&
                    active == other.active));
  }
};

Evaluation Run(ResourcePlan &plan, Form form, const SrtRuntime &runtime,
               std::span<const uint32_t> sources,
               std::span<const uint8_t> skip_sources,
               std::span<const uint8_t> skip_slots) {
  Select(plan, form);
  Evaluation out;
  out.ok = EvaluateRuntimeSources(plan, sources, runtime, out.results, out.flat,
                                  plan.clean_flat_slots, out.active,
                                  skip_sources, skip_slots);
  Select(plan, Form::Packed);
  return out;
}

// Runs one call three ways and reports what the packed form disagrees with.
void CheckAllForms(ResourcePlan &plan, const SrtRuntime &runtime,
                   std::span<const uint32_t> sources,
                   std::span<const uint8_t> skip_sources,
                   std::span<const uint8_t> skip_slots, const std::string &what) {
  const auto packed = Run(plan, Form::Packed, runtime, sources, skip_sources, skip_slots);
  for (const auto form : {Form::Schedule, Form::Walker}) {
    const auto other = Run(plan, form, runtime, sources, skip_sources, skip_slots);
    Check(packed == other,
          (what + ": the packed program disagrees with the " + FormName(form)).c_str());
  }
}

void TestPackedProgramMatchesTheCompileTimeForm() {
  auto built = SrtSynthetic::BuildSyntheticPlan();
  auto &plan = built.plan;
  Check(plan.flat.compiled, "CompileSrtPlan did not lower the synthetic plan");
  Check(plan.flat.packed.compiled, "CompileSrtPlan did not pack the synthetic plan");
  Check(plan.flat.packed.source_count == plan.descriptor_sources.size() &&
            plan.flat.packed.read_count == plan.srt_reads.size(),
        "the packed program has the wrong root counts");
  const SrtRuntime runtime{
      .user_data = built.user_data,
      .shader_base = SrtSynthetic::ShaderBase,
      .read_memory = SrtSynthetic::ReadGuestMemory,
      .userdata = &built.backing,
      .read_specialization_memory = SrtSynthetic::ReadGuestMemory,
  };

  // One source at a time, which is the path with no flat reads at all.
  std::vector<uint32_t> valid_sources;
  for (uint32_t index = 0; index < plan.descriptor_sources.size(); index++) {
    DescriptorValue values[3];
    bool ok[3] = {};
    uint32_t form_index = 0;
    for (const auto form : {Form::Packed, Form::Schedule, Form::Walker}) {
      Select(plan, form);
      ok[form_index] = EvaluateDescriptorSource(plan, index, runtime, values[form_index]);
      form_index++;
    }
    Select(plan, Form::Packed);
    Check(ok[0] == ok[1] && (!ok[0] || values[0] == values[1]),
          (built.case_names[index] + ": packed disagrees with the schedule").c_str());
    Check(ok[0] == ok[2] && (!ok[0] || values[0] == values[2]),
          (built.case_names[index] + ": packed disagrees with the walker").c_str());
    if (ok[0]) {
      valid_sources.push_back(index);
    }
  }
  Check(!valid_sources.empty() && valid_sources.size() < plan.descriptor_sources.size(),
        "the synthetic plan must keep both valid and failing descriptor sources");

  std::vector<uint32_t> all_sources(plan.descriptor_sources.size());
  for (uint32_t index = 0; index < all_sources.size(); index++) {
    all_sources[index] = index;
  }
  const std::vector<uint8_t> no_skip;
  const std::vector<uint8_t> skip_all_slots(plan.srt_reads.size(), 1u);

  // The renderer's call: every source it asks for, the two invalid slots owned by the shader.
  CheckAllForms(plan, runtime, valid_sources, no_skip, built.read_invalid, "valid sources");
  // The same call over every source, which must fail in all three forms.
  CheckAllForms(plan, runtime, all_sources, no_skip, built.read_invalid, "all sources");
  // Sources only, and slots only.
  CheckAllForms(plan, runtime, valid_sources, no_skip, skip_all_slots, "slots skipped");
  CheckAllForms(plan, runtime, {}, no_skip, built.read_invalid, "slots only");
  // A failing root reached through skip_sources, which leaves its descriptor zero.
  {
    const std::vector<uint8_t> skip_every(plan.descriptor_sources.size(), 1u);
    CheckAllForms(plan, runtime, all_sources, skip_every, built.read_invalid,
                  "every source skipped");
  }
  // One invalid slot at a time: the whole call fails, the same way in every form.
  for (uint32_t slot = 0; slot < built.read_invalid.size(); slot++) {
    if (built.read_invalid[slot] == 0u) {
      continue;
    }
    std::vector<uint8_t> only(built.read_invalid.size(), 1u);
    only[slot] = 0u;
    CheckAllForms(plan, runtime, valid_sources, no_skip, only,
                  built.read_names[slot] + " alone");
  }
  // Every valid slot on its own, so one bad root cannot mask another.
  for (uint32_t slot = 0; slot < built.read_invalid.size(); slot++) {
    if (built.read_invalid[slot] != 0u) {
      continue;
    }
    std::vector<uint8_t> only(built.read_invalid.size(), 1u);
    only[slot] = 0u;
    CheckAllForms(plan, runtime, valid_sources, no_skip, only, built.read_names[slot]);
  }

  // A clean slot keeps the compile-time form inside the packed evaluation: its root reads through
  // the specialization reader, and every normal-context ReadConst of that slot is routed to it.
  for (uint32_t slot = 0; slot < plan.clean_flat_slots.size(); slot++) {
    if (built.read_invalid[slot] != 0u) {
      continue;
    }
    plan.clean_flat_slots.assign(plan.srt_reads.size(), 0u);
    plan.clean_flat_slots[slot] = 1u;
    CompileSrtPlan(plan);
    Check(plan.flat.packed.compiled, "a clean slot stopped the plan from packing");
    CheckAllForms(plan, runtime, valid_sources, no_skip, built.read_invalid,
                  "clean slot " + built.read_names[slot]);
  }
  plan.clean_flat_slots.assign(plan.srt_reads.size(), 0u);
  CompileSrtPlan(plan);

  // Every slot valid, nothing skipped: the one shape where the packed program evaluates the flat
  // reads itself, so the values it writes into the flat buffer are compared root by root.
  {
    const uint32_t good = built.read_invalid[0] == 0u ? 0u : 1u;
    Check(built.read_invalid[good] == 0u, "the synthetic plan has no valid read slot");
    for (uint32_t slot = 0; slot < built.read_invalid.size(); slot++) {
      if (built.read_invalid[slot] != 0u) {
        plan.srt_reads[slot].value = plan.srt_reads[good].value;
      }
    }
    CompileSrtPlan(plan);
    const std::vector<uint8_t> nothing_skipped(plan.srt_reads.size(), 0u);
    CheckAllForms(plan, runtime, valid_sources, no_skip, nothing_skipped, "no slot skipped");
    CheckAllForms(plan, runtime, valid_sources, no_skip, {}, "no slot mask at all");
    const auto evaluated = Run(plan, Form::Packed, runtime, valid_sources, no_skip, {});
    Check(evaluated.ok && evaluated.flat.size() == plan.srt_reads.size(),
          "the packed program failed a plan whose slots are all valid");
  }

  // Resource control flow is not packed: those plans must fall back to the compile-time form.
  {
    ResourceBlock entry;
    entry.successors = {0u};
    plan.control_flow.push_back(entry);
    CompileSrtPlan(plan);
    Check(plan.flat.compiled && !plan.flat.packed.compiled,
          "a plan with resource control flow was packed");
    plan.control_flow.clear();
    CompileSrtPlan(plan);
  }
}

} // namespace PackedParity

} // namespace

namespace Common {

int DbgExitHandler(const char *, int, std::string_view) { std::abort(); }

int DbgExitHandler(const char *, int, fmt::text_style, std::string_view) {
  std::abort();
}

int DbgExitIfHandler(const char *, const char *, int) { return 1; }

void DbgExit(int) { std::abort(); }

} // namespace Common

int main() {
  TestMappedSrtUsesDirectReaderByDefault();
  TestIntegerRuntimeValueFollowsSrtReads();
  TestUnbasedFlatCacheHitMaterializes();
  TestFailedMaterializationPreservesPriorStage();
  TestMixedSamplerDuplicatesTheCorrectSnapshot();
  PackedParity::TestPackedProgramMatchesTheCompileTimeForm();
  std::puts("ResourceMaterializationTests: all cases passed");
  return 0;
}

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
