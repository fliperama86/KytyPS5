#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKDIAGNOSTICS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKDIAGNOSTICS_H_

// Characterisation of the GPU readbacks the SRT evaluation takes on the render thread
// (docs/performance-roadmap.md, item 1b). A read fault on a GPU-dirty page sends
// GpuResourceManager::HandleFault into BufferCache::ReadMemory, which downloads the page and
// drains the device. Everything here is inert unless --gpu-readback-diagnostics is on; it is a
// measurement tool, not a shipping path.

#include "common/common.h"

#include <atomic>
#include <cstdint>
#include <filesystem>

namespace Libs::Graphics::ReadbackDiag {

// Which part of an SRT evaluation was reading guest memory when the fault landed. The packed
// program knows exactly (its schedule is one array, split at source_prefix); the two fallback
// forms report their phase.
enum class RootKind : uint8_t {
	Outside = 0, // no evaluation was running: some other render-thread read faulted
	Source,      // a descriptor source root
	FlatRead,    // a flattened SRT scalar read
	CleanRead,   // a flat read taken through the clean (specialization) reader
	Condition,   // a resource control-flow condition
	Unknown,     // an evaluation was running but its form does not say where
};

enum class EvalForm : uint8_t { None = 0, Packed, Flat, Walker };

// The evaluation the calling thread is inside, if any. EvaluateRuntimeSourcesImpl owns the outer
// two fields; the execution forms narrow `kind` as they go.
struct Evaluation {
	uint64_t hash   = 0;
	uint32_t stage  = 0;
	EvalForm form   = EvalForm::None;
	RootKind kind   = RootKind::Outside;
	bool     active = false;
};

namespace Detail {
inline std::atomic_bool g_enabled {false};
// Always on, even with the characterisation off: two counters cheap enough to keep in a measured
// build, so every replay report can print the item 1b headline numbers (read faults a loop, and
// the device wait inside them) beside the submit count. About a hundred increments a loop.
inline std::atomic<uint64_t> g_fault_count {0};
inline std::atomic<uint64_t> g_fault_wait_ns {0};
} // namespace Detail

[[nodiscard]] inline bool Enabled() noexcept {
	return Detail::g_enabled.load(std::memory_order_relaxed);
}
void Enable(bool on);

// A read fault the render thread took on a GPU-dirty page, and the time the download's drain (or
// its own submission) waited for the device.
inline void CountFault() noexcept {
	Detail::g_fault_count.fetch_add(1, std::memory_order_relaxed);
}
inline void CountWait(uint64_t wait_ns) noexcept {
	Detail::g_fault_wait_ns.fetch_add(wait_ns, std::memory_order_relaxed);
}
[[nodiscard]] inline uint64_t TotalFaults() noexcept {
	return Detail::g_fault_count.load(std::memory_order_relaxed);
}
[[nodiscard]] inline uint64_t TotalWaitNs() noexcept {
	return Detail::g_fault_wait_ns.load(std::memory_order_relaxed);
}

[[nodiscard]] Evaluation& Current() noexcept;

// Guards one evaluation. Cheap enough to construct unconditionally: two stores when the
// diagnostics are off.
class EvaluationScope {
public:
	EvaluationScope(uint64_t hash, uint32_t stage) noexcept {
		if (!Enabled()) {
			return;
		}
		auto& current  = Current();
		m_saved        = current;
		m_owned        = true;
		current        = Evaluation {};
		current.hash   = hash;
		current.stage  = stage;
		current.active = true;
		current.kind   = RootKind::Unknown;
	}
	~EvaluationScope() noexcept {
		if (m_owned) {
			Current() = m_saved;
		}
	}
	KYTY_CLASS_NO_COPY(EvaluationScope);

private:
	Evaluation m_saved;
	bool       m_owned = false;
};

// The producer side. ObtainBuffer(is_written) is where a range becomes GPU-owned; the block map
// remembers which submission wrote it and how far the GPU thread had got.
void NoteGpuWrite(uint64_t address, uint64_t size, uint64_t tick, uint32_t progress);

// The consumer side, all on the render thread. BeginFault opens a record, the buffer cache fills
// the download fields, EndFault closes it.
void BeginFault(uint64_t vaddr);
void NoteDownloadRange(uint64_t address, uint64_t size);
// The newest producer tick seen by NoteDownloadRange so far, so the caller can ask the scheduler
// whether it has already retired. 0 when no downloaded range has a known producer.
[[nodiscard]] uint64_t PendingProducerTick();
void NoteDownloadTotals(uint64_t window_bytes, uint64_t copies, uint64_t current_tick,
                        uint32_t current_progress, bool producer_complete);
void NoteWait(uint64_t wait_ns);
// Whether this readback could have taken item 1b's own-submission path, and whether it did.
void NoteEligible(bool eligible, uint32_t reason);
void NoteOwnSubmission(bool own);
void EndFault();

void     SetLoop(uint32_t loop);
uint64_t FaultCount();
void     Dump(const std::filesystem::path& path);

} // namespace Libs::Graphics::ReadbackDiag

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_READBACKDIAGNOSTICS_H_
