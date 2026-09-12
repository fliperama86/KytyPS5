#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace Libs::Graphics {

class BufferCache;

class FaultManager {
	static constexpr size_t MaxPendingFaults = 8;

public:
	// Stage 1 diagnostics (docs/gpu-descriptor-fetch.md). A shader store to a page the BDA page
	// table does not map is dropped and only sets a fault bit, so a later CPU read of that page
	// sees stale bytes. The ring records which pages faulted so a materialization failure can say
	// whether the address it could not walk is one of them.
	static constexpr size_t FaultRingSize = 512;

	struct FaultRecord {
		uint64_t page = 0;
		uint64_t pass = 0;
	};

	struct FaultPageStatus {
		bool     seen        = false;
		uint32_t hits        = 0;
		uint64_t first_pass  = 0;
		uint64_t last_pass   = 0;
		uint64_t passes      = 0;
		uint64_t pages_total = 0;
		uint64_t ring_pages  = 0;
		uint64_t pages_known = 0;
	};

	// Step 1 of docs/sync-points-design.md: with --gpu-prologue-table the fault buffer carries one
	// counter dword past the bitmap, which a prologue read that found no entry adds to. The bit
	// it sets is the same bit a data read sets, so the page is registered exactly as it always
	// was and the compaction shader still scans one bitmap; only the counter is new, and reading
	// it back is a four-byte copy.
	FaultManager(GraphicContext& graphics, CommandScheduler& scheduler, BufferCache& buffer_cache,
	             bool with_prologue_counter = false);
	~FaultManager();
	KYTY_CLASS_NO_COPY(FaultManager);

	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return &m_fault_buffer; }
	void                  ProcessFaultBuffer();
	[[nodiscard]] FaultPageStatus QueryFaultedPage(uint64_t page) const;

private:
	void RecordFaults(std::span<const uint64_t> pages);

	GraphicContext&                            m_graphics;
	CommandScheduler&                          m_scheduler;
	BufferCache&                               m_buffer_cache;
	bool                                       m_prologue_counter = false;
	Buffer                                     m_fault_buffer;
	Buffer                                     m_download_buffer;
	std::array<uint64_t, MaxPendingFaults>      m_fault_areas {};
	uint32_t                                   m_current_area = 0;
	struct PageHistory {
		uint32_t hits       = 0;
		uint64_t first_pass = 0;
		uint64_t last_pass  = 0;
	};
	// Every page that has ever faulted this session: the ring is far too short to answer "was
	// this page ever one frame late" in a run that faults thousands of pages.
	std::unordered_map<uint64_t, PageHistory>  m_fault_history;
	std::array<FaultRecord, FaultRingSize>     m_fault_ring {};
	uint64_t                                   m_fault_ring_next  = 0;
	uint64_t                                   m_fault_passes     = 0;
	uint64_t                                   m_fault_pages      = 0;
	uint64_t                                   m_window_passes    = 0;
	uint64_t                                   m_window_pages     = 0;
	vk::DescriptorSetLayout                    m_fault_process_desc_layout = nullptr;
	vk::Pipeline                               m_fault_process_pipeline = nullptr;
	vk::PipelineLayout                         m_fault_process_pipeline_layout = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_FAULTMANAGER_H_
