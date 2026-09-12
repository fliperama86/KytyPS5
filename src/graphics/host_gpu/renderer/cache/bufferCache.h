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
#include "graphics/host_gpu/renderer/cache/guestMemoryImport.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
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
	// Diagnostic (docs/sync-points-design.md, step 2 fact check): what the prologue table and the
	// tracker say about the page holding vaddr. Reads the last constructed cache.
	static void DiagnosePage(uint64_t vaddr, uint64_t* entry, bool* registered, bool* gpu_modified,
	                         uint32_t* import_value, bool* import_readable, uint32_t* mirror_value,
	                         bool* mirror_readable, bool* cpu_modified);
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
	// Step 1 of docs/sync-points-design.md, --gpu-prologue-table. The prologue table is the upper
	// half of the same buffer as the data table, one entry per 16 KiB page: for a page a
	// registered buffer covers it holds that buffer's mirror, exactly as the data table does, and
	// for every other mapped page the page itself inside imported guest memory, so a descriptor
	// or SRT read in a shader prologue can never miss. One buffer and one descriptor either way.
	[[nodiscard]] bool PrologueTableEnabled() const noexcept { return m_prologue_enabled; }
	// Imports the committed guest range and fills its prologue entries; releases them again
	// before the guest decommits. Both are GPU-thread only and need a recording command buffer.
	void ImportGuestRange(uint64_t vaddr, uint64_t size);
	// Imports the guest's backing-store alias once, and points the prologue entries of one of its
	// views into it. Both are GPU-thread only and need a recording command buffer.
	bool ImportBackingAlias(uint64_t base, uint64_t size);
	void MapBackingView(uint64_t vaddr, uint64_t size, uint64_t backing_offset);
	[[nodiscard]] bool BackingAliasImported() const noexcept {
		return m_guest_import != nullptr && m_guest_import->BackingAliasImported();
	}
	// One line about what the driver took and refused, for the import pass to print.
	[[nodiscard]] std::string DescribeGuestImports() const;
	// Prologue reads that found no entry since the last fault-processing pass. Zero is the whole
	// point of step 1, so the first few are reported loudly.
	void NotePrologueMisses(uint32_t misses);
	// Step 2: dispatches of a side-effect program the prologue skipped because one of its roots
	// did not evaluate. Zero is the whole point, so the first few are reported loudly too.
	void NoteGpuFetchSkips(uint32_t skips);
	void ReleaseGuestRange(uint64_t vaddr, uint64_t size);
	// Records the prologue entries that changed since the last call, so they are in the command
	// buffer before the draws that read them. One predictable branch when nothing changed.
	void FlushPrologueTable() {
		if (!m_prologue_pending.empty()) {
			FlushPrologueTableImpl();
		}
	}
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
	// producer_tick != 0 asks for item 1b's path (docs/performance-roadmap.md): the copy goes on
	// its own command buffer, submitted behind that tick alone, instead of draining everything
	// the render thread has recorded since the last drain.
	void DownloadBufferMemory(std::span<const DownloadCopy> copies, uint64_t producer_tick = 0);
	void ReadMemoryOnGpu(uint64_t vaddr, uint64_t size, bool is_write);
	// The helper thread's landing: the pages it protected go back in the BDA dirty set and the
	// generation moves once for the whole batch, which is what makes the next scan upload them a
	// second time. Runs on the helper thread.
	void OnAsyncProtectLanded(const GuestRange* landed, size_t count);

	// A page a registered buffer covers reads that buffer's mirror, which is what the data table
	// says and what the BDA scan keeps up to date; every other mapped page reads the import. A
	// page that is neither keeps today's answer, zero, which faults and reads zero.
	[[nodiscard]] uint64_t PrologueEntryForPage(uint64_t page);
	void                   QueuePrologueEntries(uint64_t vaddr, uint64_t size);
	void                   FlushPrologueTableImpl();
	// Byte offset of a page's entry in the prologue half of the page-table buffer.
	[[nodiscard]] static constexpr uint64_t PrologueEntryOffset(uint64_t page) noexcept {
		return BDA_PAGETABLE_SIZE + page * sizeof(vk::DeviceAddress);
	}
	void                   WritePrologueImportRun(const GuestMemoryImport::Range& range);

	void BumpBdaGeneration() noexcept {
		m_bda_generation.fetch_add(1, std::memory_order_release);
	}

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	FaultManager                                      m_fault_manager;
	DescriptorFeedback                                m_descriptor_feedback;
	Buffer                                            m_gds_buffer;
	Buffer                                            m_bda_pagetable_buffer;
	// --gpu-prologue-table only (docs/sync-points-design.md, step 1).
	bool                                              m_prologue_enabled = false;
	std::unique_ptr<GuestMemoryImport>                m_guest_import;
	// Entries changed since the last flush, page to device address, coalesced by page.
	std::map<uint64_t, uint64_t>                      m_prologue_pending;
	std::array<uint64_t, 2>                           m_prologue_fault_diag {};
	// Step 2: total skips and the number of reports already printed.
	std::array<uint64_t, 2>                           m_gpu_fetch_skip_diag {};
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
	// Item 1b (docs/performance-roadmap.md): which submission last handed each 64 KiB block to
	// the GPU for writing. Direct-mapped and lossy on purpose -- a collision evicts the other
	// block's entry, and a block with no entry disqualifies the readback, so a miss costs the
	// fast path and never correctness. m_wide_gpu_write_tick is the newest tick of a write too
	// large to record block by block; a producer older than it cannot be trusted to be the last
	// writer, so the fast path requires the producer to be at or after it.
	static constexpr size_t   GpuWriteBlockBits  = 16;
	static constexpr size_t   GpuWriteSlots      = 16384;
	static constexpr uint64_t GpuWriteBlockLimit = 8192;
	struct GpuWriteSlot {
		uint64_t block = UINT64_MAX;
		uint64_t tick  = 0;
	};
	std::array<GpuWriteSlot, GpuWriteSlots> m_gpu_write_ticks {};
	uint64_t                                m_wide_gpu_write_tick = 0;
	// The table is kept only for the two settings that read it, so main pays nothing for it.
	bool                                    m_track_gpu_write_ticks = false;
	void     RecordGpuWriteTick(uint64_t vaddr, uint64_t size, uint64_t tick) noexcept;
	// The newest submission that wrote any part of the range, or 0 when that is not knowable.
	// reason, when the answer is 0: 1 a copy too wide to look up, 2 a block with no entry,
	// 3 no block at all, 4 an unrecorded wide write is newer than what the table holds.
	[[nodiscard]] uint64_t FindGpuWriteTick(std::span<const DownloadCopy> copies,
	                                       uint32_t* reason = nullptr) const noexcept;
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
