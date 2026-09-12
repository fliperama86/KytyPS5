# Cheap BDA sync (roadmap item 2, first half)

Status, September 12, 2026: the cost is measured and understood (below); design P (re-protection
off the render thread) chosen; implementation in progress.
Companion to [gpu-descriptor-fetch.md](gpu-descriptor-fetch.md) ("What stage 1 needs to pay
off", point 2, and "Scan breakdown, September 12") and [performance-roadmap.md](performance-roadmap.md)
item 2.

## The problem in numbers

Parked Nexus, build 840f02d, `--gpu-descriptors true` ([gpu-descriptor-fetch.md](gpu-descriptor-fetch.md),
re-baseline): `GpuResourceManager::SynchronizeBdaBuffers` runs 399 times a frame at 26.8 us,
10.68 ms of a 103.4 ms frame. Off, it is 21 scans and 3.98 ms. Stage 1 is 17.4 ms slower than off,
and 6.7 ms of that is this scan.

What a scan is given is small: 2.9 dirty ranges, 33 KiB, about two page re-protections. The same
scan on the same inputs costs 4.1 us in a replay ([frame-replay.md](frame-replay.md), phase E),
and twelve spinning threads on the guest CPUs move it to 4.8 us.

## What a scan does today

`PrepareBda` (gpuResourceManager.cpp) runs before every draw and dispatch of a `uses_dma` or
`gpu_descriptors` program. When the buffer or mapped generation moved it takes the BDA dirty set
(`TakeBdaDirtyRanges`), intersects it with the guest's mapped ranges and calls
`SynchronizeBuffersInRange` for each hit. That walks the cached buffers overlapping the range and
calls `SynchronizeBuffer`, which:

1. asks the memory tracker for the CPU-modified sub-ranges (`ForEachUploadRange`), which clears
   their dirty state and re-protects the pages (`PageManager::UpdatePageWatchersForRegion`, one
   `NtProtectVirtualMemory` per run of pages);
2. `UploadCopies`: maps a slice of the 512 MiB host-visible staging ring and `memcpy`s each
   sub-range from guest memory into it, on the render thread;
3. records `copyBuffer` from the staging ring into the device-local mirror between two barriers.

The generation pair (`BdaGeneration`, `m_mapped_generation`) is bumped by buffer register and
retire (`ChangeRegister`), by map and unmap, and by every CPU-dirty mark (`InvalidateBda`,
`InvalidateMemory`, `MarkRegionAsCpuModified`). A CPU-dirty mark is a guest write faulting on a
protected page: the fault handler unprotects the page (another `NtProtectVirtualMemory`, on the
guest thread) and marks it.

## Where the 26.8 us goes: measured

Two steps, both on September 12.

**Step 0, writer imitation in replay** ([frame-replay.md](frame-replay.md), "Writer imitation"):
a thread on the guest CPUs rewriting each dirty page right after its mark takes a scan from 4.0
to 9.3 us; on the render CPUs 8.7 us; with twelve spinners on top 12.1 us. So freshly written
lines cost about 5 us a scan, the die boundary under 1 us, and 14 us stayed unexplained. The
hypothesis this document first carried, a cross-CCD `memcpy`, was wrong about the size of the
effect.

**Step 0b, the scan split into phases inside the game** ([gpu-descriptor-fetch.md](gpu-descriptor-fetch.md),
"Scan breakdown, September 12, 2026"; Tracy capture in
`_Runtime/_Diagnostics/replay/e2e-rebaseline/scan-breakdown/`), per scan of 28.6 us:

| phase | us a scan | share |
| --- | --- | --- |
| `PageManager::ProtectCall`, the `NtProtectVirtualMemory` syscall, 3.7 calls | 20.2 | 71% |
| `SynchronizeBuffer::Record`, barriers and `copyBuffer` | 3.0 | 10% |
| the dirty-set walk and intersection | 1.9 | 7% |
| `SynchronizeBuffer::Copy`, the `memcpy` | 1.2 | 4% |
| `MemoryTracker::Lock` | 1.0 | 3% |
| staging map, bitmap walk, rest | 1.3 | 5% |

The scan is the syscall. Each re-protection is 5.4 us in the game against about 1 us in a replay,
because changing a page's protection while sixteen guest threads run is a TLB shootdown to every
processor the process is on, and the replay has no guest threads. The whole process pays for the
same policy: 5,286 protect calls a frame, 18 ms of thread time, of which 1,342 (7.4 ms) are the
render thread inside BDA scans, about 3 ms more are the render thread's per-bind syncs outside
scans, and the rest are the guest threads' faults unprotecting pages they write. With
`--gpu-descriptors false` the render thread still pays its per-bind share, which is most of the
3.98 ms of scans in that configuration.

Consequences for the designs this document carried before: E (the `memcpy` off the render
thread) would save 1.2 us of 28.6 and is dropped. B (GPU copies from imported guest memory) does
nothing about protection and is dropped. A (no mirror, shaders read guest memory) removes the
tracking entirely for the pages it covers and stays the end state for BDA-read pages, still
gated on the host-memory read bench (below). What stage 1 needs now is a tracker policy that
keeps the render thread out of `NtProtectVirtualMemory`.

## Design P: re-protection leaves the render thread

The tracker keeps its states and its faults. What changes is who re-protects an uploaded page
and when the mirror is trusted again.

- **A new page state, U (uploaded, unprotected).** A scan that uploads a dirty page no longer
  protects it. It leaves the page writable, moves it to U, and queues a protect request for a
  helper thread. The render thread makes no `NtProtectVirtualMemory` call in the upload path.
- **The helper thread** (one, any affinity group; measure both) drains the queue: it coalesces
  adjacent pending pages into one call per contiguous run (a run may span already-protected
  pages, whose protection it re-applies unchanged), calls `NtProtectVirtualMemory`, then marks
  each page "landed" and bumps the BDA generation once per batch. Every shootdown still
  interrupts every processor, the render thread included; what is removed is the syscall's own
  latency from the render thread's critical path, 1,342 times a frame.
- **Trusting the mirror again.** A U page may have been written after its upload without a
  fault. The upload that follows the landing catches everything written before the protection
  took effect, so: the next scan after a batch lands uploads every landed U page once more and
  moves it to P (protected, clean). A write after the landing faults and marks as today. One
  extra `memcpy` per page, 0.3 us.
- **Submission boundary.** Draws of a submission may read only what the guest wrote before
  submitting it (the console's rule; anything else reads garbage on the console too). So when
  the render thread starts processing a guest submission (`GuestGpu::Submit`, `SubmitCompute`,
  and `Done`), it waits for the helper to drain and for the landed pages to be re-uploaded by the
  next scan, which the landing's generation bump forces. A few waits a frame, normally already
  satisfied. This closes the only window in which a draw could see a stale mirror: a write to a
  U page in the microseconds between its upload and the helper's landing, read by a draw recorded
  in those same microseconds, which cannot happen across a submission boundary.
- **Everything else unchanged**: dirty marks, the generation pair, `PrepareBda`'s gate, the
  staging ring, the record. Faults on the guest threads are untouched by this design (a
  follow-up: larger tracker pages or `MEM_WRITE_WATCH` would cut those, and they are 10 ms a
  frame of guest thread time, not render thread time).

Expected on the render thread: the scan drops from 28.6 to about 8 us (walk 1.9, record 3.0, copy
1.2 plus the extra upload, lock, rest); 399 scans a frame from 10.7 ms to about 3 ms; the per-bind
syncs outside scans lose their protect share too. `--gpu-descriptors true` from 103 to about 95
ms, `false` from 86 to about 83. Still slower on than off until the flat reads move in-shader
(second half of item 2), which removes most of the 30 ms of `EvaluateRuntimeSourcesImpl`.

Gate: a runtime setting, default off, listed in [settings.md](settings.md).

### Measuring P in replay

The replay reproduces the counts, not the syscall's game-side cost: a protect call is about 1 us
there and 5.4 us in the game. So P is judged in replay by counts and correctness, and its
milliseconds are the counts times the game's per-call cost:

- render-thread `PageManager::ProtectCall` count per loop: 1,342 a frame today, must go to 0 in the
  upload path;
- helper protect calls per loop (after coalescing), extra uploads per loop, submission-boundary
  waits per loop and their total time;
- `ms/loop gpu` for the residual (walk, record, copy), which the replay does measure fairly;
- the image against the reference, in both `--gpu-descriptors` settings.

Then one end-to-end A/B on the same build closes the number.

## Parked: A, no mirror for BDA-read pages

The BDA page table points at guest memory imported with `VK_EXT_external_memory_host` instead of
the mirror, for pages only read through BDA. No scan, no fault, no protection for those pages.
Unknown: the cost of a dependent chain of uniform loads from host memory over PCIe in a wave's
prologue, 9,300 draws a frame. `bda_load_bench` (docs/investigations/bda-load-bench-2026-09-11.md)
measured device-local memory only; the same bench with the fixture imported from host memory,
uniform pattern plus a single-wave dependent chain, decides it. Two hours, after P and the flat
reads. Imports have to follow commit and decommit (`Common::VirtualMemory::Commit` / `Decommit`,
src/common/virtualMemory.cpp), which is the bookkeeping risk.

## Plan

0. Writer imitation in replay: done, partial (above).
0b. Scan breakdown in the game: done, decisive (above).
1. Capture analysis, no runs: from `nexus-6`'s dirty and prepare events, per frame the distinct
   dirty pages, how often each is re-protected, and the run lengths of adjacent pages, to size
   the helper's coalescing. Half an hour.
2. P behind a setting, measured in replay as above.
3. Flat SRT reads in-shader (second half of item 2, recompiler work, its own brief).
4. One end-to-end A/B against `--gpu-descriptors false` on the same build.
5. Bench for A.
