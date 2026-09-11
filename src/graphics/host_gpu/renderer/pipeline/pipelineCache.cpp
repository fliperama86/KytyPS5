#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/cache/descriptorFeedback.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/image/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/srtStats.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"
#include "kernel/memory.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <span>
#include <spirv-tools/libspirv.hpp>
#include <cstdlib>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

// Upper-bound experiment for docs/gpu-descriptor-fetch.md stage 1, temporary. With
// KYTY_DEBUG_GPU_FETCH_STUB=1 and --gpu-descriptors true, every read-only buffer with a valid flat
// root is marked gpu_fetch without any shader support: the render thread skips its evaluation
// and binding, the shader reads the dummy buffer and the picture is wrong. Measures the CPU
// ceiling of the stage before the emitter side lands.
bool GpuFetchStubEnabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DEBUG_GPU_FETCH_STUB");
		return value != nullptr && value[0] == '1';
	}();
	return enabled && Config::GpuDescriptorsEnabled();
}

void MarkGpuFetchStub(ShaderRecompiler::IR::ResourcePlan& plan) {
	// Compute stays on the CPU path: a compute plan failed to materialize with its buffer roots
	// skipped, and materialization failure is fatal.
	if (!GpuFetchStubEnabled() || plan.stage == ShaderType::Compute) {
		return;
	}
	bool any = false;
	for (auto& buffer: plan.info.buffers) {
		if (!buffer.read || buffer.written || buffer.atomic ||
		    buffer.image_alias != ShaderRecompiler::IR::BufferResource::NoImageAlias) {
			continue;
		}
		if (buffer.source >= plan.flat.sources.size() || !plan.flat.sources[buffer.source].valid) {
			continue;
		}
		buffer.gpu_fetch = true;
		any              = true;
	}
	plan.info.gpu_descriptors = plan.info.gpu_descriptors || any;
}

void CopyGpuFetchStub(const ShaderRecompiler::IR::ShaderInfo& from,
                      ShaderRecompiler::IR::ShaderInfo&       to) {
	if (!GpuFetchStubEnabled() || from.buffers.size() != to.buffers.size()) {
		return;
	}
	for (size_t index = 0; index < from.buffers.size(); index++) {
		to.buffers[index].gpu_fetch = from.buffers[index].gpu_fetch;
	}
	to.gpu_descriptors = from.gpu_descriptors;
}

} // namespace


namespace {

vk::PolygonMode ResolvePolygonMode(const HW::ModeControl& mode, bool cull_front, bool cull_back) {
	// CxPrimitiveSetup::PolygonMode disables both per-face modes when it is zero.
	if (mode.poly_mode == 0) {
		return vk::PolygonMode::eFill;
	}
	EXIT_NOT_IMPLEMENTED(mode.poly_mode != 1);
	if (cull_front && cull_back) {
		return vk::PolygonMode::eFill;
	}
	if (!cull_front && !cull_back && mode.polymode_front_ptype != mode.polymode_back_ptype) {
		EXIT("Pipeline: different polygon modes for two visible faces are unsupported\n");
	}
	// Vulkan has one polygon mode. A culled face does not constrain that mode.
	const auto polygon_mode = cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
	switch (polygon_mode) {
		case 0: return vk::PolygonMode::ePoint;
		case 1: return vk::PolygonMode::eLine;
		case 2: return vk::PolygonMode::eFill;
		default: EXIT("Pipeline: invalid polygon mode %u\n", polygon_mode);
	}
}

// The emulator revision is deliberately absent: the driver cache is content-addressed by the
// driver, so entries produced by an older shader translator are simply never hit again. They cost
// a little file size until the driver evicts them, but they never need to invalidate the file.
std::string DriverCacheSignature(const vk::PhysicalDeviceProperties& properties) {
	constexpr char hex[] = "0123456789abcdef";
	std::string    uuid(VK_UUID_SIZE * 2, '0');
	for (size_t i = 0; i < VK_UUID_SIZE; i++) {
		uuid[i * 2]     = hex[properties.pipelineCacheUUID[i] >> 4u];
		uuid[i * 2 + 1] = hex[properties.pipelineCacheUUID[i] & 0xfu];
	}
	return fmt::format("KytyPC2:{:08x}:{:08x}:{:08x}:{}\n", properties.vendorID,
	                   properties.deviceID, properties.driverVersion, uuid);
}

// A save is considered after this much wall time, and only when new pipelines were compiled.
constexpr std::chrono::steady_clock::duration DriverCacheSaveInterval = std::chrono::seconds(20);

std::string PipelineCacheTitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

template <typename... Args>
void PipelineCacheLog(fmt::format_string<Args...> format, Args&&... args) {
	auto message = fmt::format(format, std::forward<Args>(args)...);
	message += '\n';
	if (Log::GetDirection() != Log::Direction::Console) {
		std::fwrite(message.data(), 1, message.size(), stdout);
		std::fflush(stdout);
	}
	Log::Write(message);
	Log::Flush();
}

bool ReadShaderGuestMemory(void*, uint64_t address, uint32_t* value) {
	return value != nullptr &&
	       Libs::LibKernel::Memory::TryReadGpuCleanBacking(address, value, sizeof(*value));
}

bool SyncShaderGuestMemory(void*, uint64_t address, uint64_t size) {
	return Libs::LibKernel::Memory::SyncGpuCleanBacking(address, size);
}

void ReportMaterialization(const char* label, ShaderType stage, uint64_t hash,
                           const ShaderRecompiler::IR::MaterializeReport& report, bool ok) {
	if (!ok) {
		EXIT("shader resource materialization failed: stage=%u hash=0x%016" PRIx64 " reason=%s\n",
		     static_cast<uint32_t>(stage), hash, report.reason.c_str());
	}
	if (!report.dropped_summary.empty()) {
		LOGF("%s indirect image tables: hash=0x%016" PRIx64 " dropped=%" PRIu32 " shapes=%" PRIu32
		     "%s\n",
		     label, hash, report.dropped_candidates, report.dropped_shapes,
		     report.dropped_summary.c_str());
	}
}

void DumpShaderSpirv(const char* stage_name, uint64_t shader_hash,
                     const std::vector<uint32_t>& spirv) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	static std::atomic_int id = 0;
	const auto path = Config::GetShaderLogFolder() / fmt::format("{:04d}_new_shader_{}_{:016x}.spv",
	                                                             id++, stage_name, shader_hash);
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		const auto path_text = Common::PathToString(path);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		return;
	}
	file.Write(spirv.data(), spirv.size() * sizeof(uint32_t));
}

void DumpShaderOriginal(const char* stage_name, uint64_t shader_hash,
                        std::span<const uint32_t> code, const std::string& decoded_dump) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}
	EXIT_IF(code.empty());
	static std::atomic_int id = 0;
	const auto base = Config::GetShaderLogFolder() / "original" /
	                  fmt::format("{:04d}_new_shader_{}_{:016x}", id++, stage_name, shader_hash);
	Common::File::CreateDirectories(base.parent_path());
	for (const auto& [suffix, data, size]: {
	         std::tuple {".bin", static_cast<const void*>(code.data()), code.size_bytes()},
	         std::tuple {".rdna2", static_cast<const void*>(decoded_dump.data()),
	                     decoded_dump.size()},
	     }) {
		if (size == 0) {
			continue;
		}
		auto path = base;
		path += suffix;
		Common::File file(path);
		if (file.IsInvalid()) {
			const auto path_text = Common::PathToString(path);
			LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", path_text.c_str());
		} else {
			file.Write(data, size);
		}
	}
}

bool ValidateShaderSpirv(const char* label, uint64_t shader_hash,
                         const std::vector<uint32_t>& spirv) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}
	spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	tools.SetMessageConsumer([&messages](spv_message_level_t, const char*,
	                                     const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});
	if (tools.Validate(spirv)) {
		return true;
	}
	spvtools::SpirvTools disassembler(SPV_ENV_VULKAN_1_2);
	std::string          text;
	disassembler.Disassemble(spirv, &text,
	                         static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
	                             static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR));
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", text.c_str());
	return false;
}

// More than this many descriptor-feedback mismatches for one program in a session and speculating
// on its buffer layout has stopped paying off (docs/gpu-descriptor-fetch.md, stage 1).
constexpr uint32_t MaxFeedbackMismatches = 8;

// KYTY_DEBUG_SRT_STATS support. Nothing below runs unless the collector is on.
constexpr uint64_t StatsMix(uint64_t seed, uint64_t value) {
	return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u));
}

// Identity of the specialized buffer layout: the tuple, over every buffer resource, of the
// packed stride, descriptor format and descriptor swizzle a GPU-side fetch would have to predict.
uint64_t StatsBufferLayoutKey(const ShaderRecompiler::IR::ResourceSpecialization& specialization) {
	uint64_t key = StatsMix(0xcbf29ce484222325ull, specialization.buffers.size());
	for (const auto& buffer: specialization.buffers) {
		key = StatsMix(key, buffer.packed_stride);
		key = StatsMix(key, static_cast<uint64_t>(buffer.descriptor_format));
		key = StatsMix(key, buffer.descriptor_swizzle);
	}
	return key;
}

// Identity of the decoded vertex-fetch layout, matching the per-resource words that
// BuildStageStaticKey puts into the program cache key.
uint64_t StatsVertexLayoutKey(const ShaderVertexInputInfo& info) {
	uint64_t key = StatsMix(0xcbf29ce484222325ull, static_cast<uint64_t>(info.resources_num));
	for (int i = 0; i < info.resources_num; i++) {
		const auto& resource    = info.resources[i];
		const auto& destination = info.resources_dst[i];
		for (const uint32_t word:
		     {static_cast<uint32_t>(destination.register_start),
		      static_cast<uint32_t>(destination.registers_num),
		      static_cast<uint32_t>(destination.fetch_index),
		      static_cast<uint32_t>(destination.attr_id), static_cast<uint32_t>(resource.Stride()),
		      static_cast<uint32_t>(resource.SwizzleEnabled()),
		      static_cast<uint32_t>(resource.DstSelXYZW()),
		      static_cast<uint32_t>(resource.RawFormat()),
		      static_cast<uint32_t>(resource.OutOfBounds()),
		      static_cast<uint32_t>(resource.AddTid())}) {
			key = StatsMix(key, word);
		}
	}
	return key;
}

SrtStats::StageEvent StatsStageEvent(const char*                                         stage_name,
                                     const ShaderRecompiler::IR::ResourcePlan&           plan,
                                     const ShaderRecompiler::IR::BindingLayout&          bindings,
                                     const ShaderRecompiler::IR::ResourceSpecialization& spec) {
	SrtStats::StageEvent event;
	event.stage_name            = stage_name;
	event.shader_hash           = plan.shader_hash;
	event.descriptor_sources    = static_cast<uint32_t>(plan.descriptor_sources.size());
	event.buffer_sources        = static_cast<uint32_t>(plan.info.buffers.size());
	event.scalar_buffer_sources = static_cast<uint32_t>(std::ranges::count_if(
	    plan.info.buffers, [](const ShaderRecompiler::IR::BufferResource& b) { return b.scalar; }));
	event.image_sources         = static_cast<uint32_t>(plan.info.images.size());
	event.sampler_sources       = static_cast<uint32_t>(plan.info.samplers.size());
	for (const auto& image: plan.info.images) {
		if (image.source < plan.descriptor_sources.size() &&
		    plan.descriptor_sources[image.source].indirect_image.has_value()) {
			event.indirect_image_sources++;
		}
	}
	event.flat_insts = static_cast<uint32_t>(plan.flat.insts.size());
	for (const auto& inst: plan.flat.insts) {
		if (inst.op == ShaderRecompiler::IR::SrtFlatOp::ReadAddress ||
		    inst.op == ShaderRecompiler::IR::SrtFlatOp::ReadBuffer) {
			event.flat_reads++;
		}
	}
	event.buffer_layout_key = StatsBufferLayoutKey(spec);
	event.uses_dma          = plan.info.uses_dma;
	// A stage whose user data did not fit the 32-dword push block reaches its offsets through a
	// ShaderData descriptor instead.
	event.push_overflow = bindings.ShaderDataDwords() != 0 && !bindings.UsesPushData();
	return event;
}

} // namespace

struct PipelineCache::ProgramCache {
	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		uint32_t              code_size       = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	struct Permutation {
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		ShaderRecompiler::IR::CompiledShaderInfo     program;
		ShaderProgram                                handle;
	};

	struct SourceEntry {
		explicit SourceEntry(ShaderRecompiler::IR::ResourcePlan plan)
		    : resource_plan(std::move(plan)) {
			permutations.reserve(8);
		}

		// Takes the GPU-fetch shape from a freshly compiled module. Every permutation of an entry
		// shares it: gpu_fetch follows the resource plan, not the specialization.
		void AdoptGpuFetch(const ShaderRecompiler::IR::ShaderInfo& info) {
			if (!info.gpu_descriptors) {
				return;
			}
			gpu_descriptors = true;
			gpu_fetch_buffers.assign(info.buffers.size(), 0u);
			for (uint32_t i = 0; i < info.buffers.size(); i++) {
				gpu_fetch_buffers[i] = info.buffers[i].gpu_fetch ? 1u : 0u;
			}
		}

		ShaderRecompiler::IR::ResourcePlan resource_plan;
		std::vector<Permutation>           permutations;

		// GPU-side descriptor fetch state (docs/gpu-descriptor-fetch.md, stage 1). All of it is
		// inert while the plan marks no buffer gpu_fetch, which is every plan with the setting off.
		std::vector<uint8_t>                        gpu_fetch_buffers;
		ShaderRecompiler::IR::ResourceSpecialization last_specialization;
		uint32_t feedback_slot = ShaderRecompiler::IR::BindingLayout::NoFeedbackSlot;
		uint32_t mismatches    = 0;
		bool     gpu_descriptors         = false;
		bool     has_last_specialization = false;
		// The next draw of this program materializes on the CPU: a shader reported that a runtime
		// V# no longer matches what it was specialized against.
		bool cpu_next = false;
		// Too many mismatches for speculation to pay off. The program stays on the CPU path for
		// the rest of the session; the shader still fetches its own buffers.
		bool pinned_cpu = false;
	};

	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const {
			std::size_t hash = static_cast<std::size_t>(key.stage);
			PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash));
			if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
				PipelineKeyHash::Mix(hash, static_cast<std::size_t>(key.hash >> 32u));
			}
			PipelineKeyHash::Mix(hash, key.user_data_count);
			PipelineKeyHash::Mix(hash, key.code_size);
			PipelineKeyHash::Mix(hash, key.static_state.size());
			// Bucket same-shape static variants by source. ProgramKey equality performs the one
			// exact state comparison needed on a stable hit without hashing up to 429 words first.
			return hash;
		}
	};

	static constexpr std::size_t MaxStaticKeyWords = 13 + ShaderVertexInputInfo::RES_MAX * 13;

	Permutation CompilePermutation(const ShaderParams&                          params,
	                               const ShaderRecompiler::CompileOptions&      options,
	                               ShaderRecompiler::TranslateResult            translated,
	                               ShaderRecompiler::IR::ResourceSpecialization specialization,
	                               uint32_t push_data_start_dword) {
		const char* stage_name = nullptr;
		switch (options.stage) {
			case ShaderType::Vertex: stage_name = "vs"; break;
			case ShaderType::Mesh: stage_name = "ms"; break;
			case ShaderType::Pixel: stage_name = "ps"; break;
			case ShaderType::Compute: stage_name = "cs"; break;
			default: EXIT("invalid pipeline shader stage\n");
		}
		auto result = ShaderRecompiler::CompileProgram(std::move(translated), options,
		                                               specialization, push_data_start_dword);
		DumpShaderOriginal(stage_name, options.shader_hash, params.code, result.decoded_dump);
		if (!ValidateShaderSpirv(options.dump_label, options.shader_hash, result.spirv)) {
			DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);
			EXIT("%s failed hash=0x%016" PRIx64 ": SPIR-V validation failed\n", options.dump_label,
			     options.shader_hash);
		}
		DumpShaderSpirv(stage_name, options.shader_hash, result.spirv);

		vk::ShaderModuleCreateInfo create_info {};
		create_info.codeSize    = result.spirv.size() * sizeof(uint32_t);
		create_info.pCode       = result.spirv.data();
		vk::ShaderModule module = nullptr;
		RequireVulkanSuccess(device.createShaderModule(&create_info, nullptr, &module),
		                     "create recompiled shader module");
		EXIT_IF(module == nullptr);
		SetVulkanObjectNameF(device, module, "Kyty.Shader.{}[0x{:016x}]", stage_name,
		                     options.shader_hash);
		if (options.dump_ir) {
			if (!options.early_dump) {
				LOGF("%s decoded RDNA2:\n%s", options.dump_label, result.decoded_dump.c_str());
				LOGF("%s IR:\n%s", options.dump_label, result.ir_dump.c_str());
			}
			LOGF("%s SPIR-V words=%" PRIu64 " wave_size=%u\n", options.dump_label,
			     static_cast<uint64_t>(result.spirv.size()), options.wave_size);
		}
		return {
		    .specialization = std::move(specialization),
		    .program        = std::move(result.program).TakeCompiledInfo(),
		    .handle         = {.id = ++next_shader_id, .module = module},
		};
	}

	// KYTY_DEBUG_SRT_STATS: one (draw, stage) event, with the vertex-fetch layout attached for the
	// stages that have one.
	template <typename InputInfo>
	static void
	RecordStageStats(const char* stage_name, const InputInfo& input_info,
	                 const ShaderRecompiler::IR::ResourcePlan&           plan,
	                 const ShaderRecompiler::IR::BindingLayout&          bindings,
	                 const ShaderRecompiler::IR::ResourceSpecialization& specialization,
	                 bool gpu_fetch) {
		auto event = StatsStageEvent(stage_name, plan, bindings, specialization);
		event.gpu_fetch = gpu_fetch;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			event.vertex_layout_key = StatsVertexLayoutKey(input_info);
			event.has_vertex_layout = true;
		}
		SrtStats::RecordStage(event);
	}

	// Dense DescriptorFeedback slot for a program that fetches its own buffer descriptors. The
	// shader sets this bit; the readback routes it back through ReportFeedbackSlot. A program
	// that cannot get a slot has no way to report a mismatch and stays on the CPU path.
	void AssignFeedbackSlot(SourceEntry& source) {
		if (feedback_slots.size() >= DescriptorFeedback::MaxSlots) {
			source.pinned_cpu = true;
			return;
		}
		source.feedback_slot = static_cast<uint32_t>(feedback_slots.size());
		feedback_slots.push_back(&source);
	}

	// One bit the GPU set since the last readback: the program's speculation was wrong.
	void ReportFeedbackSlot(uint32_t slot) {
		if (slot >= feedback_slots.size()) {
			return;
		}
		auto& source    = *feedback_slots[slot];
		source.cpu_next = true;
		source.mismatches++;
		if (SrtStats::Enabled()) {
			SrtStats::RecordGpuDescriptor(SrtStats::GpuDescriptorEvent::FeedbackBit);
		}
		if (source.mismatches > MaxFeedbackMismatches && !source.pinned_cpu) {
			source.pinned_cpu = true;
			LOGF("GPU descriptors: program pinned to the CPU path after %u mismatches, "
			     "hash=0x%016" PRIx64 "\n",
			     source.mismatches, source.resource_plan.shader_hash);
			if (SrtStats::Enabled()) {
				SrtStats::RecordGpuDescriptor(SrtStats::GpuDescriptorEvent::ProgramPinned);
			}
		}
	}

	template <typename InputInfo>
	ShaderProgram Get(const ShaderParams& params, InputInfo& input_info,
	                  uint32_t& push_data_cursor) {
		ShaderType stage;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage = input_info.mesh.threads_num[0] != 0 ? ShaderType::Mesh : ShaderType::Vertex;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage = ShaderType::Pixel;
		} else {
			static_assert(std::is_same_v<InputInfo, ShaderComputeInputInfo>);
			stage = ShaderType::Compute;
		}
		const char* label      = nullptr;
		const char* stage_name = nullptr;
		switch (stage) {
			case ShaderType::Vertex:
				label      = "ShaderRecompiler VS";
				stage_name = "vs";
				break;
			case ShaderType::Mesh:
				label      = "ShaderRecompiler MS";
				stage_name = "ms";
				break;
			case ShaderType::Pixel:
				label      = "ShaderRecompiler PS";
				stage_name = "ps";
				break;
			case ShaderType::Compute:
				label      = "ShaderRecompiler CS";
				stage_name = "cs";
				break;
			default: EXIT("invalid pipeline shader stage\n");
		}

		KYTY_PROFILER_BLOCK("ProgramCache::FindSource");
		lookup_key.stage           = stage;
		lookup_key.hash            = params.hash;
		lookup_key.user_data_count = static_cast<uint32_t>(params.user_data.size());
		lookup_key.code_size       = static_cast<uint32_t>(params.code.size());
		BuildStageStaticKey(input_info, lookup_key.static_state);
		auto                                         entry = programs.find(lookup_key);
		KYTY_PROFILER_END_BLOCK;
		ShaderRecompiler::IR::ResourceSnapshot resources;
		// The snapshot is moved into input_info.stage below and outlives this call, so it cannot
		// be pooled here. The specialization is compared and dropped on the cached path, so its
		// two vectors come from per-stage scratch that keeps its capacity across draws.
		auto& specialization = specialization_scratch[static_cast<size_t>(stage)];
		const ShaderRecompiler::IR::SrtRuntime       runtime {
		    .user_data                  = params.user_data,
		    .shader_base                = params.Base(),
		    .read_specialization_memory = ReadShaderGuestMemory,
		    .sync_memory                = SyncShaderGuestMemory,
		};
		ShaderRecompiler::IR::MaterializeReport report;
		// GPU-side descriptor fetch, stage 1 (docs/gpu-descriptor-fetch.md). A program whose
		// shader evaluates its own buffer V#s keeps the variant its last CPU materialization
		// chose, so the draw resolves image and sampler roots only. Entirely inert, and not even
		// tested past the entry's own flag, while the setting is off.
		bool gpu_path = false;
		if (entry != programs.end()) {
			auto&      source      = entry->second;
			const bool gpu_capable = source.gpu_descriptors && Config::GpuDescriptorsEnabled();
			gpu_path = gpu_capable && source.has_last_specialization && !source.cpu_next &&
			           !source.pinned_cpu;
			const ShaderRecompiler::IR::GpuFetchOverride override {
			    .buffers        = source.gpu_fetch_buffers,
			    .specialization = &source.last_specialization,
			};
			{
				KYTY_PROFILER_BLOCK("ProgramCache::MaterializeResources");
				bool ok = ShaderRecompiler::IR::MaterializeResources(
				    source.resource_plan, runtime, resources, specialization, &report,
				    gpu_path ? &override : nullptr);
				if (!ok && gpu_path) {
					// Stage 1 bring-up: find out whether the failure is caused by the skipped
					// roots. A CPU retry that succeeds means it is; one that fails means the
					// memory itself is bad.
					const std::string first_reason = report.reason;
					ok = ShaderRecompiler::IR::MaterializeResources(
					    source.resource_plan, runtime, resources, specialization, &report);
					LOGF("gpu-descriptors: materialization failed on the GPU path for %s "
					     "0x%016" PRIx64 " (%s); CPU retry %s\n",
					     stage_name, params.hash, first_reason.c_str(), ok ? "succeeded" : "failed");
					std::fflush(stdout);
					gpu_path        = false;
					source.cpu_next = true;
				}
				ReportMaterialization(label, stage, params.hash, report, ok);
			}
			if (gpu_capable && !gpu_path) {
				if (SrtStats::Enabled()) {
					SrtStats::RecordGpuDescriptor(
					    source.pinned_cpu           ? SrtStats::GpuDescriptorEvent::CpuPinned
					    : source.cpu_next           ? SrtStats::GpuDescriptorEvent::CpuFeedback
					                                : SrtStats::GpuDescriptorEvent::CpuFirst);
				}
				// The tuples this materialization derived are what the next GPU-fetch draw keeps.
				source.last_specialization.buffers = specialization.buffers;
				source.last_specialization.images  = specialization.images;
				source.has_last_specialization     = true;
				source.cpu_next                    = false;
			}
			const auto permutation = [&] {
				KYTY_PROFILER_BLOCK("ProgramCache::FindPermutation");
				return std::ranges::find_if(
				    entry->second.permutations, [&](const Permutation& candidate) {
					    const auto& layout = candidate.program.bindings;
					    return layout.push_data_start_dword ==
					               ShaderRecompiler::IR::PushData::StartFor(
					                   push_data_cursor, layout.ShaderDataDwords()) &&
					           candidate.specialization == specialization;
				    });
			}();
			if (permutation != entry->second.permutations.end()) {
				if (SrtStats::Enabled()) {
					RecordStageStats(stage_name, input_info, entry->second.resource_plan,
					                 permutation->program.bindings, permutation->specialization,
					                 gpu_path);
				}
				input_info.stage = {.program       = &permutation->program,
				                    .resources     = std::move(resources),
				                    .feedback_slot = source.feedback_slot};
				permutation->program.bindings.AdvancePushData(push_data_cursor);
				return permutation->handle;
			}
		}

		ShaderStageInputInfo stage_input {};
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			stage_input.vertex = &input_info;
		} else if constexpr (std::is_same_v<InputInfo, ShaderPixelInputInfo>) {
			stage_input.pixel = &input_info;
		} else {
			stage_input.compute = &input_info;
		}
		ShaderRecompiler::CompileOptions options;
		options.stage       = stage;
		options.shader_hash = params.hash;
		options.user_data   = params.user_data;
		options.back_code      = params.back_code;
		options.dump_ir     = Config::GetShaderLogDirection() != Config::ShaderLogDirection::Silent;
		options.early_dump  = options.dump_ir;
		options.dump_label  = label;
		options.input_info  = stage_input;
		options.scratch_dwords = input_info.scratch_size_dwords;
		if constexpr (std::is_same_v<InputInfo, ShaderVertexInputInfo>) {
			options.user_data_base = 8;
			if (stage == ShaderType::Mesh) {
				options.user_data_base = 0;
				options.wave_size      = input_info.mesh.wave_size;
				options.scratch_dwords = input_info.mesh.scratch_size_dwords;
			}
		} else if constexpr (std::is_same_v<InputInfo, ShaderComputeInputInfo>) {
			options.wave_size = input_info.wave_size;
		}
		auto translated = ShaderRecompiler::TranslateProgram(params.code, options);
		if (entry == programs.end()) {
			auto resource_plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
			MarkGpuFetchStub(resource_plan);
			ReportMaterialization(label, stage, params.hash, report,
			                      ShaderRecompiler::IR::MaterializeResources(
			                          resource_plan, runtime, resources, specialization, &report));
			entry = programs.try_emplace(lookup_key, std::move(resource_plan)).first;
		}
		auto& source_entry = entry->second;
		if (!source_entry.permutations.empty() && Config::GpuDescriptorsEnabled()) {
			// Diagnostic for the stage 1 bring-up: a new permutation for a known program means the
			// specialization did not match any compiled one. Name the first difference.
			const auto& first = source_entry.permutations.front().specialization;
			std::string why;
			const auto  nb = std::min(first.buffers.size(), specialization.buffers.size());
			for (size_t i = 0; i < nb && why.empty(); i++) {
				const auto& a = first.buffers[i];
				const auto& b = specialization.buffers[i];
				if (!(a == b)) {
					why = fmt::format("buffer[{}] stride {:#x}->{:#x} format {}->{} swizzle {:#x}->{:#x}",
					                  i, a.packed_stride, b.packed_stride,
					                  static_cast<int>(a.descriptor_format),
					                  static_cast<int>(b.descriptor_format), a.descriptor_swizzle,
					                  b.descriptor_swizzle);
				}
			}
			if (why.empty() && first.buffers.size() != specialization.buffers.size()) {
				why = fmt::format("buffer count {}->{}", first.buffers.size(),
				                  specialization.buffers.size());
			}
			if (why.empty() && !(first.images == specialization.images)) {
				why = fmt::format("images differ (count {}->{})", first.images.size(),
				                  specialization.images.size());
			}
			if (why.empty()) {
				why = "push data start";
			}
			LOGF("gpu-descriptors: new permutation #%zu for %s 0x%016" PRIx64 " gpu_path=%d: %s\n",
			     source_entry.permutations.size() + 1, stage_name, params.hash, gpu_path ? 1 : 0,
			     why.c_str());
			std::fflush(stdout);
		}
		CopyGpuFetchStub(source_entry.resource_plan.info, translated.program.info);
		if (Config::GpuDescriptorsEnabled()) {
			// A variant compiles against the tuples the CPU just derived; those are the ones a
			// later GPU-fetch draw keeps.
			source_entry.last_specialization.buffers = specialization.buffers;
			source_entry.last_specialization.images  = specialization.images;
			source_entry.has_last_specialization     = true;
			source_entry.cpu_next                    = false;
		}
		source_entry.permutations.push_back(CompilePermutation(
		    params, options, std::move(translated), std::move(specialization), push_data_cursor));
		const auto& permutation = source_entry.permutations.back();
		source_entry.AdoptGpuFetch(permutation.program.info);
		if (source_entry.gpu_descriptors &&
		    source_entry.feedback_slot == ShaderRecompiler::IR::BindingLayout::NoFeedbackSlot) {
			AssignFeedbackSlot(source_entry);
			if (SrtStats::Enabled()) {
				SrtStats::RecordGpuDescriptor(SrtStats::GpuDescriptorEvent::CpuFirst);
			}
		}
		if (SrtStats::Enabled()) {
			RecordStageStats(stage_name, input_info, source_entry.resource_plan,
			                 permutation.program.bindings, permutation.specialization, false);
		}
		input_info.stage = {.program       = &permutation.program,
		                    .resources     = std::move(resources),
		                    .feedback_slot = source_entry.feedback_slot};
		permutation.program.bindings.AdvancePushData(push_data_cursor);

		std::array<size_t, static_cast<size_t>(ShaderType::Mesh) + 1> counts {};
		for (const auto& [key, source]: programs) {
			counts[static_cast<size_t>(key.stage)] += source.permutations.size();
		}
		// Guest geometry shaders are compiled through the host mesh stage.
		std::printf("Shaders: VS %zu | PS %zu | CS %zu | GS %zu\n",
		            counts[static_cast<size_t>(ShaderType::Vertex)],
		            counts[static_cast<size_t>(ShaderType::Pixel)],
		            counts[static_cast<size_t>(ShaderType::Compute)],
		            counts[static_cast<size_t>(ShaderType::Mesh)]);
		return permutation.handle;
	}

	explicit ProgramCache(vk::Device device): device(device) {
		lookup_key.static_state.reserve(MaxStaticKeyWords);
	}
	~ProgramCache() {
		for (const auto& [key, entry]: programs) {
			(void)key;
			for (const auto& permutation: entry.permutations) {
				device.destroyShaderModule(permutation.handle.module, nullptr);
			}
		}
	}

	std::unordered_map<ProgramKey, SourceEntry, ProgramKeyHash> programs;
	std::array<ShaderRecompiler::IR::ResourceSpecialization,
	           static_cast<size_t>(ShaderType::Mesh) + 1>       specialization_scratch;
	ProgramKey                                                  lookup_key;
	vk::Device                                                  device;
	uint64_t                                                    next_shader_id = 0;
	// Indexed by DescriptorFeedback slot. The map is node based, so an entry's address is stable
	// for the cache's lifetime and nothing is ever erased.
	std::vector<SourceEntry*> feedback_slots;
};

PipelineCache::PipelineCache(GraphicContext& graphics)
    : m_graphics(graphics), m_program_cache(std::make_unique<ProgramCache>(graphics.device)) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	InitializeDriverCache();
}

PipelineCache::~PipelineCache() {
	{
		Common::LockGuard save_lock(m_save_mutex);
		JoinCacheWriter();
	}
	Save();
	auto destroy = [this](const auto& pipelines) {
		for (const auto& [key, pipeline]: pipelines) {
			(void)key;
			m_graphics.device.destroyPipeline(pipeline->pipeline, nullptr);
			m_graphics.device.destroyPipelineLayout(pipeline->pipeline_layout, nullptr);
			m_graphics.device.destroyDescriptorSetLayout(pipeline->descriptor_set_layout, nullptr);
		}
	};
	destroy(m_graphics_pipelines);
	destroy(m_compute_pipelines);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
	}
}

void PipelineCache::InitializeDriverCache() {
	const auto title_id = PipelineCacheTitleId();
	if (title_id.empty()) {
		return;
	}
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		PipelineCacheLog("Vulkan pipeline cache: disabled (non-Release build)");
		return;
	}

	m_driver_cache_path     = std::filesystem::path("_PipelineCache") / (title_id + ".bin");
	const auto path         = Common::PathToString(m_driver_cache_path);
	const bool cache_exists = Common::File::IsFileExisting(m_driver_cache_path);
	if (cache_exists) {
		PipelineCacheLog("Vulkan pipeline cache: loading {}", path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initializing {}", path);
	}
	std::vector<uint8_t> initial_data;
	if (cache_exists) {
		Common::File file(m_driver_cache_path, Common::File::Mode::Read);
		const auto   file_size = file.IsInvalid() ? 0 : file.Size();
		const auto   signature = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
		if (file_size >= signature.size() + sizeof(uint64_t) &&
		    file_size <= std::numeric_limits<uint32_t>::max()) {
			std::string cached_signature(signature.size(), '\0');
			uint64_t    payload_hash = 0;
			initial_data.resize(file_size - signature.size() - sizeof(payload_hash));
			uint32_t signature_read = 0;
			uint32_t hash_read      = 0;
			uint32_t payload_read   = 0;
			file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
			          &signature_read);
			file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
			file.Read(initial_data.data(), static_cast<uint32_t>(initial_data.size()),
			          &payload_read);
			file.Close();
			if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
			    payload_read != initial_data.size() || cached_signature != signature ||
			    XXH3_64bits(initial_data.data(), initial_data.size()) != payload_hash) {
				initial_data.clear();
				PipelineCacheLog(
				    "Vulkan pipeline cache: invalidating {} (driver, emulator, or data mismatch)",
				    path);
			}
		} else {
			file.Close();
			PipelineCacheLog("Vulkan pipeline cache: invalidating {} (invalid file size)", path);
		}
	}

	vk::PipelineCacheCreateInfo create {};
	create.initialDataSize = initial_data.size();
	create.pInitialData    = initial_data.empty() ? nullptr : initial_data.data();
	auto result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	if (result != vk::Result::eSuccess && !initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: driver rejected {} ({}); starting empty", path,
		                 vk::to_string(result));
		initial_data.clear();
		create.initialDataSize = 0;
		create.pInitialData    = nullptr;
		result = m_graphics.device.createPipelineCache(&create, nullptr, &m_driver_cache);
	}
	if (result != vk::Result::eSuccess) {
		PipelineCacheLog("Vulkan pipeline cache: disabled ({})", vk::to_string(result));
		m_driver_cache = nullptr;
		return;
	}
	if (!initial_data.empty()) {
		PipelineCacheLog("Vulkan pipeline cache: loaded {} bytes from {}", initial_data.size(),
		                 path);
	} else {
		PipelineCacheLog("Vulkan pipeline cache: initialized empty");
	}
}

bool PipelineCache::SnapshotDriverCache(std::vector<uint8_t>& payload) {
	Common::LockGuard lock(m_mutex);
	if (m_driver_cache == nullptr) {
		return false;
	}

	size_t     size = 0;
	vk::Result result;
	for (uint32_t attempt = 0; attempt < 3; attempt++) {
		size   = 0;
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, nullptr);
		if (result != vk::Result::eSuccess || size == 0 ||
		    size > std::numeric_limits<uint32_t>::max()) {
			break;
		}
		payload.resize(size);
		result = m_graphics.device.getPipelineCacheData(m_driver_cache, &size, payload.data());
		if (result != vk::Result::eIncomplete) {
			break;
		}
	}
	if (result != vk::Result::eSuccess || size == 0 ||
	    size > std::numeric_limits<uint32_t>::max()) {
		PipelineCacheLog("Vulkan pipeline cache: save failed ({}, {} bytes)",
		                 vk::to_string(result), size);
		payload.clear();
		return false;
	}
	payload.resize(size);
	return true;
}

bool PipelineCache::WriteDriverCacheFile(const std::vector<uint8_t>& payload) {
	auto       prefix       = DriverCacheSignature(m_graphics.GetPhysicalDeviceProperties());
	const auto payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_driver_cache_path.parent_path())) {
		PipelineCacheLog("Vulkan pipeline cache: failed to create cache directory");
		return false;
	}
	auto temp_path = m_driver_cache_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_driver_cache_path)) {
		PipelineCacheLog("Vulkan pipeline cache: failed to write {}",
		                 Common::PathToString(m_driver_cache_path));
		return false;
	}
	return true;
}

void PipelineCache::JoinCacheWriter() {
	if (m_cache_writer.joinable()) {
		m_cache_writer.join();
	}
	m_cache_writer_busy.store(false, std::memory_order_release);
}

void PipelineCache::SaveIfDirty() {
	if (m_pipelines_since_save.load(std::memory_order_relaxed) == 0) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now - m_last_save < DriverCacheSaveInterval) {
		return;
	}
	// A write is still in flight: skip this round instead of queueing another one.
	if (m_cache_writer_busy.load(std::memory_order_acquire)) {
		return;
	}

	Common::LockGuard save_lock(m_save_mutex);
	JoinCacheWriter();

	m_last_save = now;
	std::vector<uint8_t> payload;
	if (!SnapshotDriverCache(payload)) {
		return;
	}
	const auto pipelines = m_pipelines_since_save.exchange(0, std::memory_order_relaxed);
	const auto bytes     = payload.size();
	m_cache_writer_busy.store(true, std::memory_order_release);
	m_cache_writer = std::thread([this, payload = std::move(payload), pipelines, bytes] {
		if (WriteDriverCacheFile(payload)) {
			PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {} ({} new pipelines)",
			                 bytes, Common::PathToString(m_driver_cache_path), pipelines);
		}
		m_cache_writer_busy.store(false, std::memory_order_release);
	});
}

void PipelineCache::Save() {
	Common::LockGuard    save_lock(m_save_mutex);
	JoinCacheWriter();
	std::vector<uint8_t> payload;
	if (SnapshotDriverCache(payload)) {
		if (WriteDriverCacheFile(payload)) {
			PipelineCacheLog("Vulkan pipeline cache: saved {} bytes to {} ({} new pipelines)",
			                 payload.size(), Common::PathToString(m_driver_cache_path),
			                 m_pipelines_since_save.exchange(0, std::memory_order_relaxed));
		}
	}
	Common::LockGuard lock(m_mutex);
	if (m_driver_cache != nullptr) {
		m_graphics.device.destroyPipelineCache(m_driver_cache, nullptr);
		m_driver_cache = nullptr;
	}
}

PipelineCache::GraphicsPrograms PipelineCache::GetGraphicsPrograms(
    const HW::VertexShaderInfo& vertex_regs, const HW::PixelShaderInfo& pixel_regs,
    const HW::ShaderRegisters& sh, const HW::Context& context, const HW::UserConfig& user_config,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping, bool pixel_active,
    ShaderVertexInputInfo& vertex_info, ShaderPixelInputInfo& pixel_info) {
	const auto vertex_params = PrepareProgram(vertex_regs, context, user_config, vertex_info);
	const bool mesh_active   = vertex_info.mesh.threads_num[0] != 0;
	if (mesh_active) {
		EXIT_NOT_IMPLEMENTED(!m_graphics.mesh_shader_enabled);
		auto& mesh              = vertex_info.mesh;
		mesh.host_subgroup_size = m_graphics.subgroup_size;
		const auto& limits      = m_graphics.mesh_shader_properties;
		const auto  logical_threads =
		    mesh.threads_num[0] * mesh.threads_num[1] * mesh.threads_num[2];
		const auto host_threads = ((logical_threads + mesh.wave_size - 1u) / mesh.wave_size) *
		                          std::min(mesh.host_subgroup_size, mesh.wave_size);
		if (host_threads > limits.maxMeshWorkGroupInvocations ||
		    host_threads > limits.maxMeshWorkGroupSize[0] ||
		    mesh.max_vertices > limits.maxMeshOutputVertices ||
		    mesh.max_primitives > limits.maxMeshOutputPrimitives ||
		    mesh.lds_size_dwords * sizeof(uint32_t) > limits.maxMeshSharedMemorySize) {
			EXIT("mesh shader exceeds host limits: threads=%u vertices=%u primitives=%u LDS=%u\n",
			     host_threads, mesh.max_vertices, mesh.max_primitives, mesh.lds_size_dwords);
		}
	}
	ShaderParams pixel_params;
	if (pixel_active) {
		pixel_params = PrepareProgram(pixel_regs, sh, target_export_mapping, pixel_info);
	}
	if (context.GetClipControl().clip_disable) {
		const auto& viewport = context.GetScreenViewport().viewports[0];
		const auto& limits   = m_graphics.GetPhysicalDeviceProperties().limits;
		auto&       clip     = vertex_info.clip_space;
		clip.scale[0]        = viewport.xscale;
		clip.scale[1]        = viewport.yscale;
		clip.offset[0]       = viewport.xoffset;
		clip.offset[1]       = viewport.yoffset;
		clip.half_extent[0] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u)) * 0.5f;
		clip.half_extent[1] =
		    static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u)) * 0.5f;
		clip.enabled = true;
	}
	Common::LockGuard lock(m_mutex);
	uint32_t          push_data_cursor =
	    mesh_active ? ShaderRecompiler::IR::PushData::MeshDrawDwordCount : 0;
	GraphicsPrograms  result;
	if (pixel_active) {
		result.pixel = m_program_cache->Get(pixel_params, pixel_info, push_data_cursor);
	}
	result.vertex = m_program_cache->Get(vertex_params, vertex_info, push_data_cursor);
	return result;
}

void PipelineCache::ReportFeedbackSlot(uint32_t slot) {
	Common::LockGuard lock(m_mutex);
	m_program_cache->ReportFeedbackSlot(slot);
}

ShaderProgram PipelineCache::GetComputeProgram(const HW::ComputeShaderInfo& regs,
                                               const HW::ShaderRegisters&   sh,
                                               ShaderComputeInputInfo&      input_info) {
	input_info.host_subgroup_size = m_graphics.SupportsComputeWave64() ? 64u : 32u;
	const auto        params      = PrepareProgram(regs, sh, input_info);
	Common::LockGuard lock(m_mutex);
	uint32_t          push_data_cursor = 0;
	return m_program_cache->Get(params, input_info, push_data_cursor);
}

bool PipelineStaticParameters::operator==(const PipelineStaticParameters& other) const noexcept {
	return std::memcmp(this, &other, sizeof(*this)) == 0;
}

PipelineCache::Pipeline& PipelineCache::CreateGraphicsPipeline(
    std::span<const RenderColorInfo> colors, const RenderDepthInfo& depth,
    const ShaderVertexInputInfo& vs_input_info, CommandBuffer& command,
    const ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
    bool primitive_restart_enable, const ShaderProgram& vertex_program,
    const ShaderProgram& pixel_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Gfx)", profiler::colors::DeepOrangeA200);

	EXIT_IF(colors.size() > RENDER_COLOR_ATTACHMENTS_MAX);
	EXIT_IF(!vertex_program);
	const bool ps_active = ps_input_info != nullptr;
	EXIT_IF(ps_active && !pixel_program);
	const auto color_count = static_cast<uint32_t>(colors.size());

	Common::LockGuard lock(m_mutex);
	auto&             ctx = command.GetRegisters();

	const HW::ModeControl& mc = ctx.GetModeControl();

	const auto vs_id = vertex_program.id;
	const auto ps_id = ps_active ? pixel_program.id : 0;

	GraphicsPipelineKey key {};
	key.vs_shader_id            = vs_id;
	key.ps_shader_id            = ps_id;
	auto& static_params         = key.static_params;
	auto& rendering             = key.rendering;
	rendering.color_count       = color_count;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		EXIT_IF(!colors[i].image_id || colors[i].desc.view_info.format == vk::Format::eUndefined);
		static_params.color_mask[i] = colors[i].export_mapping.ApplyMask(
		    render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot));
		rendering.color_formats[i] = colors[i].desc.view_info.format;
		if (attachment_samples == 0) {
			attachment_samples = colors[i].desc.info.samples;
		} else if (attachment_samples != colors[i].desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, colors[i].desc.info.samples);
		}
	}
	const bool with_depth =
	    depth.desc.view_info.format != vk::Format::eUndefined && static_cast<bool>(depth.image_id);
	if (with_depth) {
		const auto aspects       = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		rendering.depth_format   = aspects & vk::ImageAspectFlagBits::eDepth
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		rendering.stencil_format = aspects & vk::ImageAspectFlagBits::eStencil
		                               ? depth.desc.view_info.format
		                               : vk::Format::eUndefined;
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
	}
	if (color_count == 0 && !with_depth) {
		attachment_samples = render_sample_count(ctx.GetAaConfig().msaa_num_samples);
		EXIT_IF(!static_cast<bool>(
		    m_graphics.GetPhysicalDeviceProperties().limits.framebufferNoAttachmentsSampleCounts &
		    vulkan_sample_count(attachment_samples)));
	}
	EXIT_IF(attachment_samples == 0 ||
	        vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {});

	if (ps_active && depth.depth_test_enable && ps_input_info->ps_execute_on_noop) {
		static std::atomic<uint32_t> log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 16) {
			LOGF("Pipeline: temporary: accepting EXEC_ON_NOOP with depth test enabled\n");
		}
	}

	const auto& clip_control               = ctx.GetClipControl();
	static_params.negative_one_to_one      = !clip_control.dx_clip_space;
	static_params.depth_clip_enable        = clip_control.IsZClipEnabled();
	static_params.topology                 = topology;
	static_params.primitive_restart_enable = primitive_restart_enable;
	static_params.samples                  = attachment_samples;
	static_params.sample_shading_enable =
	    ps_active && attachment_samples > 1 && ps_input_info->ps_sample_shading;
	if (static_params.sample_shading_enable && !m_graphics.sample_rate_shading_enabled) {
		EXIT("Pipeline: sample-rate shading is required but unsupported by the host\n");
	}
	static_params.depth_bounds_test_enable = depth.depth_bounds_test_enable;
	static_params.depth_min_bounds         = depth.depth_min_bounds;
	static_params.depth_max_bounds         = depth.depth_max_bounds;
	static_params.stencil_test_enable      = depth.stencil_test_enable;
	static_params.stencil_front            = depth.stencil_static_front;
	static_params.stencil_back             = depth.stencil_static_back;
	const bool rect_list     = topology == vk::PrimitiveTopology::ePatchList;
	static_params.cull_back  = !rect_list && mc.cull_back;
	static_params.cull_front = !rect_list && mc.cull_front;
	static_params.face       = mc.face;
	static_params.provoking_vtx_last = mc.provoking_vtx_last;
	static_params.polygon_mode =
	    ResolvePolygonMode(mc, static_params.cull_front, static_params.cull_back);

	for (uint32_t i = 0; i < color_count; i++) {
		const auto& rt                        = ctx.GetRenderTarget(colors[i].target_slot);
		const auto& bc                        = ctx.GetBlendControl(colors[i].target_slot);
		static_params.color_srcblend[i]       = bc.color_srcblend;
		static_params.color_comb_fcn[i]       = bc.color_comb_fcn;
		static_params.color_destblend[i]      = bc.color_destblend;
		static_params.alpha_srcblend[i]       = bc.alpha_srcblend;
		static_params.alpha_comb_fcn[i]       = bc.alpha_comb_fcn;
		static_params.alpha_destblend[i]      = bc.alpha_destblend;
		static_params.separate_alpha_blend[i] = bc.separate_alpha_blend;
		static_params.blend_enable[i]         = bc.enable;
		static_params.blend_bypass[i]         = rt.info.blend_bypass;
	}
	if (vs_input_info.stage.program->stage != ShaderType::Mesh) {
		EXIT_IF(vs_input_info.buffers_num < 0 ||
		        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX ||
		        vs_input_info.resources_num < 0 ||
		        vs_input_info.resources_num > ShaderVertexInputInfo::RES_MAX);
		key.vertex_input.binding_count   = static_cast<uint8_t>(vs_input_info.buffers_num);
		key.vertex_input.attribute_count = static_cast<uint8_t>(vs_input_info.resources_num);
		uint32_t attributes_num          = 0;
		for (int binding = 0; binding < vs_input_info.buffers_num; binding++) {
			const auto& buffer = vs_input_info.buffers[binding];
			EXIT_IF(buffer.attr_num < 0 || buffer.attr_num > ShaderVertexInputBuffer::ATTR_MAX);
			attributes_num += static_cast<uint32_t>(buffer.attr_num);
			EXIT_IF(attributes_num > static_cast<uint32_t>(vs_input_info.resources_num));
			key.vertex_input.bindings[binding] = {.stride   = buffer.stride,
			                                      .instance = buffer.fetch_index != 0};
			for (int attribute = 0; attribute < buffer.attr_num; attribute++) {
				const auto index = buffer.attr_indices[attribute];
				EXIT_IF(index < 0 || index >= vs_input_info.resources_num);
				key.vertex_input.attributes[index] = {
				    .offset  = buffer.attr_offsets[attribute],
				    .binding = static_cast<uint8_t>(binding),
				};
			}
		}
		EXIT_IF(attributes_num != static_cast<uint32_t>(vs_input_info.resources_num));
	}

	if (m_last_graphics_pipeline != nullptr && key == m_last_graphics_key) {
		return *m_last_graphics_pipeline;
	}

	if (auto iter = m_graphics_pipelines.find(key); iter != m_graphics_pipelines.end()) {
		m_last_graphics_key      = key;
		m_last_graphics_pipeline = iter->second.get();
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(vs_input_info);
		if (ps_active) {
			ShaderDbgDumpInputInfo(*ps_input_info);
		}
		LOGF("PipelineTrace: shader modules VS=%" PRIu64 " module=%p PS=%" PRIu64 " module=%p\n",
		     vs_id, static_cast<void*>(vertex_program.module), ps_id,
		     static_cast<void*>(pixel_program.module));
	}

	auto cached = std::make_unique<Pipeline>();
	LogPipelineTrace("CreatePipelineInternal begin", vs_id, ps_id);
	CreatePipelineInternal(m_graphics, *cached, rendering, key.vertex_input, vs_input_info,
	                       vertex_program, ps_input_info, pixel_program, static_params,
	                       m_driver_cache);
	LogPipelineTrace("CreatePipelineInternal done", vs_id, ps_id);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_graphics_pipelines.emplace(std::move(key), std::move(cached));
	EXIT_IF(!inserted);
	m_pipelines_since_save.fetch_add(1, std::memory_order_relaxed);

	m_last_graphics_key      = iter->first;
	m_last_graphics_pipeline = iter->second.get();
	return *iter->second;
}

PipelineCache::Pipeline&
PipelineCache::CreateComputePipeline(const ShaderComputeInputInfo& input_info,
                                     const ShaderProgram&          compute_program) {
	KYTY_PROFILER_BLOCK("PipelineCache::CreatePipeline(Compute)", profiler::colors::RedA100);

	EXIT_IF(!compute_program);

	Common::LockGuard lock(m_mutex);

	if (auto iter = m_compute_pipelines.find(compute_program.id);
	    iter != m_compute_pipelines.end()) {
		return *iter->second;
	}

	if (graphics_debug_dump_enabled()) {
		ShaderDbgDumpInputInfo(input_info);
	}

	auto cached = std::make_unique<Pipeline>();
	CreatePipelineInternal(m_graphics, *cached, input_info, compute_program.module, m_driver_cache);

	EXIT_NOT_IMPLEMENTED(cached->pipeline == nullptr);
	EXIT_NOT_IMPLEMENTED(cached->pipeline_layout == nullptr);

	auto [iter, inserted] = m_compute_pipelines.emplace(compute_program.id, std::move(cached));
	EXIT_IF(!inserted);
	m_pipelines_since_save.fetch_add(1, std::memory_order_relaxed);

	return *iter->second;
}
} // namespace Libs::Graphics
