#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <shared_mutex>

namespace Libs::Graphics {

class CommandScheduler;
class GuestGpu;

class GpuResourceManager {
public:
	GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler);
	~GpuResourceManager();
	KYTY_CLASS_NO_COPY(GpuResourceManager);

	[[nodiscard]] BufferCache&  GetBufferCache() { return m_buffer_cache; }
	[[nodiscard]] TextureCache& GetTextureCache() { return m_texture_cache; }
	void                        SetGpu(GuestGpu* gpu) noexcept { m_gpu = gpu; }

	[[nodiscard]] bool HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept;
	[[nodiscard]] bool InvalidateMemory(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsMapped(uint64_t vaddr, uint64_t size) const noexcept;
	void               MapMemory(uint64_t vaddr, uint64_t size);
	void               UnmapMemory(uint64_t vaddr, uint64_t size);
	void               PrepareBda();
	// Step 1 of docs/sync-points-design.md: imports every mapped range the prologue page
	// table does not cover yet. Called from PrepareBda, so it runs on the GPU thread with a
	// command buffer recording whatever thread the guest mapped from. One relaxed load when
	// nothing is waiting.
	void               ImportPendingRanges();
	// Design P (docs/bda-sync-design.md): the submission boundary waits here for the
	// re-protection helper to have drained and its last batch to have landed, then scans once so
	// every landed page is uploaded again before the submission's first draw.
	void               AsyncProtectBoundary();
	void               RunGarbageCollector();

private:
	friend struct GpuResourceManagerTestAccess;
	[[nodiscard]] bool BdaScanRequired(uint64_t buffer_generation,
	                                   uint64_t mapped_generation) const noexcept;
	// The scan body, with m_mapped_ranges_mutex held.
	void               ScanBda(uint64_t buffer_generation, uint64_t mapped_generation,
	                           bool record = true);
	PageManager               m_page_manager;
	CommandScheduler&         m_scheduler;
	BufferCache               m_buffer_cache;
	TextureCache              m_texture_cache;
	mutable std::shared_mutex m_mapped_ranges_mutex;
	RangeSet                  m_mapped_ranges;
	GuestGpu*                 m_gpu = nullptr;
	bool                      m_fault_process_pending = false;
	// A guest map added a range the imports do not cover yet (step 1). Written under
	// m_mapped_ranges_mutex by any guest thread, cleared on the GPU thread.
	std::atomic_bool          m_import_pending {false};
	uint64_t                  m_mapped_generation = 0;
	uint64_t m_last_bda_buffer_generation = std::numeric_limits<uint64_t>::max();
	uint64_t m_last_bda_mapped_generation = std::numeric_limits<uint64_t>::max();
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPURESOURCEMANAGER_H_
