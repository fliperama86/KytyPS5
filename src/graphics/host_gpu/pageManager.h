#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_

#include "common/common.h"
#include "graphics/host_gpu/regionDefinitions.h"

#include <memory>

namespace Libs::Graphics {

enum class PageFaultAccess { Read, Write, Execute, Unknown };

// How many host page-protection changes the tracker has made, and how many tracker pages they
// covered, since the process started. Two relaxed atomic adds per NtProtectVirtualMemory call;
// the frame replay reads them around a BDA scan to report the re-protection each scan causes
// (docs/frame-replay.md, phase E, and docs/investigations/render-thread-kernel-2026-09-10.md).
[[nodiscard]] uint64_t PageProtectCallCount() noexcept;
[[nodiscard]] uint64_t PageProtectPageCount() noexcept;

class PageManager final {
public:
	PageManager();
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	[[nodiscard]] uint64_t GetPageSize() const;

	template <bool track>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size);
	template <bool track, bool is_read = false>
	void UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
