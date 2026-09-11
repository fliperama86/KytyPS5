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
constexpr uint32_t kGroupThreads = 64;
constexpr uint32_t kLoadsPerIter = 16;
constexpr uint32_t kSamples      = 5;
constexpr double   kTargetMs     = 30.0;

constexpr const char* kVariantName[3] = {"A ssbo", "B bda", "C bda+srt"};
constexpr const char* kPatternName[3] = {"uniform", "strided", "scattered"};

[[noreturn]] void Fail(const char* what, VkResult result = VK_SUCCESS) {
	std::fprintf(stderr, "bda_load_bench: %s (VkResult %d)\n", what, static_cast<int>(result));
	std::exit(1);
}

void Check(VkResult result, const char* what) {
	if (result != VK_SUCCESS) {
		Fail(what, result);
	}
}

#define KYTY_INSTANCE_ENTRIES(X)                                                                   \
	X(vkEnumeratePhysicalDevices)                                                                  \
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

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
PFN_vkCreateInstance      vkCreateInstance      = nullptr;

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

struct Buffer {
	VkBuffer        handle  = VK_NULL_HANDLE;
	VkDeviceMemory  memory  = VK_NULL_HANDLE;
	VkDeviceSize    size    = 0;
	VkDeviceAddress address = 0;
	void*           mapped  = nullptr;
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
	Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible,
	                    bool device_address);
	void  CreateBuffers();
	void  UploadFixture();
	void  CreatePipelineObjects();
	VkPipeline CreatePipeline(int variant, int pattern);
	double RunOnce(VkPipeline pipeline, uint32_t iterations);
	void   Submit();
};

struct PushConstants {
	uint64_t guest_base;
	uint64_t srt_base;
	uint32_t iterations;
	uint32_t span_mask;
};

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

	VkPhysicalDeviceProperties2 props {};
	props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	vkGetPhysicalDeviceProperties2(gpu, &props);
	std::memcpy(name, props.properties.deviceName, sizeof(name));
	period         = props.properties.limits.timestampPeriod;
	driver_version = props.properties.driverVersion;
	api_version    = props.properties.apiVersion;
	if (props.properties.limits.maxStorageBufferRange < kPageTableBytes) {
		Fail("maxStorageBufferRange is smaller than the page table");
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

	const float       priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info {};
	queue_info.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = family;
	queue_info.queueCount       = 1;
	queue_info.pQueuePriorities = &priority;

	VkDeviceCreateInfo info {};
	info.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	info.pNext                = &enable;
	info.queueCreateInfoCount = 1;
	info.pQueueCreateInfos    = &queue_info;
	Check(vkCreateDevice(gpu, &info, nullptr, &device), "vkCreateDevice");

#define KYTY_LOAD(name)                                                                            \
	name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));                       \
	if ((name) == nullptr) {                                                                       \
		Fail("device entry point " #name " is missing");                                           \
	}
	KYTY_DEVICE_ENTRIES(KYTY_LOAD)
#undef KYTY_LOAD

	vkGetDeviceQueue(device, family, 0, &queue);
}

Buffer Bench::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible,
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

	const VkMemoryPropertyFlags wanted =
	    host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
	                 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
		const bool usable = (requirements.memoryTypeBits & (1u << i)) != 0;
		const bool match  = (memory.memoryTypes[i].propertyFlags & wanted) == wanted;
		if (usable && match) {
			// Prefer plain device-local memory over the small host-visible device-local heap.
			if (!host_visible &&
			    (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
				continue;
			}
			type = i;
			break;
		}
	}
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
	if (host_visible) {
		Check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped),
		      "vkMapMemory");
	}
	return buffer;
}

void Bench::CreateBuffers() {
	const VkBufferUsageFlags storage =
	    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
	    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	page_table = CreateBuffer(kPageTableBytes, storage, false, false);
	fault      = CreateBuffer(kFaultBytes, storage, false, false);
	guest      = CreateBuffer(kGuestBytes, storage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
	                          false, true);
	sink       = CreateBuffer(uint64_t {kWorkGroups} * kGroupThreads * 4, storage, false, false);
	staging    = CreateBuffer(uint64_t {1} << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, false);
	readback   = CreateBuffer(uint64_t {64} << 10, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false);
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

	const Buffer* buffers[4] = {&page_table, &fault, &guest, &sink};
	VkDescriptorBufferInfo infos[4] {};
	VkWriteDescriptorSet   writes[4] {};
	for (uint32_t i = 0; i < 4; ++i) {
		infos[i].buffer        = buffers[i]->handle;
		infos[i].offset        = 0;
		infos[i].range         = VK_WHOLE_SIZE;
		writes[i].sType        = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet       = set;
		writes[i].dstBinding   = i;
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
	query_info.queryCount = 2;
	Check(vkCreateQueryPool(device, &query_info, nullptr, &queries), "vkCreateQueryPool");

	VkFenceCreateInfo fence_info {};
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	Check(vkCreateFence(device, &fence_info, nullptr, &fence), "vkCreateFence");
}

VkPipeline Bench::CreatePipeline(int variant, int pattern) {
	const int32_t values[2] = {variant, pattern};
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

double Bench::RunOnce(VkPipeline pipeline, uint32_t iterations) {
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
	vkCmdResetQueryPool(cmd, queries, 0, 2);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
	vkCmdDispatch(cmd, kWorkGroups, 1, 1);
	vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
	Check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
	Submit();

	uint64_t stamps[2] {};
	Check(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t),
	                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
	      "vkGetQueryPoolResults");
	return static_cast<double>(stamps[1] - stamps[0]) * static_cast<double>(period);
}

struct Cell {
	uint32_t iterations   = 0;
	double   best_ns      = 0.0;
	double   samples[kSamples] {};
	double   ns_per_load  = 0.0;
	double   ns_per_iter  = 0.0;
};

} // namespace

int main(int argc, char** argv) {
	int index = -1;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			index = std::atoi(argv[i + 1]);
		}
	}

	LoadLoader();
	Bench bench;
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
	std::printf("dispatch: %u groups x %u threads = %llu invocations, %u loads per iteration\n",
	            kWorkGroups, kGroupThreads,
	            static_cast<unsigned long long>(uint64_t {kWorkGroups} * kGroupThreads),
	            kLoadsPerIter);
	std::printf("guest memory: %llu MB mapped in %llu pages of %llu KB; page table %llu MB\n",
	            static_cast<unsigned long long>(kGuestBytes >> 20),
	            static_cast<unsigned long long>(kGuestPages),
	            static_cast<unsigned long long>(kPageSize >> 10),
	            static_cast<unsigned long long>(kPageTableBytes >> 20));

	Cell cells[3][3] {};
	for (int pattern = 0; pattern < 3; ++pattern) {
		for (int variant = 0; variant < 3; ++variant) {
			VkPipeline pipeline = bench.CreatePipeline(variant, pattern);

			// Calibrate so that one dispatch lands near kTargetMs of GPU time.
			uint32_t probe    = 16;
			double   probe_ns = bench.RunOnce(pipeline, probe);
			double   scale    = (kTargetMs * 1.0e6) / probe_ns;
			auto     wanted   = static_cast<double>(probe) * scale;
			uint32_t iterations =
			    static_cast<uint32_t>(std::max(1.0, std::min(wanted, 4.0e6)));

			bench.RunOnce(pipeline, iterations); // warm up at the final size

			Cell& cell     = cells[pattern][variant];
			cell.iterations = iterations;
			cell.best_ns    = 0.0;
			for (uint32_t s = 0; s < kSamples; ++s) {
				const double ns = bench.RunOnce(pipeline, iterations);
				cell.samples[s] = ns;
				if (cell.best_ns == 0.0 || ns < cell.best_ns) {
					cell.best_ns = ns;
				}
			}
			const auto invocations = static_cast<double>(uint64_t {kWorkGroups} * kGroupThreads);
			const double iters     = static_cast<double>(iterations);
			cell.ns_per_iter       = cell.best_ns / (invocations * iters);
			cell.ns_per_load       = cell.ns_per_iter / static_cast<double>(kLoadsPerIter);

			std::printf("\n%-9s %-9s iterations=%u\n", kPatternName[pattern],
			            kVariantName[variant], iterations);
			std::printf("  samples (ms):");
			for (uint32_t s = 0; s < kSamples; ++s) {
				std::printf(" %.3f", cell.samples[s] / 1.0e6);
			}
			std::printf("\n  min %.3f ms, %.2f ps/load\n", cell.best_ns / 1.0e6,
			            cell.ns_per_load * 1000.0);
			std::fflush(stdout);

			vkDestroyPipeline(bench.device, pipeline, nullptr);
		}
	}

	// Sanity: every access must have found a mapped page, and the sink must hold real data.
	{
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
	const auto* words       = static_cast<const uint32_t*>(bench.readback.mapped);
	uint32_t    fault_bits  = 0;
	for (uint32_t i = 0; i < (32u << 10u) / 4u; ++i) {
		if (words[i] != 0) {
			++fault_bits;
		}
	}
	// The last dispatch was the scattered C variant. Every lane summed the same constants, so the
	// expected accumulator is known exactly.
	const uint32_t last_iterations = cells[2][2].iterations;
	const uint32_t vsharp_tail     = static_cast<uint32_t>(kSpanBytes / 64) + 0x0002'4000u;
	const uint32_t expected        = last_iterations * (kLoadsPerIter * kFillWord + vsharp_tail);
	uint32_t       sink_mismatch   = 0;
	for (uint32_t i = 0; i < 256; ++i) {
		if (words[(32u << 10u) / 4u + i] != expected) {
			++sink_mismatch;
		}
	}

	std::printf("\nfault words set near the mapped range: %u (expected 0)\n", fault_bits);
	std::printf("sink values differing from the expected 0x%08x: %u of 256 (expected 0)\n",
	            expected, sink_mismatch);

	std::printf("\n| pattern | variant | iterations | min dispatch (ms) | ps / load | ratio vs A |"
	            " 200M reads (ms) |\n");
	std::printf("| --- | --- | ---: | ---: | ---: | ---: | ---: |\n");
	for (int pattern = 0; pattern < 3; ++pattern) {
		const double base = cells[pattern][0].ns_per_load;
		for (int variant = 0; variant < 3; ++variant) {
			const Cell& cell = cells[pattern][variant];
			std::printf("| %s | %s | %u | %.2f | %.2f | %.2fx | %.2f |\n", kPatternName[pattern],
			            kVariantName[variant], cell.iterations, cell.best_ns / 1.0e6,
			            cell.ns_per_load * 1000.0, cell.ns_per_load / base,
			            200.0e6 * cell.ns_per_load / 1.0e6);
		}
	}

	std::printf("\n| pattern | descriptor prologue (ps per invocation) |"
	            " 1M invocations (ms) |\n");
	std::printf("| --- | ---: | ---: |\n");
	for (int pattern = 0; pattern < 3; ++pattern) {
		const double prologue = cells[pattern][2].ns_per_iter - cells[pattern][1].ns_per_iter;
		// One million invocations of `prologue` nanoseconds each is exactly `prologue` ms.
		std::printf("| %s | %.2f | %.4f |\n", kPatternName[pattern], prologue * 1000.0, prologue);
	}

	vkDeviceWaitIdle(bench.device);
	return 0;
}
