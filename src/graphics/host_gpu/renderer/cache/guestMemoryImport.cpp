#include "graphics/host_gpu/renderer/cache/guestMemoryImport.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <utility>

namespace Libs::Graphics {

namespace {

using Clock = std::chrono::steady_clock;

std::atomic_uint64_t g_import_ranges {0};
std::atomic_uint64_t g_import_bytes {0};
std::atomic_uint64_t g_imports {0};
std::atomic_uint64_t g_import_releases {0};
std::atomic_uint64_t g_import_failures {0};
std::atomic_uint64_t g_import_ns {0};
std::atomic_uint64_t g_entry_writes {0};
std::atomic_uint64_t g_data_fault_pages {0};
std::atomic_uint64_t g_prologue_fault_pages {0};
std::atomic_uint64_t g_side_effect_skips {0};
std::atomic_uint64_t g_compute_clears {0};
std::atomic_uint64_t g_compute_clear_refused {0};

} // namespace

BdaPrologueCounters ReadBdaPrologueCounters() noexcept {
	BdaPrologueCounters counters;
	counters.import_ranges        = g_import_ranges.load(std::memory_order_relaxed);
	counters.import_bytes         = g_import_bytes.load(std::memory_order_relaxed);
	counters.imports              = g_imports.load(std::memory_order_relaxed);
	counters.import_releases      = g_import_releases.load(std::memory_order_relaxed);
	counters.import_failures      = g_import_failures.load(std::memory_order_relaxed);
	counters.import_ns            = g_import_ns.load(std::memory_order_relaxed);
	counters.entry_writes         = g_entry_writes.load(std::memory_order_relaxed);
	counters.data_fault_pages     = g_data_fault_pages.load(std::memory_order_relaxed);
	counters.prologue_fault_pages = g_prologue_fault_pages.load(std::memory_order_relaxed);
	counters.side_effect_skips    = g_side_effect_skips.load(std::memory_order_relaxed);
	counters.compute_clears       = g_compute_clears.load(std::memory_order_relaxed);
	counters.compute_clear_refused = g_compute_clear_refused.load(std::memory_order_relaxed);
	return counters;
}

// The per-loop counters the replay samples; the import totals stay cumulative.
void ResetBdaPrologueFrameCounters() noexcept {
	g_entry_writes.store(0, std::memory_order_relaxed);
	g_data_fault_pages.store(0, std::memory_order_relaxed);
	g_prologue_fault_pages.store(0, std::memory_order_relaxed);
	g_side_effect_skips.store(0, std::memory_order_relaxed);
	g_compute_clears.store(0, std::memory_order_relaxed);
	g_compute_clear_refused.store(0, std::memory_order_relaxed);
}

void NoteBdaPrologueEntryWrites(uint64_t count) noexcept {
	g_entry_writes.fetch_add(count, std::memory_order_relaxed);
}

void NoteBdaFaultPages(uint64_t pages, bool prologue) noexcept {
	(prologue ? g_prologue_fault_pages : g_data_fault_pages)
	    .fetch_add(pages, std::memory_order_relaxed);
}

void NoteGpuFetchSkips(uint64_t skips) noexcept {
	g_side_effect_skips.fetch_add(skips, std::memory_order_relaxed);
}

void NoteComputeClear(bool consumed) noexcept {
	(consumed ? g_compute_clears : g_compute_clear_refused)
	    .fetch_add(1, std::memory_order_relaxed);
}

GuestMemoryImport::GuestMemoryImport(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	m_available = graphics.external_memory_host_enabled &&
	              VULKAN_HPP_DEFAULT_DISPATCHER.vkGetMemoryHostPointerPropertiesEXT != nullptr;
	m_alignment = graphics.imported_host_pointer_alignment != 0
	                  ? graphics.imported_host_pointer_alignment
	                  : 4096;
	if (!m_available && Config::GpuPrologueTableEnabled()) {
		std::printf("gpu-prologue-table: guest memory cannot be imported on this device; "
		            "prologue reads keep the mirror and the fault path\n");
		std::fflush(stdout);
	}
}

GuestMemoryImport::~GuestMemoryImport() {
	for (auto& [address, entry]: m_imports) {
		(void)address;
		if (entry.buffer != nullptr) {
			m_graphics.device.destroyBuffer(entry.buffer, nullptr);
		}
		if (entry.memory != nullptr) {
			m_graphics.device.freeMemory(entry.memory, nullptr);
		}
	}
	m_imports.clear();
	if (m_alias_buffer != nullptr) {
		m_graphics.device.destroyBuffer(m_alias_buffer, nullptr);
	}
	if (m_alias_memory != nullptr) {
		m_graphics.device.freeMemory(m_alias_memory, nullptr);
	}
}

bool GuestMemoryImport::ImportBackingAlias(uint64_t base, uint64_t size) {
	if (!m_available || m_alias_base != 0 || base == 0 || size == 0) {
		return m_alias_base != 0;
	}
	const auto address = ImportRange(base, size);
	if (address == 0) {
		return false;
	}
	// The alias lives for the process; take it out of the address-keyed map so nothing releases
	// it and no guest lookup ever lands on it.
	const auto found = m_imports.find(base);
	if (found == m_imports.end()) {
		return false;
	}
	m_alias_memory = found->second.memory;
	m_alias_buffer = found->second.buffer;
	m_alias_base   = found->second.base;
	m_alias_size   = found->second.size;
	m_imports.erase(found);
	m_counters.ranges--;
	return true;
}

vk::DeviceAddress GuestMemoryImport::AddressOf(uint64_t vaddr) const noexcept {
	if (m_imports.empty()) {
		return 0;
	}
	auto it = m_imports.upper_bound(vaddr);
	if (it == m_imports.begin()) {
		return 0;
	}
	--it;
	if (vaddr - it->first >= it->second.size) {
		return 0;
	}
	return it->second.base + (vaddr - it->first);
}

// A span of one guest mapping, cut into chunks the driver will take. The chunk size is global and
// only shrinks: the first refusal halves it, so a run converges after a few tries instead of
// paying a failed allocation for every large range.
std::vector<GuestMemoryImport::Range> GuestMemoryImport::ImportSpan(uint64_t vaddr,
                                                                    uint64_t size) {
	constexpr uint64_t GuestPageSize = 16384;
	std::vector<Range> imported;
	if (!m_available || size == 0) {
		return imported;
	}
	if ((vaddr % GuestPageSize) != 0 || (size % GuestPageSize) != 0) {
		NoteFailure(vaddr, size, "alignment", vk::Result::eErrorUnknown);
		return imported;
	}
	const auto base = ImportRange(vaddr, size);
	if (base == 0) {
		m_skipped_bytes += size;
		return imported;
	}
	m_largest_success = std::max(m_largest_success, size);
	imported.push_back({vaddr, size, base});
	return imported;
}

void GuestMemoryImport::NoteFailure(uint64_t vaddr, uint64_t size, const char* what,
                                    vk::Result result) {
	m_counters.failures++;
	g_import_failures.fetch_add(1, std::memory_order_relaxed);
	constexpr uint64_t MaxReported = 4;
	if (m_reported_failures >= MaxReported) {
		return;
	}
	m_reported_failures++;
	m_smallest_failure = m_smallest_failure == 0 ? size : std::min(m_smallest_failure, size);
	std::printf("gpu-prologue-table: %s failed at 0x%016llx size 0x%llx (%s); those pages keep "
	            "the mirror and the fault path\n",
	            what, static_cast<unsigned long long>(vaddr),
	            static_cast<unsigned long long>(size), vk::to_string(result).c_str());
	std::fflush(stdout);
}

vk::DeviceAddress GuestMemoryImport::ImportRange(uint64_t vaddr, uint64_t size) {
	if (!m_available || size == 0) {
		return 0;
	}
	// The host pointer and the length both have to be aligned for the import, and the range has
	// to stay inside what the guest committed: a guest range is at least 16 KiB aligned, so this
	// only ever rejects a malformed one.
	if ((vaddr % m_alignment) != 0 || (size % m_alignment) != 0) {
		NoteFailure(vaddr, size, "alignment", vk::Result::eErrorUnknown);
		return 0;
	}

	const auto started = Clock::now();

	vk::MemoryHostPointerPropertiesEXT host_properties {};
	const auto properties_result = m_graphics.device.getMemoryHostPointerPropertiesEXT(
	    vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
	    reinterpret_cast<const void*>(vaddr), &host_properties);
	if (properties_result != vk::Result::eSuccess || host_properties.memoryTypeBits == 0) {
		NoteFailure(vaddr, size, "host-pointer properties", properties_result);
		return 0;
	}

	vk::ExternalMemoryBufferCreateInfo external {};
	external.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;

	vk::BufferCreateInfo buffer_info {};
	buffer_info.pNext       = &external;
	buffer_info.size        = size;
	buffer_info.usage       = vk::BufferUsageFlagBits::eShaderDeviceAddress |
	                          vk::BufferUsageFlagBits::eTransferSrc;
	buffer_info.sharingMode = vk::SharingMode::eExclusive;

	Entry entry {};
	entry.size = size;
	if (const auto created = m_graphics.device.createBuffer(&buffer_info, nullptr, &entry.buffer);
	    created != vk::Result::eSuccess) {
		NoteFailure(vaddr, size, "buffer creation", created);
		return 0;
	}

	const auto requirements = m_graphics.device.getBufferMemoryRequirements(entry.buffer);
	const auto usable = requirements.memoryTypeBits & host_properties.memoryTypeBits;
	// System memory, not a host-visible slice of VRAM: the import is guest memory the CPU keeps
	// writing, and the bench measured it on the plain HOST_VISIBLE | HOST_COHERENT type.
	const auto& memory = m_graphics.physical_device_memory_properties;
	uint32_t    type   = UINT32_MAX;
	for (uint32_t pass = 0; pass < 2 && type == UINT32_MAX; ++pass) {
		for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
			if ((usable & (1u << i)) == 0) {
				continue;
			}
			const auto flags = memory.memoryTypes[i].propertyFlags;
			if (pass == 0 &&
			    static_cast<bool>(flags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
				continue;
			}
			type = i;
			break;
		}
	}
	if (!m_reported_type && type != UINT32_MAX) {
		m_reported_type = true;
		std::printf("gpu-prologue-table: importing into memory type %u (heap %u, %s)\n", type,
		            memory.memoryTypes[type].heapIndex,
		            vk::to_string(memory.memoryTypes[type].propertyFlags).c_str());
		std::fflush(stdout);
	}
	if (type == UINT32_MAX) {
		m_graphics.device.destroyBuffer(entry.buffer, nullptr);
		NoteFailure(vaddr, size, "memory type", vk::Result::eErrorUnknown);
		return 0;
	}

	vk::ImportMemoryHostPointerInfoEXT import {};
	import.handleType   = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;
	import.pHostPointer = reinterpret_cast<void*>(vaddr);

	vk::MemoryAllocateFlagsInfo flags {};
	flags.pNext = &import;
	flags.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;

	vk::MemoryAllocateInfo allocate {};
	allocate.pNext           = &flags;
	allocate.allocationSize  = size;
	allocate.memoryTypeIndex = type;
	if (const auto allocated = m_graphics.device.allocateMemory(&allocate, nullptr, &entry.memory);
	    allocated != vk::Result::eSuccess) {
		m_graphics.device.destroyBuffer(entry.buffer, nullptr);
		NoteFailure(vaddr, size, "import", allocated);
		return 0;
	}
	if (const auto bound = m_graphics.device.bindBufferMemory(entry.buffer, entry.memory, 0);
	    bound != vk::Result::eSuccess) {
		m_graphics.device.destroyBuffer(entry.buffer, nullptr);
		m_graphics.device.freeMemory(entry.memory, nullptr);
		NoteFailure(vaddr, size, "bind", bound);
		return 0;
	}

	vk::BufferDeviceAddressInfo address_info {};
	address_info.buffer = entry.buffer;
	entry.base          = m_graphics.device.getBufferAddress(address_info);
	if (entry.base == 0) {
		m_graphics.device.destroyBuffer(entry.buffer, nullptr);
		m_graphics.device.freeMemory(entry.memory, nullptr);
		NoteFailure(vaddr, size, "device address", vk::Result::eErrorUnknown);
		return 0;
	}

	m_counters.import_ns += static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
	m_counters.imports++;
	m_counters.ranges++;
	m_counters.bytes += size;
	g_imports.fetch_add(1, std::memory_order_relaxed);
	g_import_ranges.fetch_add(1, std::memory_order_relaxed);
	g_import_bytes.fetch_add(size, std::memory_order_relaxed);
	g_import_ns.store(m_counters.import_ns, std::memory_order_relaxed);
	const auto base = entry.base;
	m_imports.emplace(vaddr, entry);
	return base;
}

void GuestMemoryImport::DestroyEntry(Entry& entry) {
	const auto started = Clock::now();
	m_counters.releases++;
	if (m_counters.ranges != 0) {
		m_counters.ranges--;
	}
	m_counters.bytes -= std::min(m_counters.bytes, entry.size);
	g_import_releases.fetch_add(1, std::memory_order_relaxed);
	if (g_import_ranges.load(std::memory_order_relaxed) != 0) {
		g_import_ranges.fetch_sub(1, std::memory_order_relaxed);
	}
	g_import_bytes.fetch_sub(std::min(g_import_bytes.load(std::memory_order_relaxed), entry.size),
	                         std::memory_order_relaxed);
	// The caller has already cleared the page-table entries that pointed here, but a submission
	// recorded before that may still be reading them, so the objects go away only once the
	// scheduler says the tick that carried those reads has retired. The guest cannot decommit
	// the pages before that: the unmap path waits for the device first.
	const auto buffer = entry.buffer;
	const auto memory = entry.memory;
	const auto device = m_graphics.device;
	entry.buffer      = nullptr;
	entry.memory      = nullptr;
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([device, buffer, memory] {
			if (buffer != nullptr) {
				device.destroyBuffer(buffer, nullptr);
			}
			if (memory != nullptr) {
				device.freeMemory(memory, nullptr);
			}
		});
	} else {
		if (buffer != nullptr) {
			device.destroyBuffer(buffer, nullptr);
		}
		if (memory != nullptr) {
			device.freeMemory(memory, nullptr);
		}
	}
	m_counters.release_ns += static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count());
}

} // namespace Libs::Graphics
