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

// The same two, for the calling thread alone. Design P moves the re-protection to a helper
// thread, and the process-wide counters cannot tell a render-thread call from one that merely
// happened while the render thread was scanning, so what a BDA scan reports is this pair.
[[nodiscard]] uint64_t PageProtectThreadCallCount() noexcept;
[[nodiscard]] uint64_t PageProtectThreadPageCount() noexcept;

// The same two numbers for one call, so a caller can attribute its own protections without
// reading the process-wide counters, which every thread bumps. Design P (docs/bda-sync-design.md)
// needs the render thread's upload path and the helper thread's landings counted apart.
struct ProtectStats {
	uint64_t calls = 0;
	uint64_t pages = 0;
};

class PageManager final {
public:
	PageManager();
	// The owner must stop all PageManager callers before destruction.
	~PageManager();

	KYTY_CLASS_NO_COPY(PageManager);

	[[nodiscard]] uint64_t GetPageSize() const;

	template <bool track>
	void UpdatePageWatchers(uint64_t vaddr, uint64_t size, ProtectStats* stats = nullptr);
	template <bool track, bool is_read = false>
	void UpdatePageWatchersForRegion(uint64_t base_addr, RegionBits& mask,
	                                 ProtectStats* stats = nullptr);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_PAGEMANAGER_H_
