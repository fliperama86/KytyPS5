#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_DESCRIPTORFEEDBACK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_DESCRIPTORFEEDBACK_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {

// Storage buffer of u32 bits bound at DescriptorBindingKind::DescriptorFeedback, one bit per
// program-cache feedback slot (docs/gpu-descriptor-fetch.md, stage 1). A shader whose buffer
// descriptors are fetched on the GPU compares the runtime V# fields it was specialized against
// with the baked constants and sets its slot's bit on any mismatch.
//
// The bits are read back on the fault-buffer schedule and with the same mechanism: the bit buffer
// is copied into one area of a host-visible ring, cleared on the GPU right behind the copy, and
// scanned in a deferred operation once that submission retires. Nothing drains the GPU, and
// because the copy and the clear are ordered in the same command stream, no bit is cleared
// without having been read.
//
// Nothing is recorded until a shader binds the buffer, so a session that never enables
// --gpu-descriptors issues no command of its own for it.
class DescriptorFeedback {
public:
	// One bit per slot. 65,536 slots is 8 KiB of device memory and far more programs than a
	// session compiles.
	static constexpr uint32_t MaxSlots = 65536;

	DescriptorFeedback(GraphicContext& graphics, CommandScheduler& scheduler);
	KYTY_CLASS_NO_COPY(DescriptorFeedback);

	// Marks the buffer live. The host must never read whatever the allocation happened to
	// contain, so the first Process() clears it and discards that window.
	[[nodiscard]] Buffer* GetBuffer();

	// Receives every slot whose bit was set since the previous read. Installed once by the render
	// context; runs inside the deferred operation, on the thread that retires it.
	void SetSink(Common::UniqueFunction<void, uint32_t>&& sink) { m_sink = std::move(sink); }

	// Records the copy and the clear and defers the scan. A no-op until the buffer is bound.
	void Process();

private:
	static constexpr uint64_t BitWords        = MaxSlots / 32u;
	static constexpr uint64_t BitBytes        = MaxSlots / 8u;
	static constexpr uint32_t MaxPendingReads = 8;

	GraphicContext&                        m_graphics;
	CommandScheduler&                      m_scheduler;
	Buffer                                 m_bits;
	Buffer                                 m_download;
	Common::UniqueFunction<void, uint32_t> m_sink;
	std::array<uint64_t, MaxPendingReads>  m_areas {};
	uint32_t                               m_current_area = 0;
	bool                                   m_in_use       = false;
	bool                                   m_cleared      = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_DESCRIPTORFEEDBACK_H_
