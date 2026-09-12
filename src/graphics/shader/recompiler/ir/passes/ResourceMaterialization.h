#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// Canonical module-affecting resource state. Runtime addresses and descriptor payloads remain in
// ResourceSnapshot and therefore do not create shader permutations.
struct ResourceSpecialization {
	struct Buffer {
		uint32_t               packed_stride                   = 0;
		Prospero::BufferFormat descriptor_format               = Prospero::BufferFormat::kInvalid;
		uint32_t               descriptor_swizzle              = DstSel(4, 5, 6, 7);
		bool                   operator==(const Buffer&) const = default;
	};

	struct Image {
		Prospero::TextureNumericClass numeric_class = Prospero::TextureNumericClass::Unsupported;
		Decoder::ImageDimension       dimension     = Decoder::ImageDimension::Unknown;
		uint32_t                      mip_count     = 1;
		Prospero::BufferFormat        conversion_format          = Prospero::BufferFormat::kInvalid;
		uint32_t                      shader_swizzle             = ShaderImageIdentitySwizzle;
		uint32_t                      indirect_root              = ImageResource::NoIndirectImage;
		uint32_t                      indirect_mapping_offset    = 0;
		uint32_t                      indirect_search_iterations = 0;
		bool                          cube                       = false;
		bool                          fmask                      = false;
		bool                          operator==(const Image&) const = default;
	};

	std::vector<Buffer> buffers;
	std::vector<Image>  images;

	bool operator==(const ResourceSpecialization&) const = default;
};

// Extracts the descriptor/SRT value graph before resource specialization. The returned plan owns
// its values and is independent of the translated shader CFG.
ResourcePlan ExtractResourcePlan(const Program& program);

// Resolves and specializes the immutable resource plan in one transaction. On failure both
// destinations are unchanged.
struct MaterializeReport {
	std::string reason;
	uint32_t    dropped_candidates = 0;
	uint32_t    dropped_shapes     = 0;
	std::string dropped_summary;
	// Stage 1 diagnostics (docs/gpu-descriptor-fetch.md): set when a descriptor source failed to
	// evaluate, so the renderer can say what happened to the page the walk tried to read.
	std::string failure_detail;
	uint64_t    failure_address       = 0;
	bool        failure_address_valid = false;
	uint32_t    failure_source        = UINT32_MAX;
	// Skipped (gpu_fetch) sources of the plan that failed, as "3,7" or empty.
	std::string skipped_sources;
	// Every guest address the failing root read, oldest first, for the caller to look up.
	std::vector<uint64_t> failure_addresses;
};

// Buffer resources the shader evaluates for itself (docs/gpu-descriptor-fetch.md, stage 1). The
// host passes this on a draw that keeps the program's last CPU-derived variant: those descriptors
// are not needed for the draw to be correct, so their specialization tuples come from the last
// CPU materialization instead of from the runtime V# and the same permutation is selected no
// matter what the guest has since written into the SRT. A mismatch is reported by the shader
// through the DescriptorFeedback binding, which puts the program back on the CPU path.
struct GpuFetchOverride {
	// One byte per entry in ResourcePlan::info.buffers, non-zero for a gpu_fetch resource.
	std::span<const uint8_t> buffers;
	// One byte per flat SRT slot, non-zero where the shader evaluates the read itself (stage 1b).
	// Those slots stay zero in the snapshot on either path: a module compiled with in-shader reads
	// never loads them from the FlattenedSrt binding, and nothing on the host reads them either.
	std::span<const uint8_t> flat_slots;
	// Tuples of the program's last CPU materialization. Must cover every marked buffer. Null on a
	// draw that still materializes its descriptors on the CPU, which leaves `buffers` inert.
	const ResourceSpecialization* specialization = nullptr;
};

bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                          MaterializeReport*      report    = nullptr,
                          const GpuFetchOverride* gpu_fetch = nullptr);

// Applies an already-derived specialization to native IR before layout and emission.
void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization);

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_RESOURCEMATERIALIZATION_H_ */
