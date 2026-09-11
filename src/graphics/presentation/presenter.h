#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_

#include "common/common.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Libs::Graphics {

class CommandBuffer;
class RenderContext;
struct ImageInfo;
struct WindowContext;

// Frame replay (docs/frame-replay.md): the last presented frame copied into host memory,
// tightly packed. The guest picks the video-out pixel format, so both the name and the pixel
// size have to travel with the bytes: this title presents A2B10G10R10, not an 8-bit format.
struct PresentedImage {
	uint32_t             width            = 0;
	uint32_t             height           = 0;
	uint32_t             bytes_per_pixel  = 4;
	std::string          format;
	std::vector<uint8_t> pixels;
};

class Presenter final {
public:
	struct Frame;

	explicit Presenter(WindowContext& window);
	~Presenter();
	KYTY_CLASS_NO_COPY(Presenter);

	[[nodiscard]] Frame&         PrepareFrame(CommandBuffer& command, const ImageInfo& info);
	[[nodiscard]] Frame&         PrepareBlankFrame(uint32_t width, uint32_t height, bool opaque,
	                                               CommandBuffer* producer = nullptr);
	[[nodiscard]] Frame*         PrepareLastFrame();
	[[nodiscard]] bool           IsGuestPaused() const noexcept;
	[[nodiscard]] bool           NeedsSystemOverlayRefresh() const noexcept;
	[[nodiscard]] RenderContext& Renderer() const noexcept;
	void                         Present(Frame& frame, bool reuse = false);
	// Frame replay: copies the frame the swapchain presented last into host memory. False when
	// nothing has been presented yet.
	bool                         ReadLastPresentedFrame(PresentedImage* out);
	void                         Discard(Frame& frame);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PRESENTER_H_
