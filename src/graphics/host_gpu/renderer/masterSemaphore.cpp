#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/gpuFaultTrace.h"

#include <algorithm>
#include <cstdio>

namespace Libs::Graphics {

namespace {
void ReportDeviceFault(GraphicContext& graphics, vk::Result result) {
	if (result != vk::Result::eErrorDeviceLost) {
		return;
	}
	GpuFaultTrace::Dump();
	if (graphics.diagnostic_checkpoints_enabled) {
		uint32_t count = 0;
		graphics.queue.getCheckpointDataNV(&count, nullptr);
		std::vector<vk::CheckpointDataNV> checkpoints(count);
		if (count != 0) {
			graphics.queue.getCheckpointDataNV(&count, checkpoints.data());
		}
		for (const auto& checkpoint: checkpoints) {
			std::printf("GPU checkpoint: stage=%s shader=0x%016" PRIx64 "\n",
			            vk::to_string(checkpoint.stage).c_str(),
			            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(checkpoint.pCheckpointMarker)));
		}
	}
	if (graphics.device_fault_enabled) {
		vk::DeviceFaultCountsEXT counts {};
		auto status = graphics.device.getFaultInfoEXT(&counts, nullptr);
		std::printf("GPU fault counts: result=%s addresses=%u vendors=%u binary=%" PRIu64 "\n",
		            vk::to_string(status).c_str(), counts.addressInfoCount, counts.vendorInfoCount,
		            static_cast<uint64_t>(counts.vendorBinarySize));
		if (status == vk::Result::eSuccess) {
			std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
			std::vector<vk::DeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
			std::vector<uint8_t> binary(counts.vendorBinarySize);
			vk::DeviceFaultInfoEXT info {};
			info.pAddressInfos = addresses.data();
			info.pVendorInfos = vendors.data();
			info.pVendorBinaryData = binary.data();
			status = graphics.device.getFaultInfoEXT(&counts, &info);
			std::printf("GPU fault: result=%s description=%s\n", vk::to_string(status).c_str(),
			            info.description.data());
			if (status == vk::Result::eSuccess || status == vk::Result::eIncomplete) {
				if (status == vk::Result::eSuccess && !binary.empty()) {
					const char* path = std::getenv("KYTY_DEBUG_GPU_FAULT_FILE");
					if (path != nullptr) {
						if (FILE* file = std::fopen(path, "wb")) {
							const auto size = std::min<uint64_t>(counts.vendorBinarySize, binary.size());
							const auto written = std::fwrite(binary.data(), 1, size, file);
							const bool closed = std::fclose(file) == 0;
							std::printf("GPU fault binary: %s bytes=%zu path=%s\n",
							            written == size && closed ? "saved" : "write failed", written, path);
						} else {
							std::printf("GPU fault binary: cannot open %s\n", path);
						}
					}
				}
				for (const auto& address: addresses) {
					std::printf("GPU fault address: type=%s address=0x%016" PRIx64
					            " precision=0x%016" PRIx64 "\n",
					            vk::to_string(address.addressType).c_str(),
					            static_cast<uint64_t>(address.reportedAddress),
					            static_cast<uint64_t>(address.addressPrecision));
				}
				for (const auto& vendor: vendors) {
					std::printf("GPU vendor fault: %s code=0x%016" PRIx64 " data=0x%016" PRIx64 "\n",
					            vendor.description.data(), static_cast<uint64_t>(vendor.vendorFaultCode),
					            static_cast<uint64_t>(vendor.vendorFaultData));
				}
			}
		}
	}
	std::fflush(stdout);
}
} // namespace

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	if (result != vk::Result::eSuccess) {
		ReportDeviceFault(m_graphics, result);
		EXIT("vkGetSemaphoreCounterValue: %s (%d), completed=%" PRIu64 " next=%" PRIu64 "\n",
		     vk::to_string(result).c_str(), static_cast<int>(result), KnownGpuTick(), CurrentTick());
	}

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	if (result != vk::Result::eSuccess) {
		ReportDeviceFault(m_graphics, result);
		EXIT("vkWaitSemaphores: %s (%d), waiting=%" PRIu64 " completed=%" PRIu64 " next=%" PRIu64 "\n",
		     vk::to_string(result).c_str(), static_cast<int>(result), tick, KnownGpuTick(), CurrentTick());
	}
	Refresh();
}

} // namespace Libs::Graphics
