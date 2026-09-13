#pragma once

#include <cstdint>
#include <string_view>

namespace Libs::Graphics {
class BufferCache;
struct ShaderComputeInputInfo;
namespace DemonsSouls {
// Compatibility policy for the title/version whose explicit compute boundaries
// and periodic-copy kernel have been checked. Unknown versions use the emulator.
constexpr bool IsSupportedVersion(std::string_view title, std::string_view version) {
	// PPSA01342 01.005.000 is a local evaluation entry: the loader patches still verify their
	// byte signatures and function hash, and the copy kernel is keyed by shader hash.
	return (title == "PPSA01341" && version == "01.007.000") ||
	       (title == "PPSA01342" && version == "01.005.000");
}
bool IsSupportedGame();
bool TryLinearCopy(const ShaderComputeInputInfo& input, BufferCache& cache, uint32_t x, uint32_t y,
                   uint32_t z, uint32_t mode);
} // namespace DemonsSouls
} // namespace Libs::Graphics
