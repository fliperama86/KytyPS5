#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"
#include "graphics/host_gpu/renderer/cache/descriptorFeedback.h"
#include "graphics/host_gpu/renderer/cache/faultManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <atomic>
#include <map>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;
class TextureCache;

using BufferId = Common::SlotId;
inline constexpr BufferId NULL_BUFFER_ID {0};

class BufferCache {
public:
	static constexpr uint32_t CACHING_PAGEBITS  = 14;
	static constexpr uint64_t CACHING_PAGESIZE  = uint64_t {1} << CACHING_PAGEBITS;
	static constexpr uint64_t CACHING_NUMPAGES  = uint64_t {1} << (40 - CACHING_PAGEBITS);
	static constexpr uint64_t BDA_PAGETABLE_SIZE =
	    CACHING_NUMPAGES * sizeof(vk::DeviceAddress);

	BufferCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	            TextureCache& texture_cache);
	~BufferCache();
	KYTY_CLASS_NO_COPY(BufferCache);

	void                   InvalidateMemory(uint64_t vaddr, uint64_t size);
	// Design P (docs/bda-sync-design.md): waits for the asynchronous re-protection helper to have
	// drained and its last batch to have landed, so the scan that follows re-uploads every page
	// it protected. One relaxed load when the setting is off.
	void                   DrainAsyncProtect() { m_memory_tracker.DrainAsyncProtect(); }
	[[nodiscard]] bool     AsyncProtectEnabled() const noexcept {
		return m_memory_tracker.AsyncProtectEnabled();
	}
	// Frame replay (docs/frame-replay.md): re-marks a range as CPU-written, the state the game's
	// page faults leave behind, so a replay loop exercises the same dirty-upload path.
	void                   MarkRegionAsCpuModified(uint64_t vaddr, uint64_t size);
	void                   ReadMemory(uint64_t vaddr, uint64_t size, bool is_write = false);
	[[nodiscard]] Buffer&  GetBuffer(BufferId id) { return m_slot_buffers[id]; }
	[[nodiscard]] BufferId FindBuffer(uint64_t vaddr, uint64_t size);
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBuffer(uint64_t vaddr, uint64_t size,
	                                                        bool     is_written,
	                                                        bool     is_texel_buffer = false,
	                                                        BufferId id              = {});
	[[nodiscard]] StreamBuffer&                GetUtilityBuffer(MemoryUsage usage) noexcept {
		switch (usage) {
			case MemoryUsage::Upload: return m_staging_buffer;
			case MemoryUsage::Stream: return m_stream_buffer;
			case MemoryUsage::Download: return m_download_buffer;
			case MemoryUsage::DeviceLocal: return m_device_buffer;
		}
		EXIT("BufferCache: invalid utility-buffer usage\n");
	}
	[[nodiscard]] const Buffer* GetGdsBuffer() const noexcept { return &m_gds_buffer; }
	[[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept { return &m_bda_pagetable_buffer; }
	[[nodiscard]] Buffer* GetFaultBuffer() noexcept { return m_fault_manager.GetFaultBuffer(); }
	[[nodiscard]] Buffer* GetDescriptorFeedbackBuffer() {
		return m_descriptor_feedback.GetBuffer();
	}
	void SetDescriptorFeedbackSink(Common::UniqueFunction<void, uint32_t>&& sink) {
		m_descriptor_feedback.SetSink(std::move(sink));
	}
	[[nodiscard]] std::pair<Buffer*, uint64_t> ObtainBufferForImage(uint64_t vaddr, uint64_t size);
	void FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds);
	void CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
	                bool src_gds);
	// Cache-index and exact dirty-range queries require GPU-thread serialization.
	[[nodiscard]] bool IsRegionRegistered(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool HasGpuDirtyBytes(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionCpuModified(uint64_t vaddr, uint64_t size);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t vaddr, uint64_t size);
	void               ProcessFaultBuffer();
	// Stage 1 diagnostics (docs/gpu-descriptor-fetch.md): everything the caches know about one
	// guest address, for a materialization failure to print before it exits. Read-only: it never
	// creates or touches a buffer.
	struct AddressProbe {
		uint64_t address        = 0;
		uint64_t page           = 0;
		bool     page_mapped    = false; // a BDA page-table entry exists for the 16 KiB page
		bool     buffer_found   = false;
		uint64_t buffer_start   = 0;
		uint64_t buffer_size    = 0;
		bool     buffer_deleted = false;
		bool     registered     = false; // a cached buffer covers the dword
		bool     gpu_modified   = false; // memory tracker: the GPU wrote this page
		bool     cpu_modified   = false;
		bool     gpu_dirty      = false; // m_gpu_modified_ranges: unread GPU writes
		FaultManager::FaultPageStatus fault {};
	};
	[[nodiscard]] AddressProbe ProbeAddress(uint64_t address);

	// Stage 1 diagnostics: the last few hundred shader stages that could have written guest
	// memory, so a stale read can be traced to a writer. Recorded only while --gpu-descriptors
	// is on. A uses_dma / gpu_descriptors stage stores through the BDA page table and has no
	// CPU-visible range, so it is recorded with size 0.
	struct ShaderWrite {
		uint64_t hash    = 0;
		uint64_t address = 0;
		uint64_t size    = 0;
		uint64_t serial  = 0;
		uint32_t stage   = 0;
		bool     dma     = false;
		bool     formatted = false;
		// The stage reads some of its own buffer descriptors in-shader through the BDA page
		// table, so an unmapped page makes it read zero instead of failing.
		bool gpu_fetch = false;
	};
	void RecordShaderWrite(uint64_t hash, uint32_t stage, uint64_t address, uint64_t size,
	                       bool dma, bool formatted, bool gpu_fetch);
	// Writers whose range covers the address, newest first, plus the newest DMA stages.
	[[nodiscard]] std::vector<ShaderWrite> FindShaderWrites(uint64_t address,
	                                                        size_t   dma_tail = 8) const;
	// Reads back and clears the shader descriptor-feedback bits, on the fault-buffer schedule.
	void               ProcessDescriptorFeedback();
	void               SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size);
	void               RunGarbageCollector();
	// Frame capture (docs/frame-replay.md): writes every GPU-modified byte of every cached buffer
	// back to guest memory and clears the GPU ownership, keeping the buffers. GPU thread only.
	// Returns the number of bytes written back.
	uint64_t FlushGpuModifiedMemory();
	// Frame capture: the CPU-dirty set of the range, without clearing it.
	template <typename Func>
	void ForEachCpuModifiedRange(uint64_t vaddr, uint64_t size, Func&& func) {
		m_memory_tracker.ForEachCpuModifiedRange(vaddr, size, std::forward<Func>(func));
	}
	[[nodiscard]] uint64_t BdaGeneration() const noexcept {
		return m_bda_generation.load(std::memory_order_acquire);
	}
	// Records a range whose guest bytes may no longer match the cached buffers and advances the
	// generation. Callers that only retire buffers use BumpBdaGeneration instead: a deleted
	// buffer has nothing to upload, and a later lookup registers and dirties its replacement.
	void                   InvalidateBda(uint64_t vaddr, uint64_t size);
	void                   ForgetBdaRange(uint64_t vaddr, uint64_t size);
	[[nodiscard]] RangeSet TakeBdaDirtyRanges();

private:
	friend struct BufferCacheTestAccess;

	using BufferMap = std::map<uint64_t, BufferId>;
	struct OverlapResult {
		BufferMap::iterator first;
		BufferMap::iterator last;
		uint64_t            begin;
		uint64_t            end;
		bool                has_stream_leap;
	};

	struct DownloadCopy;
	using PageTable = MultiLevelPageTable<BufferId, CACHING_PAGEBITS, 40, 16>;
	static_assert(CACHING_PAGESIZE == (uint64_t {1} << PageTable::kPageBits));
	static constexpr uint64_t               DOWNLOAD_ALIGNMENT = 64;
	[[nodiscard]] static constexpr uint64_t AlignDownload(uint64_t size) noexcept {
		return (size + DOWNLOAD_ALIGNMENT - 1) & ~(DOWNLOAD_ALIGNMENT - 1);
	}
	[[nodiscard]] static std::pair<uint64_t, uint64_t> DownloadEnvelope(const DownloadCopy& copy);
	void WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source, uint64_t size);
	void TouchBuffer(const Buffer& buffer);
	[[nodiscard]] OverlapResult ResolveOverlaps(uint64_t vaddr, uint64_t size);
	void JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score);
	[[nodiscard]] BufferId CreateBuffer(uint64_t vaddr, uint64_t size);
	void                   Register(BufferId id);
	void Unregister(BufferId id);
	template <bool insert>
	void ChangeRegister(BufferId id);
	void DeleteBuffer(BufferId id);
	[[nodiscard]] bool SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size,
	                                     bool is_written, bool is_texel_buffer);
	[[nodiscard]] vk::Buffer UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
	                                      uint64_t total_size);
	[[nodiscard]] bool SynchronizeBufferFromImage(Buffer& buffer, uint64_t vaddr, uint64_t size);
	void DownloadBufferMemory(std::span<const DownloadCopy> copies);
	void ReadMemoryOnGpu(uint64_t vaddr, uint64_t size, bool is_write);
	// The helper thread's landing: the pages it protected go back in the BDA dirty set and the
	// generation moves once for the whole batch, which is what makes the next scan upload them a
	// second time. Runs on the helper thread.
	void OnAsyncProtectLanded(const GuestRange* landed, size_t count);

	void BumpBdaGeneration() noexcept {
		m_bda_generation.fetch_add(1, std::memory_order_release);
	}

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	DescriptorFeedback                                m_descriptor_feedback;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	Common::SlotVector<Buffer>                        m_slot_buffers;
	Common::LeastRecentlyUsedCache<BufferId, uint64_t> m_lru_cache;
	BufferMap                                         m_buffers;
	PageTable                                         m_page_table;
	RangeSet                                          m_gpu_modified_ranges;
	MemoryTracker                                     m_memory_tracker;
	StreamBuffer                                      m_staging_buffer;
	StreamBuffer                                      m_stream_buffer;
	StreamBuffer                                      m_download_buffer;
	StreamBuffer                                      m_device_buffer;
	TextureCache&                                     m_texture_cache;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t m_trigger_gc_memory  = 1ull * 1024 * 1024 * 1024;
	uint64_t m_critical_gc_memory = 2ull * 1024 * 1024 * 1024;
	uint64_t m_gc_tick            = 0;
	std::atomic_uint64_t m_bda_generation {0};
	// Guest ranges dirtied since the last BDA preparation. Written from any guest thread that
	// faults or invalidates memory, drained on the GPU thread.
	std::mutex m_bda_dirty_mutex;
	RangeSet   m_bda_dirty_ranges;
	static constexpr size_t             ShaderWriteRingSize = 4096;
	std::vector<ShaderWrite>            m_shader_writes;
	uint64_t                            m_shader_write_next = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BUFFERCACHE_H_
