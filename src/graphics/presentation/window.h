#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

class Presenter;

[[nodiscard]] Presenter& WindowInit(uint32_t width, uint32_t height);
// Frame replay (docs/frame-replay.md): the presenter WindowInit created, or nullptr before it.
[[nodiscard]] Presenter* WindowGetPresenter() noexcept;
void                     WindowRun();
void                     WindowShutdown();

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_WINDOW_H_ */
