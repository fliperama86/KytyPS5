// Standalone micro-benchmark: what does a shader memory read cost when it goes through the
// emulator's guest-address page table (buffer device address) instead of through a normally bound
// storage buffer?
//
// The shader in tests/bda_load_bench.comp reproduces the shape that
// src/graphics/shader/recompiler/backend/spirv/spirvEmitterMemory.cpp emits for a guest load. This
// program only drives it: it creates a headless Vulkan device, allocates a 256 MB "guest memory"
// buffer plus the 512 MB page table and 8 MB fault-bit buffer the emulator uses, maps every 16 KB
// page, and times dispatches with timestamp queries.
//
// The fixture that stands for guest memory can live in three places, selected with --fixture:
//
//   device-local  a plain VRAM allocation (the original bench, the baseline)
//   host-import   a VirtualAlloc block imported with VK_EXT_external_memory_host, which is what
//                 design A in docs/bda-sync-design.md would point the BDA page table at
//   host-visible  a plain HOST_VISIBLE | HOST_COHERENT allocation, the control for "same system
//                 memory reads, no import"
//
// The fill is identical in all three cases (the GPU writes it through the same transfer commands),
// so the bit-exact verification at the end checks the same constant everywhere.
//
// It deliberately shares nothing with the emulator runtime - Vulkan is loaded by hand, there is no
// window, no SDL and no instance layer.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#if defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	define NOMINMAX
#	include <windows.h>
#else
#	include <dlfcn.h>
#	include <sys/mman.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bda_load_bench_spv.h"

namespace {

// Mirrors BufferCache in src/graphics/host_gpu/renderer/cache/bufferCache.h.
constexpr uint32_t kPageBits       = 14;
constexpr uint64_t kPageSize       = uint64_t {1} << kPageBits;
constexpr uint64_t kNumPages       = uint64_t {1} << (40 - kPageBits);
constexpr uint64_t kPageTableBytes = kNumPages * sizeof(uint64_t); // 512 MB
constexpr uint64_t kFaultBytes     = kNumPages / 8;               // 8 MB

// The addressable region the access patterns walk, plus a tail that holds the descriptor chain so
// the scattered pattern never reads it back as data.
constexpr uint64_t kSpanBytes   = uint64_t {256} << 20;
constexpr uint64_t kChainOffset = kSpanBytes;
constexpr uint64_t kChainBytes  = 0x3000;
constexpr uint32_t kChainSlots  = 64;
constexpr uint64_t kGuestBytes  = kSpanBytes + (uint64_t {1} << 20);
constexpr uint64_t kGuestBase   = uint64_t {1} << 32; // guest address of the region
constexpr uint64_t kGuestPages  = kGuestBytes / kPageSize;
constexpr uint64_t kFirstPage   = kGuestBase / kPageSize;
constexpr uint32_t kSpanMask    = static_cast<uint32_t>(kSpanBytes - 1) & ~3u;
constexpr uint32_t kFillWord    = 0x01010101u;

constexpr uint32_t kWorkGroups   = 16384;
constexpr uint32_t kProbeGroups  = 1024; // small first probe, so a slow fixture cannot hit TDR
constexpr uint32_t kGroupThreads = 64;
constexpr uint32_t kLoadsPerIter = 16;
constexpr uint32_t kSamples      = 5;
constexpr uint32_t kQueryCount   = 4;
constexpr double   kTargetMs     = 30.0;

// chain-latency: one workgroup, a dependent chain of N loads.
constexpr uint32_t kChainN[2]      = {64, 1024};
constexpr uint32_t kChainGroups[4] = {1, 16, 128, 1024};
constexpr uint32_t kChainSamples   = 9;

// A chain of 1024 loads touches 1024 lines and about as many page table entries, which the GPU's
// L2 holds easily, so an untimed scattered dispatch runs first and evicts them: 8.4 M random loads
// over the 256 MB region. Pass 1 is then a cold read of the fixture and pass 2 a warm one.
constexpr uint32_t kEvictGroups = 8192;

// dispatch-split probe: same total work as one big dispatch, cut into many small ones. The
// iteration count is a quarter of the calibrated one so that the barrier-separated variant, which
// is tens of times slower, stays well inside the driver's timeout on a slow fixture.
constexpr uint32_t kSplitGroups   = 256;
constexpr uint32_t kSplitDispatch = kWorkGroups / kSplitGroups;
constexpr uint32_t kSplitDivisor  = 4;

constexpr int kTputPatterns = 3; // the first three are the throughput table

constexpr const char* kVariantName[3] = {"A ssbo", "B bda", "C bda+srt"};
constexpr const char* kPatternName[4] = {"uniform", "strided", "scattered", "chain"};

enum class FixtureKind
{
	DeviceLocal,
	HostImport,
	HostVisible
};

enum class MemoryChoice
{
	DeviceLocal,
	HostVisible
};

const char* FixtureName(FixtureKind kind) {
	switch (kind) {
		case FixtureKind::DeviceLocal: return "device-local";
		case FixtureKind::HostImport: return "host-import";
		case FixtureKind::HostVisible: return "host-visible";
	}
	return "?";
}

[[noreturn]] void Fail(const char* what, VkResult result = VK_SUCCESS) {
	std::fprintf(stderr, "bda_load_bench: %s (VkResult %d)\n", what, static_cast<int>(result));
	std::exit(1);
}

void Check(VkResult result, const char* what) {
	if (result != VK_SUCCESS) {
		Fail(what, result);
	}
}

void PrintMemoryFlags(VkMemoryPropertyFlags flags) {
	struct Named {
		VkMemoryPropertyFlagBits bit;
		const char*              name;
	};
	static const Named kNames[] = {
	    {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL"},
	    {VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE"},
	    {VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT"},
	    {VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HOST_CACHED"},
	    {VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LAZILY_ALLOCATED"},
	    {VK_MEMORY_PROPERTY_PROTECTED_BIT, "PROTECTED"},
	};
	bool first = true;
	for (const auto& named : kNames) {
		if ((flags & named.bit) != 0) {
			std::printf("%s%s", first ? "" : " | ", named.name);
			first = false;
		}
	}
	if (first) {
		std::printf("(none)");
	}
}

#define KYTY_INSTANCE_ENTRIES(X)                                                                   \
	X(vkEnumeratePhysicalDevices)                                                                  \
	X(vkEnumerateDeviceExtensionProperties)                                                        \
	X(vkGetPhysicalDeviceProperties2)                                                              \
	X(vkGetPhysicalDeviceFeatures2)                                                                \
	X(vkGetPhysicalDeviceQueueFamilyProperties)                                                    \
	X(vkGetPhysicalDeviceMemoryProperties)                                                         \
	X(vkCreateDevice)                                                                              \
	X(vkGetDeviceProcAddr)                                                                         \
	X(vkDestroyInstance)

#define KYTY_DEVICE_ENTRIES(X)                                                                     \
	X(vkGetDeviceQueue)                                                                            \
	X(vkCreateBuffer)                                                                              \
	X(vkDestroyBuffer)                                                                             \
	X(vkGetBufferMemoryRequirements)                                                               \
	X(vkAllocateMemory)                                                                            \
	X(vkFreeMemory)                                                                                \
	X(vkBindBufferMemory)                                                                          \
	X(vkGetBufferDeviceAddress)                                                                    \
	X(vkMapMemory)                                                                                 \
	X(vkCreateDescriptorSetLayout)                                                                 \
	X(vkCreatePipelineLayout)                                                                      \
	X(vkCreateShaderModule)                                                                        \
	X(vkDestroyShaderModule)                                                                       \
	X(vkCreateComputePipelines)                                                                    \
	X(vkDestroyPipeline)                                                                           \
	X(vkCreateDescriptorPool)                                                                      \
	X(vkAllocateDescriptorSets)                                                                    \
	X(vkUpdateDescriptorSets)                                                                      \
	X(vkCreateCommandPool)                                                                         \
	X(vkAllocateCommandBuffers)                                                                    \
	X(vkResetCommandBuffer)                                                                        \
	X(vkBeginCommandBuffer)                                                                        \
	X(vkEndCommandBuffer)                                                                          \
	X(vkCmdBindPipeline)                                                                           \
	X(vkCmdBindDescriptorSets)                                                                     \
	X(vkCmdPushConstants)                                                                          \
	X(vkCmdDispatch)                                                                               \
	X(vkCmdCopyBuffer)                                                                             \
	X(vkCmdFillBuffer)                                                                             \
	X(vkCmdPipelineBarrier)                                                                        \
	X(vkCmdResetQueryPool)                                                                         \
	X(vkCmdWriteTimestamp)                                                                         \
	X(vkCreateQueryPool)                                                                           \
	X(vkGetQueryPoolResults)                                                                       \
	X(vkCreateFence)                                                                               \
	X(vkResetFences)                                                                               \
	X(vkWaitForFences)                                                                             \
	X(vkQueueSubmit)                                                                               \
	X(vkDeviceWaitIdle)                                                                            \
	X(vkDestroyDevice)

#define KYTY_DECLARE(name) PFN_##name name = nullptr;
KYTY_INSTANCE_ENTRIES(KYTY_DECLARE)
KYTY_DEVICE_ENTRIES(KYTY_DECLARE)
#undef KYTY_DECLARE

PFN_vkGetInstanceProcAddr              vkGetInstanceProcAddr              = nullptr;
PFN_vkCreateInstance                   vkCreateInstance                   = nullptr;
PFN_vkGetMemoryHostPointerPropertiesEXT vkGetMemoryHostPointerPropertiesEXT = nullptr;

void LoadLoader() {
#if defined(_WIN32)
	HMODULE library = ::LoadLibraryA("vulkan-1.dll");
	if (library == nullptr) {
		Fail("vulkan-1.dll could not be loaded");
	}
	vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
	    ::GetProcAddress(library, "vkGetInstanceProcAddr"));
#else
	void* library = dlopen("libvulkan.so.1", RTLD_NOW);
	if (library == nullptr) {
		Fail("libvulkan.so.1 could not be loaded");
	}
	vkGetInstanceProcAddr =
	    reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library, "vkGetInstanceProcAddr"));
#endif
	if (vkGetInstanceProcAddr == nullptr) {
		Fail("vkGetInstanceProcAddr is missing");
	}
	vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
	    vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
	if (vkCreateInstance == nullptr) {
		Fail("vkCreateInstance is missing");
	}
}

// A block of committed host memory, aligned for VK_EXT_external_memory_host. The emulator's guest
// pages come from Common::VirtualMemory::Commit, which is the same VirtualAlloc underneath.
struct HostBlock {
	void*  reservation = nullptr;
	void*  base        = nullptr;
	size_t size        = 0;
};

HostBlock AllocateHostBlock(size_t bytes, size_t alignment) {
	HostBlock block {};
	if (alignment == 0) {
		alignment = 1;
	}
	const size_t rounded = (bytes + alignment - 1) & ~(alignment - 1);
#if defined(_WIN32)
	const size_t reserve = rounded + alignment;
	block.reservation    = ::VirtualAlloc(nullptr, reserve, MEM_RESERVE, PAGE_READWRITE);
	if (block.reservation == nullptr) {
		Fail("VirtualAlloc MEM_RESERVE failed");
	}
	auto address = reinterpret_cast<uintptr_t>(block.reservation);
	address      = (address + alignment - 1) & ~static_cast<uintptr_t>(alignment - 1);
	block.base   = ::VirtualAlloc(reinterpret_cast<void*>(address), rounded, MEM_COMMIT,
	                              PAGE_READWRITE);
	if (block.base == nullptr) {
		Fail("VirtualAlloc MEM_COMMIT failed");
	}
#else
	const size_t reserve = rounded + alignment;
	block.reservation    = mmap(nullptr, reserve, PROT_READ | PROT_WRITE,
	                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (block.reservation == MAP_FAILED) {
		Fail("mmap failed");
	}
	auto address = reinterpret_cast<uintptr_t>(block.reservation);
	address      = (address + alignment - 1) & ~static_cast<uintptr_t>(alignment - 1);
	block.base   = reinterpret_cast<void*>(address);
#endif
	block.size = rounded;
	std::memset(block.base, 0, rounded);
	return block;
}

struct Buffer {
	VkBuffer        handle  = VK_NULL_HANDLE;
	VkDeviceMemory  memory  = VK_NULL_HANDLE;
	VkDeviceSize    size    = 0;
	VkDeviceAddress address = 0;
	void*           mapped  = nullptr;
};

struct PassTimes {
	double pass1 = 0.0;
	double pass2 = 0.0;
};

struct Bench {
	VkInstance       instance = VK_NULL_HANDLE;
	VkPhysicalDevice gpu      = VK_NULL_HANDLE;
	VkDevice         device   = VK_NULL_HANDLE;
	VkQueue          queue    = VK_NULL_HANDLE;
	uint32_t         family   = 0;
	float            period   = 1.0f;
	char             name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] {};
	uint32_t         driver_version = 0;
	uint32_t         api_version    = 0;

	FixtureKind fixture         = FixtureKind::DeviceLocal;
	bool        host_import_ext = false;
	VkDeviceSize import_alignment = 0;
	HostBlock    host_block {};
	uint32_t     guest_memory_type = UINT32_MAX;

	VkPhysicalDeviceMemoryProperties memory {};

	Buffer page_table;
	Buffer fault;
	Buffer guest;
	Buffer sink;
	Buffer staging;
	Buffer readback;

	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkPipelineLayout      layout     = VK_NULL_HANDLE;
	VkDescriptorPool      pool       = VK_NULL_HANDLE;
	VkDescriptorSet       set        = VK_NULL_HANDLE;
	VkShaderModule        module     = VK_NULL_HANDLE;
	VkCommandPool         commands   = VK_NULL_HANDLE;
	VkCommandBuffer       cmd        = VK_NULL_HANDLE;
	VkQueryPool           queries    = VK_NULL_HANDLE;
	VkFence               fence      = VK_NULL_HANDLE;

	void CreateInstance();
	void PickDevice(int index);
	void CreateDevice();
	uint32_t PickMemoryType(uint32_t usable, MemoryChoice choice);
	Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryChoice choice,
	                    bool device_address);
	Buffer CreateImportedBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
	void  CreateBuffers();
	void  UploadFixture();
	void  CreatePipelineObjects();
	VkPipeline CreatePipeline(int variant, int pattern);
	double    RunSplit(VkPipeline pipeline, uint32_t iterations, uint32_t groups,
	                   uint32_t dispatches, bool barriers);
	double    RunOnce(VkPipeline pipeline, uint32_t iterations, uint32_t groups);
	PassTimes RunTwoPass(VkPipeline pipeline, uint32_t iterations, uint32_t groups,
	                     VkPipeline evict = VK_NULL_HANDLE);
	uint32_t  Calibrate(VkPipeline pipeline);
	void      Submit();
};

struct PushConstants {
	uint64_t guest_base;
	uint64_t srt_base;
	uint32_t iterations;
	uint32_t span_mask;
};

void CmdBarrier(VkCommandBuffer c) {
	VkMemoryBarrier barrier {};
	barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
	                     nullptr);
}

void Bench::CreateInstance() {
	VkApplicationInfo app {};
	app.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app.pApplicationName = "bda_load_bench";
	app.apiVersion       = VK_API_VERSION_1_3;

	VkInstanceCreateInfo info {};
	info.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &app;
	Check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");

#define KYTY_LOAD(name)                                                                            \
	name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));                   \
	if ((name) == nullptr) {                                                                       \
		Fail("instance entry point " #name " is missing");                                         \
	}
	KYTY_INSTANCE_ENTRIES(KYTY_LOAD)
#undef KYTY_LOAD
}

void Bench::PickDevice(int index) {
	uint32_t count = 0;
	Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
	std::vector<VkPhysicalDevice> devices(count);
	Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
	      "vkEnumeratePhysicalDevices");
	if (count == 0) {
		Fail("no Vulkan device");
	}

	int best = -1;
	for (uint32_t i = 0; i < count; ++i) {
		VkPhysicalDeviceProperties2 props {};
		props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		vkGetPhysicalDeviceProperties2(devices[i], &props);
		std::printf("device %u: %s\n", i, props.properties.deviceName);
		if (best < 0 && props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			best = static_cast<int>(i);
		}
	}
	if (index >= 0) {
		best = index;
	}
	if (best < 0) {
		best = 0;
	}
	gpu = devices[static_cast<uint32_t>(best)];

	VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props {};
	host_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;

	VkPhysicalDeviceProperties2 props {};
	props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	props.pNext = &host_props;
	vkGetPhysicalDeviceProperties2(gpu, &props);
	std::memcpy(name, props.properties.deviceName, sizeof(name));
	period           = props.properties.limits.timestampPeriod;
	driver_version   = props.properties.driverVersion;
	api_version      = props.properties.apiVersion;
	import_alignment = host_props.minImportedHostPointerAlignment;
	if (props.properties.limits.maxStorageBufferRange < kPageTableBytes) {
		Fail("maxStorageBufferRange is smaller than the page table");
	}

	uint32_t extension_count = 0;
	Check(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extension_count, nullptr),
	      "vkEnumerateDeviceExtensionProperties");
	std::vector<VkExtensionProperties> extensions(extension_count);
	Check(vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extension_count, extensions.data()),
	      "vkEnumerateDeviceExtensionProperties");
	for (const auto& extension : extensions) {
		if (std::strcmp(extension.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0) {
			host_import_ext = true;
		}
	}

	uint32_t families = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, nullptr);
	std::vector<VkQueueFamilyProperties> family_props(families);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, family_props.data());
	bool found = false;
	for (uint32_t i = 0; i < families; ++i) {
		if ((family_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
		    family_props[i].timestampValidBits > 0) {
			family = i;
			found  = true;
			break;
		}
	}
	if (!found) {
		Fail("no compute queue family with timestamps");
	}
	vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
}

void Bench::CreateDevice() {
	VkPhysicalDeviceVulkan12Features features12 {};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

	VkPhysicalDeviceFeatures2 features {};
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &features12;
	vkGetPhysicalDeviceFeatures2(gpu, &features);
	if (features12.bufferDeviceAddress == VK_FALSE) {
		Fail("bufferDeviceAddress is unsupported");
	}
	if (features.features.shaderInt64 == VK_FALSE) {
		Fail("shaderInt64 is unsupported");
	}

	VkPhysicalDeviceVulkan12Features enable12 {};
	enable12.sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	enable12.bufferDeviceAddress = VK_TRUE;

	VkPhysicalDeviceFeatures2 enable {};
	enable.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	enable.pNext                = &enable12;
	enable.features.shaderInt64 = VK_TRUE;

	const float             priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info {};
	queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = family;
	queue_info.queueCount       = 1;
	queue_info.pQueuePriorities = &priority;

	std::vector<const char*> device_extensions;
	if (host_import_ext) {
		device_extensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
	}

	VkDeviceCreateInfo info {};
	info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.pNext                   = &enable;
	info.queueCreateInfoCount    = 1;
	info.pQueueCreateInfos       = &queue_info;
	info.enabledExtensionCount   = static_cast<uint32_t>(device_extensions.size());
	info.ppEnabledExtensionNames = device_extensions.empty() ? nullptr : device_extensions.data();
	Check(vkCreateDevice(gpu, &info, nullptr, &device), "vkCreateDevice");

#define KYTY_LOAD(name)                                                                            \
	name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));                       \
	if ((name) == nullptr) {                                                                       \
		Fail("device entry point " #name " is missing");                                           \
	}
	KYTY_DEVICE_ENTRIES(KYTY_LOAD)
#undef KYTY_LOAD

	if (host_import_ext) {
		vkGetMemoryHostPointerPropertiesEXT =
		    reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
		        vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT"));
		if (vkGetMemoryHostPointerPropertiesEXT == nullptr) {
			Fail("vkGetMemoryHostPointerPropertiesEXT is missing");
		}
	}

	vkGetDeviceQueue(device, family, 0, &queue);
}

uint32_t Bench::PickMemoryType(uint32_t usable, MemoryChoice choice) {
	const VkMemoryPropertyFlags wanted =
	    choice == MemoryChoice::HostVisible
	        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
	        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
		if ((usable & (1u << i)) == 0) {
			continue;
		}
		const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
		if ((flags & wanted) != wanted) {
			continue;
		}
		// Both choices must avoid the small host-visible device-local (resizable BAR) heap: it is
		// neither plain VRAM nor plain system memory.
		if ((flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0 &&
		    (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
			continue;
		}
		return i;
	}
	return UINT32_MAX;
}

Buffer Bench::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryChoice choice,
                           bool device_address) {
	Buffer buffer {};
	buffer.size = size;

	VkBufferCreateInfo info {};
	info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size        = size;
	info.usage       = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	Check(vkCreateBuffer(device, &info, nullptr, &buffer.handle), "vkCreateBuffer");

	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);

	uint32_t type = PickMemoryType(requirements.memoryTypeBits, choice);
	if (type == UINT32_MAX) {
		Fail("no matching memory type");
	}

	VkMemoryAllocateFlagsInfo flags {};
	flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

	VkMemoryAllocateInfo allocate {};
	allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext           = device_address ? &flags : nullptr;
	allocate.allocationSize  = requirements.size;
	allocate.memoryTypeIndex = type;
	Check(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory), "vkAllocateMemory");
	Check(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");

	if (device_address) {
		VkBufferDeviceAddressInfo address_info {};
		address_info.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
		address_info.buffer = buffer.handle;
		buffer.address      = vkGetBufferDeviceAddress(device, &address_info);
	}
	if (choice == MemoryChoice::HostVisible) {
		Check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped),
		      "vkMapMemory");
	}
	return buffer;
}

// The design-A shape: guest pages stay where the guest put them, and the GPU reads them in place.
Buffer Bench::CreateImportedBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
	if (!host_import_ext) {
		Fail("VK_EXT_external_memory_host is not supported by this device");
	}
	const auto alignment = static_cast<size_t>(import_alignment == 0 ? 4096 : import_alignment);
	host_block           = AllocateHostBlock(static_cast<size_t>(size), alignment);

	VkMemoryHostPointerPropertiesEXT host_properties {};
	host_properties.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
	Check(vkGetMemoryHostPointerPropertiesEXT(
	          device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host_block.base,
	          &host_properties),
	      "vkGetMemoryHostPointerPropertiesEXT");

	Buffer buffer {};
	buffer.size = size;

	VkExternalMemoryBufferCreateInfo external {};
	external.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;

	VkBufferCreateInfo info {};
	info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.pNext       = &external;
	info.size        = size;
	info.usage       = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	Check(vkCreateBuffer(device, &info, nullptr, &buffer.handle), "vkCreateBuffer");

	VkMemoryRequirements requirements {};
	vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
	const uint32_t usable = requirements.memoryTypeBits & host_properties.memoryTypeBits;
	if (usable == 0) {
		Fail("no memory type can hold the imported host pointer");
	}
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
		if ((usable & (1u << i)) != 0) {
			type = i;
			break;
		}
	}
	guest_memory_type = type;

	VkImportMemoryHostPointerInfoEXT import {};
	import.sType        = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
	import.handleType   = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
	import.pHostPointer = host_block.base;

	VkMemoryAllocateFlagsInfo flags {};
	flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags.pNext = &import;
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

	VkMemoryAllocateInfo allocate {};
	allocate.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext           = &flags;
	allocate.allocationSize  = host_block.size;
	allocate.memoryTypeIndex = type;
	Check(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory), "vkAllocateMemory");
	Check(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");

	VkBufferDeviceAddressInfo address_info {};
	address_info.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	address_info.buffer = buffer.handle;
	buffer.address      = vkGetBufferDeviceAddress(device, &address_info);
	buffer.mapped       = host_block.base;
	return buffer;
}

void Bench::CreateBuffers() {
	const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
	                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT |
	                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	page_table = CreateBuffer(kPageTableBytes, storage, MemoryChoice::DeviceLocal, false);
	fault      = CreateBuffer(kFaultBytes, storage, MemoryChoice::DeviceLocal, false);

	const VkBufferUsageFlags guest_usage = storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	switch (fixture) {
		case FixtureKind::DeviceLocal:
			guest = CreateBuffer(kGuestBytes, guest_usage, MemoryChoice::DeviceLocal, true);
			break;
		case FixtureKind::HostVisible:
			guest = CreateBuffer(kGuestBytes, guest_usage, MemoryChoice::HostVisible, true);
			break;
		case FixtureKind::HostImport: guest = CreateImportedBuffer(kGuestBytes, guest_usage); break;
	}
	if (guest_memory_type == UINT32_MAX) {
		VkMemoryRequirements requirements {};
		vkGetBufferMemoryRequirements(device, guest.handle, &requirements);
		guest_memory_type = PickMemoryType(requirements.memoryTypeBits,
		                                   fixture == FixtureKind::DeviceLocal
		                                       ? MemoryChoice::DeviceLocal
		                                       : MemoryChoice::HostVisible);
	}

	sink     = CreateBuffer(uint64_t {kWorkGroups} * kGroupThreads * 4, storage,
	                        MemoryChoice::DeviceLocal, false);
	staging  = CreateBuffer(uint64_t {1} << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	                        MemoryChoice::HostVisible, false);
	readback = CreateBuffer(uint64_t {64} << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                        MemoryChoice::HostVisible, false);
}

void Bench::Submit() {
	VkSubmitInfo submit {};
	submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers    = &cmd;
	Check(vkResetFences(device, 1, &fence), "vkResetFences");
	Check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
	Check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
}

void Bench::UploadFixture() {
	auto* bytes = static_cast<uint8_t*>(staging.mapped);

	// Page table entries: one device address per mapped 16 KB page.
	auto* entries = reinterpret_cast<uint64_t*>(bytes);
	for (uint64_t page = 0; page < kGuestPages; ++page) {
		entries[page] = guest.address + page * kPageSize;
	}
	const VkDeviceSize entry_bytes = kGuestPages * sizeof(uint64_t);

	// Descriptor chain: user data root -> table -> V#, all holding guest addresses.
	auto* chain = bytes + entry_bytes;
	std::memset(chain, 0, kChainBytes);
	const uint64_t chain_base  = kGuestBase + kChainOffset;
	const uint64_t table_base  = chain_base + 0x1000;
	const uint64_t vsharp_base = chain_base + 0x2000;
	for (uint32_t slot = 0; slot < kChainSlots; ++slot) {
		std::memcpy(chain + slot * 16, &table_base, sizeof(uint64_t));
		const uint64_t vsharp = vsharp_base + slot * 16;
		std::memcpy(chain + 0x1000 + slot * 8, &vsharp, sizeof(uint64_t));
		uint32_t words[4] {};
		words[0] = static_cast<uint32_t>(kGuestBase);
		words[1] = static_cast<uint32_t>(kGuestBase >> 32) & 0xffffu; // base high | stride
		words[1] |= 64u << 16u;
		words[2] = static_cast<uint32_t>(kSpanBytes / 64);
		words[3] = 0x0002'4000u;
		std::memcpy(chain + 0x2000 + slot * 16, words, sizeof(words));
	}

	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");

	vkCmdFillBuffer(cmd, fault.handle, 0, kFaultBytes, 0);
	vkCmdFillBuffer(cmd, sink.handle, 0, sink.size, 0);
	vkCmdFillBuffer(cmd, guest.handle, 0, kSpanBytes, kFillWord);

	VkBufferCopy entry_copy {};
	entry_copy.srcOffset = 0;
	entry_copy.dstOffset = kFirstPage * sizeof(uint64_t);
	entry_copy.size      = entry_bytes;
	vkCmdCopyBuffer(cmd, staging.handle, page_table.handle, 1, &entry_copy);

	VkBufferCopy chain_copy {};
	chain_copy.srcOffset = entry_bytes;
	chain_copy.dstOffset = kChainOffset;
	chain_copy.size      = kChainBytes;
	vkCmdCopyBuffer(cmd, staging.handle, guest.handle, 1, &chain_copy);

	VkMemoryBarrier barrier {};
	barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
	                     nullptr);
	Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
	Submit();
}

void Bench::CreatePipelineObjects() {
	VkDescriptorSetLayoutBinding bindings[4] {};
	for (uint32_t i = 0; i < 4; ++i) {
		bindings[i].binding         = i;
		bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo set_info {};
	set_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = 4;
	set_info.pBindings    = bindings;
	Check(vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout),
	      "vkCreateDescriptorSetLayout");

	VkPushConstantRange range {};
	range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	range.size       = sizeof(PushConstants);

	VkPipelineLayoutCreateInfo layout_info {};
	layout_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount         = 1;
	layout_info.pSetLayouts            = &set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &range;
	Check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout), "vkCreatePipelineLayout");

	VkDescriptorPoolSize size {};
	size.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = 4;
	VkDescriptorPoolCreateInfo pool_info {};
	pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets       = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes    = &size;
	Check(vkCreateDescriptorPool(device, &pool_info, nullptr, &pool), "vkCreateDescriptorPool");

	VkDescriptorSetAllocateInfo allocate {};
	allocate.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool     = pool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts        = &set_layout;
	Check(vkAllocateDescriptorSets(device, &allocate, &set), "vkAllocateDescriptorSets");

	const Buffer*          buffers[4] = {&page_table, &fault, &guest, &sink};
	VkDescriptorBufferInfo infos[4] {};
	VkWriteDescriptorSet   writes[4] {};
	for (uint32_t i = 0; i < 4; ++i) {
		infos[i].buffer           = buffers[i]->handle;
		infos[i].offset           = 0;
		infos[i].range            = VK_WHOLE_SIZE;
		writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet          = set;
		writes[i].dstBinding      = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo     = &infos[i];
	}
	vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);

	VkShaderModuleCreateInfo module_info {};
	module_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = sizeof(BDA_LOAD_BENCH_SPV);
	module_info.pCode    = BDA_LOAD_BENCH_SPV;
	Check(vkCreateShaderModule(device, &module_info, nullptr, &module), "vkCreateShaderModule");

	VkCommandPoolCreateInfo commands_info {};
	commands_info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commands_info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	commands_info.queueFamilyIndex = family;
	Check(vkCreateCommandPool(device, &commands_info, nullptr, &commands), "vkCreateCommandPool");

	VkCommandBufferAllocateInfo cmd_info {};
	cmd_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cmd_info.commandPool        = commands;
	cmd_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_info.commandBufferCount = 1;
	Check(vkAllocateCommandBuffers(device, &cmd_info, &cmd), "vkAllocateCommandBuffers");

	VkQueryPoolCreateInfo query_info {};
	query_info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	query_info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
	query_info.queryCount = kQueryCount;
	Check(vkCreateQueryPool(device, &query_info, nullptr, &queries), "vkCreateQueryPool");

	VkFenceCreateInfo fence_info {};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	Check(vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence");
}

VkPipeline Bench::CreatePipeline(int variant, int pattern) {
	const int32_t            values[2] = {variant, pattern};
	VkSpecializationMapEntry entries[2] {};
	entries[0].constantID = 0;
	entries[0].offset     = 0;
	entries[0].size       = sizeof(int32_t);
	entries[1].constantID = 1;
	entries[1].offset     = sizeof(int32_t);
	entries[1].size       = sizeof(int32_t);

	VkSpecializationInfo specialization {};
	specialization.mapEntryCount = 2;
	specialization.pMapEntries   = entries;
	specialization.dataSize      = sizeof(values);
	specialization.pData         = values;

	VkComputePipelineCreateInfo info {};
	info.sType                     = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage               = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module              = module;
	info.stage.pName               = "main";
	info.stage.pSpecializationInfo = &specialization;
	info.layout                    = layout;

	VkPipeline pipeline = VK_NULL_HANDLE;
	Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline),
	      "vkCreateComputePipelines");
	return pipeline;
}

double Bench::RunSplit(VkPipeline pipeline, uint32_t iterations, uint32_t groups,
                       uint32_t dispatches, bool barriers) {
	PushConstants push {};
	push.guest_base = kGuestBase;
	push.srt_base   = kGuestBase + kChainOffset;
	push.iterations = iterations;
	push.span_mask  = kSpanMask;

	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	Check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
	Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
	vkCmdResetQueryPool(cmd, queries, 0, kQueryCount);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
	for (uint32_t d = 0; d < dispatches; ++d) {
		if (barriers && d > 0) {
			CmdBarrier(cmd);
		}
		vkCmdDispatch(cmd, groups, 1, 1);
	}
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
	Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
	Submit();

	uint64_t stamps[2] {};
	Check(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t),
	                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
	      "vkGetQueryPoolResults");
	return static_cast<double>(stamps[1] - stamps[0]) * static_cast<double>(period);
}

double Bench::RunOnce(VkPipeline pipeline, uint32_t iterations, uint32_t groups) {
	return RunSplit(pipeline, iterations, groups, 1, false);
}

// Two identical passes over the same addresses in the same command buffer, separated only by an
// execution barrier, so pass 2 sees whatever pass 1 left in the caches.
PassTimes Bench::RunTwoPass(VkPipeline pipeline, uint32_t iterations, uint32_t groups,
                            VkPipeline evict) {
	PushConstants push {};
	push.guest_base = kGuestBase;
	push.srt_base   = kGuestBase + kChainOffset;
	push.iterations = iterations;
	push.span_mask  = kSpanMask;

	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	Check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
	Check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
	vkCmdResetQueryPool(cmd, queries, 0, kQueryCount);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
	if (evict != VK_NULL_HANDLE) {
		PushConstants evict_push = push;
		evict_push.iterations    = 1;
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, evict);
		vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(evict_push),
		                   &evict_push);
		vkCmdDispatch(cmd, kEvictGroups, 1, 1);
		CmdBarrier(cmd);
	}
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
	vkCmdDispatch(cmd, groups, 1, 1);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
	CmdBarrier(cmd);
	vkCmdDispatch(cmd, groups, 1, 1);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 2);
	Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
	Submit();

	uint64_t stamps[3] {};
	Check(vkGetQueryPoolResults(device, queries, 0, 3, sizeof(stamps), stamps, sizeof(uint64_t),
	                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
	      "vkGetQueryPoolResults");
	PassTimes times {};
	times.pass1 = static_cast<double>(stamps[1] - stamps[0]) * static_cast<double>(period);
	times.pass2 = static_cast<double>(stamps[2] - stamps[1]) * static_cast<double>(period);
	return times;
}

// Pick an iteration count so that one pass lands near kTargetMs. The first probe runs a fraction of
// the groups so that a fixture 50x slower than VRAM still cannot produce a multi-second dispatch.
uint32_t Bench::Calibrate(VkPipeline pipeline) {
	const double target_ns = kTargetMs * 1.0e6;
	const double probe_ns  = RunOnce(pipeline, 1, kProbeGroups);
	const double per_iter  = probe_ns * (static_cast<double>(kWorkGroups) / kProbeGroups);
	auto         iterations =
	    static_cast<uint32_t>(std::max(1.0, std::min(target_ns / std::max(per_iter, 1.0), 4.0e6)));

	double measured = RunOnce(pipeline, iterations, kWorkGroups);
	for (int refine = 0; refine < 2; ++refine) {
		if (measured > target_ns * 0.6 && measured < target_ns * 1.8) {
			break;
		}
		const double scale = target_ns / std::max(measured, 1.0);
		const auto   next  = static_cast<uint32_t>(
            std::max(1.0, std::min(static_cast<double>(iterations) * scale, 4.0e6)));
		if (next == iterations) {
			break;
		}
		iterations = next;
		measured   = RunOnce(pipeline, iterations, kWorkGroups);
	}
	return iterations;
}

struct Cell {
	uint32_t iterations = 0;
	double   best[2] {};        // ns for pass 1 and pass 2
	double   samples[2][kSamples] {};
	double   ns_per_load[2] {};
	double   ns_per_iter[2] {};
};

struct ChainCell {
	uint32_t n = 0;
	double   best[2] {};
	double   samples[2][kChainSamples] {};
	double   ns_per_load[2] {};
};

void MergeMin(double& target, double value) {
	if (target == 0.0 || value < target) {
		target = value;
	}
}

} // namespace

int main(int argc, char** argv) {
	int         index = -1;
	int         runs  = 3;
	FixtureKind fixture = FixtureKind::DeviceLocal;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			index = std::atoi(argv[i + 1]);
		} else if (std::strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
			runs = std::max(1, std::atoi(argv[i + 1]));
		} else if (std::strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
			const char* value = argv[i + 1];
			if (std::strcmp(value, "device-local") == 0) {
				fixture = FixtureKind::DeviceLocal;
			} else if (std::strcmp(value, "host-import") == 0) {
				fixture = FixtureKind::HostImport;
			} else if (std::strcmp(value, "host-visible") == 0) {
				fixture = FixtureKind::HostVisible;
			} else {
				Fail("--fixture must be device-local, host-import or host-visible");
			}
		}
	}

	LoadLoader();
	Bench bench;
	bench.fixture = fixture;
	bench.CreateInstance();
	bench.PickDevice(index);
	bench.CreateDevice();
	bench.CreateBuffers();
	bench.CreatePipelineObjects();
	bench.UploadFixture();

	std::printf("\nselected: %s\n", bench.name);
	std::printf("driver version: %u.%u.%u, api %u.%u.%u\n", bench.driver_version >> 22,
	            (bench.driver_version >> 14) & 0xff, bench.driver_version & 0x3fff,
	            VK_API_VERSION_MAJOR(bench.api_version), VK_API_VERSION_MINOR(bench.api_version),
	            VK_API_VERSION_PATCH(bench.api_version));
	std::printf("timestampPeriod: %.4f ns\n", static_cast<double>(bench.period));
	std::printf("fixture: %s\n", FixtureName(bench.fixture));
	std::printf("  VK_EXT_external_memory_host: %s\n", bench.host_import_ext ? "present" : "absent");
	std::printf("  minImportedHostPointerAlignment: %llu bytes\n",
	            static_cast<unsigned long long>(bench.import_alignment));
	if (bench.fixture == FixtureKind::HostImport) {
		std::printf("  host block: %p, %llu bytes committed (requested %llu)\n",
		            bench.host_block.base,
		            static_cast<unsigned long long>(bench.host_block.size),
		            static_cast<unsigned long long>(kGuestBytes));
	}
	std::printf("  guest memory type %u, heap %u, flags: ", bench.guest_memory_type,
	            bench.memory.memoryTypes[bench.guest_memory_type].heapIndex);
	PrintMemoryFlags(bench.memory.memoryTypes[bench.guest_memory_type].propertyFlags);
	std::printf("\n  heap size: %llu MB\n",
	            static_cast<unsigned long long>(
	                bench.memory.memoryHeaps[bench.memory.memoryTypes[bench.guest_memory_type]
	                                             .heapIndex]
	                    .size >>
	                20));
	std::printf("  guest buffer device address: 0x%llx\n",
	            static_cast<unsigned long long>(bench.guest.address));
	std::printf("dispatch: %u groups x %u threads = %llu invocations, %u loads per iteration\n",
	            kWorkGroups, kGroupThreads,
	            static_cast<unsigned long long>(uint64_t {kWorkGroups} * kGroupThreads),
	            kLoadsPerIter);
	std::printf("guest memory: %llu MB mapped in %llu pages of %llu KB; page table %llu MB\n",
	            static_cast<unsigned long long>(kGuestBytes >> 20),
	            static_cast<unsigned long long>(kGuestPages),
	            static_cast<unsigned long long>(kPageSize >> 10),
	            static_cast<unsigned long long>(kPageTableBytes >> 20));

	double agg_iter[kTputPatterns][3][2] {};   // min ns per invocation-iteration
	double agg_chain[2][3][2] {};              // [N index][variant][pass] min ns per dependent load
	double agg_parallel[4][2] {};              // [groups index][pass]
	double agg_split[3] {};                    // one big / many small / many small with barriers

	// Scattered BDA reads over the whole region, used untimed to evict the caches before a chain.
	VkPipeline evict = bench.CreatePipeline(1, 2);

	for (int run = 0; run < runs; ++run) {
		std::printf("\n=== run %d (%s) ===\n", run + 1, FixtureName(bench.fixture));

		Cell cells[kTputPatterns][3] {};
		for (int pattern = 0; pattern < kTputPatterns; ++pattern) {
			for (int variant = 0; variant < 3; ++variant) {
				VkPipeline pipeline = bench.CreatePipeline(variant, pattern);

				const uint32_t iterations = bench.Calibrate(pipeline);
				bench.RunTwoPass(pipeline, iterations, kWorkGroups); // warm up at the final size

				Cell& cell      = cells[pattern][variant];
				cell.iterations = iterations;
				for (uint32_t s = 0; s < kSamples; ++s) {
					const PassTimes times = bench.RunTwoPass(pipeline, iterations, kWorkGroups);
					cell.samples[0][s]    = times.pass1;
					cell.samples[1][s]    = times.pass2;
					MergeMin(cell.best[0], times.pass1);
					MergeMin(cell.best[1], times.pass2);
				}
				const auto invocations = static_cast<double>(uint64_t {kWorkGroups} * kGroupThreads);
				const double iters     = static_cast<double>(iterations);
				for (int pass = 0; pass < 2; ++pass) {
					cell.ns_per_iter[pass] = cell.best[pass] / (invocations * iters);
					cell.ns_per_load[pass] =
					    cell.ns_per_iter[pass] / static_cast<double>(kLoadsPerIter);
					MergeMin(agg_iter[pattern][variant][pass], cell.ns_per_iter[pass]);
				}

				std::printf("\n%-9s %-9s iterations=%u\n", kPatternName[pattern],
				            kVariantName[variant], iterations);
				for (int pass = 0; pass < 2; ++pass) {
					std::printf("  pass %d (ms):", pass + 1);
					for (uint32_t s = 0; s < kSamples; ++s) {
						std::printf(" %.3f", cell.samples[pass][s] / 1.0e6);
					}
					std::printf("\n    min %.3f ms, %.2f ps/load\n", cell.best[pass] / 1.0e6,
					            cell.ns_per_load[pass] * 1000.0);
				}
				std::fflush(stdout);

				vkDestroyPipeline(bench.device, pipeline, nullptr);
			}
		}

		// chain-latency: one workgroup of 64 threads, a dependent chain of N loads.
		ChainCell chain_cells[2][3] {};
		for (int n_index = 0; n_index < 2; ++n_index) {
			for (int variant = 0; variant < 3; ++variant) {
				VkPipeline pipeline = bench.CreatePipeline(variant, 3);
				const uint32_t n    = kChainN[n_index];
				bench.RunTwoPass(pipeline, n, 1, evict); // warm up

				ChainCell& cell = chain_cells[n_index][variant];
				cell.n          = n;
				for (uint32_t s = 0; s < kChainSamples; ++s) {
					const PassTimes times = bench.RunTwoPass(pipeline, n, 1, evict);
					cell.samples[0][s]    = times.pass1;
					cell.samples[1][s]    = times.pass2;
					MergeMin(cell.best[0], times.pass1);
					MergeMin(cell.best[1], times.pass2);
				}
				for (int pass = 0; pass < 2; ++pass) {
					cell.ns_per_load[pass] = cell.best[pass] / static_cast<double>(n);
					MergeMin(agg_chain[n_index][variant][pass], cell.ns_per_load[pass]);
				}

				std::printf("\nchain     %-9s N=%u (1 workgroup, caches evicted first)\n",
				            kVariantName[variant], n);
				for (int pass = 0; pass < 2; ++pass) {
					std::printf("  pass %d %s (us):", pass + 1, pass == 0 ? "cold" : "warm");
					for (uint32_t s = 0; s < kChainSamples; ++s) {
						std::printf(" %.2f", cell.samples[pass][s] / 1.0e3);
					}
					std::printf("\n    min %.2f us, %.1f ns per dependent load\n",
					            cell.best[pass] / 1.0e3, cell.ns_per_load[pass]);
				}
				std::fflush(stdout);
				vkDestroyPipeline(bench.device, pipeline, nullptr);
			}
		}

		// How well do independent dependent chains overlap? Same chain, more workgroups.
		{
			VkPipeline pipeline = bench.CreatePipeline(1, 3);
			const uint32_t n    = kChainN[1];
			std::printf("\nchain-parallel B bda N=%u\n", n);
			for (int g = 0; g < 4; ++g) {
				const uint32_t groups = kChainGroups[g];
				bench.RunTwoPass(pipeline, n, groups, evict);
				double best1 = 0.0;
				double best2 = 0.0;
				for (uint32_t s = 0; s < kChainSamples; ++s) {
					const PassTimes times = bench.RunTwoPass(pipeline, n, groups, evict);
					MergeMin(best1, times.pass1);
					MergeMin(best2, times.pass2);
				}
				MergeMin(agg_parallel[g][0], best1);
				MergeMin(agg_parallel[g][1], best2);
				std::printf("  %4u groups: pass1 %.2f us (%.1f ns/dep load), pass2 %.2f us "
				            "(%.1f ns/dep load)\n",
				            groups, best1 / 1.0e3, best1 / static_cast<double>(n), best2 / 1.0e3,
				            best2 / static_cast<double>(n));
			}
			std::fflush(stdout);
			vkDestroyPipeline(bench.device, pipeline, nullptr);
		}

		// Does the GPU overlap back to back dispatches the way it overlaps draws?
		{
			VkPipeline     pipeline   = bench.CreatePipeline(2, 0);
			const uint32_t iterations = std::max(1u, cells[0][2].iterations / kSplitDivisor);
			const double   one =
			    std::min({bench.RunSplit(pipeline, iterations, kWorkGroups, 1, false),
			              bench.RunSplit(pipeline, iterations, kWorkGroups, 1, false),
			              bench.RunSplit(pipeline, iterations, kWorkGroups, 1, false)});
			const double many =
			    std::min({bench.RunSplit(pipeline, iterations, kSplitGroups, kSplitDispatch, false),
			              bench.RunSplit(pipeline, iterations, kSplitGroups, kSplitDispatch, false),
			              bench.RunSplit(pipeline, iterations, kSplitGroups, kSplitDispatch,
			                             false)});
			const double serial =
			    std::min({bench.RunSplit(pipeline, iterations, kSplitGroups, kSplitDispatch, true),
			              bench.RunSplit(pipeline, iterations, kSplitGroups, kSplitDispatch, true),
			              bench.RunSplit(pipeline, iterations, kSplitGroups, kSplitDispatch,
			                             true)});
			MergeMin(agg_split[0], one);
			MergeMin(agg_split[1], many);
			MergeMin(agg_split[2], serial);
			std::printf("\ndispatch split, uniform C bda+srt, iterations=%u\n", iterations);
			std::printf("  1 x %u groups            : %.3f ms\n", kWorkGroups, one / 1.0e6);
			std::printf("  %u x %u groups, no barrier: %.3f ms\n", kSplitDispatch, kSplitGroups,
			            many / 1.0e6);
			std::printf("  %u x %u groups, barriers  : %.3f ms\n", kSplitDispatch, kSplitGroups,
			            serial / 1.0e6);
			std::fflush(stdout);
			vkDestroyPipeline(bench.device, pipeline, nullptr);
		}
	}

	vkDestroyPipeline(bench.device, evict, nullptr);

	// Sanity: every access must have found a mapped page, and the sink must hold real data. The
	// fixture is filled identically in every kind, so the expected accumulator is the same number.
	constexpr uint32_t kVerifyIterations = 8;
	{
		VkPipeline pipeline = bench.CreatePipeline(2, 2); // scattered C bda+srt
		bench.RunOnce(pipeline, kVerifyIterations, kWorkGroups);
		vkDestroyPipeline(bench.device, pipeline, nullptr);

		VkCommandBufferBeginInfo begin {};
		begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		Check(vkResetCommandBuffer(bench.cmd, 0), "vkResetCommandBuffer");
		Check(vkBeginCommandBuffer(bench.cmd, &begin), "vkBeginCommandBuffer");
		VkBufferCopy copy {};
		copy.srcOffset = (kFirstPage / 8) & ~uint64_t {3};
		copy.dstOffset = 0;
		copy.size      = 32u << 10u;
		vkCmdCopyBuffer(bench.cmd, bench.fault.handle, bench.readback.handle, 1, &copy);
		VkBufferCopy sink_copy {};
		sink_copy.srcOffset = 0;
		sink_copy.dstOffset = 32u << 10u;
		sink_copy.size      = 1024;
		vkCmdCopyBuffer(bench.cmd, bench.sink.handle, bench.readback.handle, 1, &sink_copy);
		Check(vkEndCommandBuffer(bench.cmd), "vkEndCommandBuffer");
		bench.Submit();
	}
	const auto* words      = static_cast<const uint32_t*>(bench.readback.mapped);
	uint32_t    fault_bits = 0;
	for (uint32_t i = 0; i < (32u << 10u) / 4u; ++i) {
		if (words[i] != 0) {
			++fault_bits;
		}
	}
	const uint32_t vsharp_tail = static_cast<uint32_t>(kSpanBytes / 64) + 0x0002'4000u;
	const uint32_t expected    = kVerifyIterations * (kLoadsPerIter * kFillWord + vsharp_tail);
	uint32_t       sink_mismatch = 0;
	for (uint32_t i = 0; i < 256; ++i) {
		if (words[(32u << 10u) / 4u + i] != expected) {
			++sink_mismatch;
		}
	}

	std::printf("\nfault words set near the mapped range: %u (expected 0)\n", fault_bits);
	std::printf("sink values differing from the expected 0x%08x: %u of 256 (expected 0), "
	            "sink[0] = 0x%08x\n",
	            expected, sink_mismatch, words[(32u << 10u) / 4u]);

	std::printf("\n=== combined: %s, minimum over %d runs x %u samples ===\n",
	            FixtureName(bench.fixture), runs, kSamples);
	std::printf("\n| fixture | pattern | variant | ps/load pass 1 | ps/load pass 2 |"
	            " ratio vs A p1 | pass2 / pass1 | 200M reads p1 (ms) |\n");
	std::printf("| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |\n");
	for (int pattern = 0; pattern < kTputPatterns; ++pattern) {
		const double base = agg_iter[pattern][0][0] / kLoadsPerIter;
		for (int variant = 0; variant < 3; ++variant) {
			const double p1 = agg_iter[pattern][variant][0] / kLoadsPerIter;
			const double p2 = agg_iter[pattern][variant][1] / kLoadsPerIter;
			std::printf("| %s | %s | %s | %.2f | %.2f | %.2fx | %.2fx | %.2f |\n",
			            FixtureName(bench.fixture), kPatternName[pattern], kVariantName[variant],
			            p1 * 1000.0, p2 * 1000.0, p1 / base, p2 / p1, 200.0e6 * p1 / 1.0e6);
		}
	}

	std::printf("\n| fixture | pattern | descriptor prologue p1 (ps per invocation) |"
	            " p2 (ps per invocation) |\n");
	std::printf("| --- | --- | ---: | ---: |\n");
	for (int pattern = 0; pattern < kTputPatterns; ++pattern) {
		const double p1 = agg_iter[pattern][2][0] - agg_iter[pattern][1][0];
		const double p2 = agg_iter[pattern][2][1] - agg_iter[pattern][1][1];
		std::printf("| %s | %s | %.2f | %.2f |\n", FixtureName(bench.fixture),
		            kPatternName[pattern], p1 * 1000.0, p2 * 1000.0);
	}

	std::printf("\n| fixture | variant | N | ns per dependent load, cold | warm |"
	            " slope cold (ns) | slope warm (ns) |\n");
	std::printf("| --- | --- | ---: | ---: | ---: | ---: | ---: |\n");
	for (int variant = 0; variant < 3; ++variant) {
		for (int n_index = 0; n_index < 2; ++n_index) {
			const double p1 = agg_chain[n_index][variant][0];
			const double p2 = agg_chain[n_index][variant][1];
			// slope: (T(1024) - T(64)) / (1024 - 64), which removes the fixed dispatch overhead.
			const double slope1 = (agg_chain[1][variant][0] * kChainN[1] -
			                       agg_chain[0][variant][0] * kChainN[0]) /
			                      static_cast<double>(kChainN[1] - kChainN[0]);
			const double slope2 = (agg_chain[1][variant][1] * kChainN[1] -
			                       agg_chain[0][variant][1] * kChainN[0]) /
			                      static_cast<double>(kChainN[1] - kChainN[0]);
			std::printf("| %s | %s | %u | %.1f | %.1f | %.1f | %.1f |\n",
			            FixtureName(bench.fixture), kVariantName[variant], kChainN[n_index], p1, p2,
			            slope1, slope2);
		}
	}

	std::printf("\n| fixture | chains in flight (groups) | total cold (us) | total warm (us) |"
	            " ns per dependent load, cold |\n");
	std::printf("| --- | ---: | ---: | ---: | ---: |\n");
	for (int g = 0; g < 4; ++g) {
		std::printf("| %s | %u | %.2f | %.2f | %.1f |\n", FixtureName(bench.fixture),
		            kChainGroups[g], agg_parallel[g][0] / 1.0e3, agg_parallel[g][1] / 1.0e3,
		            agg_parallel[g][0] / static_cast<double>(kChainN[1]));
	}

	std::printf("\n| fixture | dispatch shape | ms |\n");
	std::printf("| --- | --- | ---: |\n");
	std::printf("| %s | 1 x %u groups | %.3f |\n", FixtureName(bench.fixture), kWorkGroups,
	            agg_split[0] / 1.0e6);
	std::printf("| %s | %u x %u groups, no barrier | %.3f |\n", FixtureName(bench.fixture),
	            kSplitDispatch, kSplitGroups, agg_split[1] / 1.0e6);
	std::printf("| %s | %u x %u groups, barriers | %.3f |\n", FixtureName(bench.fixture),
	            kSplitDispatch, kSplitGroups, agg_split[2] / 1.0e6);

	vkDeviceWaitIdle(bench.device);
	return 0;
}
