#include "graphics/host_gpu/renderer/cache/descriptorFeedback.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <bit>

namespace Libs::Graphics {

DescriptorFeedback::DescriptorFeedback(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler),
      m_bits(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, BitBytes),
      m_download(graphics, scheduler, MemoryUsage::Download, 0, AllFlags,
                 MaxPendingReads * BitBytes) {
	SetVulkanObjectNameF(m_graphics.device, m_bits.Handle(), "Descriptor Feedback Buffer");
}

Buffer* DescriptorFeedback::GetBuffer() {
	// Binding happens while a render pass is open, so nothing is recorded here; the first
	// Process() clears the allocation instead.
	m_in_use = true;
	return &m_bits;
}

void DescriptorFeedback::Process() {
	if (!m_in_use) {
		return;
	}
	if (!m_cleared) {
		// First pass after the buffer was bound: the allocation held whatever the device left
		// there, so discard this window rather than reading it as feedback. A shader that really
		// did report a mismatch reports it again on its next draw.
		m_cleared = true;
		m_scheduler.EndRendering();
		m_bits.Fill(0, BitBytes, 0);
		return;
	}
	if (const auto wait_tick = m_areas[m_current_area]; wait_tick != 0) {
		m_scheduler.Wait(wait_tick);
		m_scheduler.PopPendingOperations();
	}

	const auto offset = uint64_t {m_current_area} * BitBytes;
	m_scheduler.EndRendering();
	auto& command = m_scheduler.Current();
	EXIT_IF(command.IsInvalid());
	// The copy reads what the shaders have written; the clear behind it is ordered by the copy's
	// own release barrier, so every bit that reaches the download area is exactly a bit cleared.
	m_download.CopyFrom(command, m_bits, 0, offset, BitBytes, vk::AccessFlagBits::eShaderWrite,
	                    vk::AccessFlagBits::eHostRead, vk::AccessFlagBits::eMemoryRead,
	                    vk::AccessFlagBits::eHostRead);
	m_bits.Fill(0, BitBytes, 0);

	const auto area   = m_current_area;
	auto*      mapped = m_download.Mapped().data() + offset;
	m_scheduler.DeferOperation([this, mapped, offset, area] {
		m_download.Invalidate(offset, BitBytes);
		const auto* words = std::bit_cast<const uint32_t*>(mapped);
		if (m_sink) {
			for (uint32_t index = 0; index < BitWords; index++) {
				auto word = words[index];
				while (word != 0u) {
					const auto bit = static_cast<uint32_t>(std::countr_zero(word));
					word &= word - 1u;
					m_sink(index * 32u + bit);
				}
			}
		}
		m_areas[area] = 0;
	});

	m_areas[m_current_area++] = m_scheduler.CurrentTick();
	m_current_area %= MaxPendingReads;
}

} // namespace Libs::Graphics
