#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GUESTMEMORYIMPORT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GUESTMEMORYIMPORT_H_

// Step 1 of docs/sync-points-design.md: committed guest memory imported with
// VK_EXT_external_memory_host so the GPU can read a guest page where the guest put it.
//
// One VkDeviceMemory and one VkBuffer per imported range, keyed by guest address. The device
// address of a guest byte is then base + (vaddr - range start), which is what the prologue BDA
// page table holds by default. Only the prologue reads through it: the bench
// (docs/investigations/bda-host-memory-bench-2026-09-12.md) says data reads from an import are
// 300 to 400 times slower than from the VRAM mirror, so nothing else may point here.
//
// Imports follow the resource manager's mapped ranges, which follow the kernel's commits, and an
// import is released before the guest decommits the pages it covers: GpuResourceManager::MapMemory
// asks for one and UnmapMemory releases it, both on the GPU thread.

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class CommandScheduler;

// Step 1 counters, process wide, for the replay report and one line at start-up. The fault pages
// are the two BDA page tables' misses kept apart: the data table's fault buffer registers a page a
// shader body could not read, the prologue table's one a descriptor or SRT read could not.
struct BdaPrologueCounters {
	uint64_t import_ranges        = 0; // imports alive
	uint64_t import_bytes         = 0;
	uint64_t imports              = 0; // imports made
	uint64_t import_releases      = 0;
	uint64_t import_failures      = 0;
	uint64_t import_ns            = 0;
	uint64_t entry_writes         = 0; // prologue page-table entries written
	uint64_t data_fault_pages     = 0;
	uint64_t prologue_fault_pages = 0;
};

[[nodiscard]] BdaPrologueCounters ReadBdaPrologueCounters() noexcept;
void                              ResetBdaPrologueFrameCounters() noexcept;
void                              NoteBdaPrologueEntryWrites(uint64_t count) noexcept;
void                              NoteBdaFaultPages(uint64_t pages, bool prologue) noexcept;

class GuestMemoryImport {
public:
	struct Counters {
		uint64_t ranges     = 0; // covered ranges alive, allocations and views together
		uint64_t bytes      = 0; // bytes of the allocations alive
		uint64_t imports    = 0; // allocations made since the process started
		uint64_t releases   = 0;
		uint64_t failures   = 0;
		uint64_t import_ns  = 0;
		uint64_t release_ns = 0;
	};

	struct Range {
		uint64_t          address = 0; // guest address of the first byte
		uint64_t          size    = 0;
		vk::DeviceAddress base    = 0; // device address of that byte
	};

	GuestMemoryImport(GraphicContext& graphics, CommandScheduler& scheduler);
	~GuestMemoryImport();
	KYTY_CLASS_NO_COPY(GuestMemoryImport);

	[[nodiscard]] bool Available() const noexcept { return m_available; }

	// Imports the writable alias of the guest's direct-memory backing store, once. Every direct,
	// flexible and pooled guest mapping is a view of it, so one allocation covers all of them --
	// and the driver takes one 13.5 GiB import where it refuses a few hundred MiB of separate
	// views. Returns false when it is unavailable or refused, and the caller then falls back to
	// importing the mappings themselves.
	bool ImportBackingAlias(uint64_t base, uint64_t size);
	[[nodiscard]] bool BackingAliasImported() const noexcept { return m_alias_base != 0; }

	// Records a view of that alias as covering [vaddr, vaddr + size): the guest bytes are at
	// alias base + backing_offset and no allocation of its own is needed. Reports the part that
	// was not covered before, so the caller can fill its page-table entries.
	template <typename Func>
	void AddBackingView(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                    Func&& on_added) {
		if (m_alias_base == 0 || size == 0 || backing_offset > m_alias_size ||
		    size > m_alias_size - backing_offset) {
			return;
		}
		std::vector<Range> gaps;
		ForEachGap(vaddr, size, [&](uint64_t gap, uint64_t gap_size) {
			gaps.push_back({gap, gap_size, 0});
		});
		for (const auto& gap: gaps) {
			Entry entry {};
			entry.size = gap.size;
			entry.base = m_alias_base + backing_offset + (gap.address - vaddr);
			m_imports.emplace(gap.address, entry);
			m_counters.ranges++;
			m_views++;
			on_added(Range {gap.address, gap.size, entry.base});
		}
	}

	// Imports the part of [vaddr, vaddr + size) that no import covers yet. Every newly covered
	// sub-range is reported so the caller can fill its page-table entries. GPU thread only.
	template <typename Func>
	void Import(uint64_t vaddr, uint64_t size, Func&& on_imported) {
		std::vector<Range> gaps;
		ForEachGap(vaddr, size, [&](uint64_t gap, uint64_t gap_size) {
			gaps.push_back({gap, gap_size, 0});
		});
		for (const auto& gap: gaps) {
			for (const auto& imported: ImportSpan(gap.address, gap.size)) {
				on_imported(imported);
			}
		}
	}

	// Destroys every import that intersects [vaddr, vaddr + size), reporting each whole released
	// range so the caller can clear its entries. An import is destroyed as a whole even when the
	// range only clips it; the caller re-imports whatever is still mapped. The Vulkan objects go
	// through the scheduler's deferred queue, so the device is done with them before the driver
	// gives up the host pages. GPU thread only.
	template <typename Func>
	void Release(uint64_t vaddr, uint64_t size, Func&& on_released) {
		if (size == 0 || m_imports.empty()) {
			return;
		}
		const auto end = vaddr + size;
		auto       it  = m_imports.lower_bound(vaddr);
		if (it != m_imports.begin() && std::prev(it)->first + std::prev(it)->second.size > vaddr) {
			it = std::prev(it);
		}
		while (it != m_imports.end() && it->first < end) {
			const Range released {it->first, it->second.size, it->second.base};
			DestroyEntry(it->second);
			it = m_imports.erase(it);
			on_released(released);
		}
	}

	// Device address of a guest byte, or 0 when nothing imported it.
	[[nodiscard]] vk::DeviceAddress AddressOf(uint64_t vaddr) const noexcept;

	[[nodiscard]] const Counters& GetCounters() const noexcept { return m_counters; }
	// What the driver took and refused, so one line can say why coverage is short.
	[[nodiscard]] uint64_t LargestImport() const noexcept { return m_largest_success; }
	[[nodiscard]] uint64_t SmallestRefused() const noexcept { return m_smallest_failure; }
	[[nodiscard]] uint64_t SkippedBytes() const noexcept { return m_skipped_bytes; }
	[[nodiscard]] uint64_t Views() const noexcept { return m_views; }
	[[nodiscard]] uint64_t AliasBytes() const noexcept { return m_alias_size; }

private:
	struct Entry {
		uint64_t          size   = 0;
		vk::DeviceMemory  memory = nullptr;
		vk::Buffer        buffer = nullptr;
		vk::DeviceAddress base   = 0;
	};

	template <typename Func>
	void ForEachGap(uint64_t vaddr, uint64_t size, Func&& func) {
		if (size == 0) {
			return;
		}
		const auto end    = vaddr + size;
		auto       cursor = vaddr;
		auto       it     = m_imports.lower_bound(vaddr);
		if (it != m_imports.begin() && std::prev(it)->first + std::prev(it)->second.size > vaddr) {
			it = std::prev(it);
		}
		for (; it != m_imports.end() && it->first < end; ++it) {
			if (it->first > cursor) {
				func(cursor, it->first - cursor);
			}
			cursor = std::max(cursor, it->first + it->second.size);
			if (cursor >= end) {
				return;
			}
		}
		if (cursor < end) {
			func(cursor, end - cursor);
		}
	}

	// One allocation per span, never a part of one: the driver refuses a partial import of a
	// mapping, and refuses most large ones outright, so a span the import pass hands over is
	// taken whole or not at all.
	[[nodiscard]] std::vector<Range> ImportSpan(uint64_t vaddr, uint64_t size);
	[[nodiscard]] vk::DeviceAddress  ImportRange(uint64_t vaddr, uint64_t size);
	void                            NoteFailure(uint64_t vaddr, uint64_t size, const char* what,
	                                            vk::Result result);
	void                            DestroyEntry(Entry& entry);

	GraphicContext&            m_graphics;
	CommandScheduler&          m_scheduler;
	std::map<uint64_t, Entry>  m_imports;
	Counters                   m_counters;
	bool                       m_available = false;
	// The backing-store alias: one allocation, kept for the life of the process.
	vk::DeviceMemory           m_alias_memory = nullptr;
	vk::Buffer                 m_alias_buffer = nullptr;
	vk::DeviceAddress          m_alias_base   = 0;
	uint64_t                   m_alias_size   = 0;
	uint64_t                   m_alignment = 0;
	// What the driver took and refused, for one line at the end of the import pass.
	uint64_t                   m_largest_success  = 0;
	uint64_t                   m_smallest_failure = 0;
	uint64_t                   m_skipped_bytes    = 0;
	uint64_t                   m_views            = 0;
	uint64_t                   m_reported_failures = 0;
	bool                       m_reported_type     = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_GUESTMEMORYIMPORT_H_
