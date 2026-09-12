# Copy-free BDA sync (roadmap item 2, first half)

Status, September 12, 2026: design written; step 0 done (partial reproduction, see its result);
step 0b (scan breakdown in the game) in progress.
Companion to [gpu-descriptor-fetch.md](gpu-descriptor-fetch.md) ("What stage 1 needs to pay
off", point 2) and [performance-roadmap.md](performance-roadmap.md) item 2.

## The problem in numbers

Parked Nexus, build 840f02d, `--gpu-descriptors true` ([gpu-descriptor-fetch.md](gpu-descriptor-fetch.md),
re-baseline): `GpuResourceManager::SynchronizeBdaBuffers` runs 399 times a frame at 26.8 us,
10.68 ms of a 103.4 ms frame. Off, it is 21 scans and 3.98 ms. Stage 1 is 17.4 ms slower than off,
and 6.7 ms of that is this scan.

What a scan is given is small: 2.9 dirty ranges, 33 KiB, about two page re-protections. The same
scan on the same inputs costs 4.1 us in a replay ([frame-replay.md](frame-replay.md), phase E),
and twelve spinning threads on the guest CPUs move it to 4.8 us. So the cost is not the scan's
inputs, not thread count, not the number of protection changes, and not buffer churn (the parked
Nexus has none).

## What a scan does today

`PrepareBda` (gpuResourceManager.cpp) runs before every draw and dispatch of a `uses_dma` or
`gpu_descriptors` program. When the buffer or mapped generation moved it takes the BDA dirty set
(`TakeBdaDirtyRanges`), intersects it with the guest's mapped ranges and calls
`SynchronizeBuffersInRange` for each hit. That walks the cached buffers overlapping the range and
calls `SynchronizeBuffer`, which:

1. asks the memory tracker for the CPU-modified sub-ranges (`ForEachUploadRange`), which clears
   their dirty state and re-protects the pages (a kernel call per run of pages);
2. `UploadCopies`: maps a slice of the 512 MiB host-visible staging ring and `memcpy`s each
   sub-range from guest memory into it, on the render thread;
3. records `copyBuffer` from the staging ring into the device-local mirror between two barriers.

The generation pair (`BdaGeneration`, `m_mapped_generation`) is bumped by buffer register and
retire (`ChangeRegister`), by map and unmap, and by every CPU-dirty mark (`InvalidateBda`,
`InvalidateMemory`, `MarkRegionAsCpuModified`). In the parked Nexus, 4.8% of the 9,845
preparations a frame find a moved generation, and those are the 399 scans.

## Where the 26 us goes: hypothesis

Step 2 is the only part of a scan that touches the guest's bytes. In the game those bytes were
written moments earlier by a guest thread on the other CCD (the affinity policy puts the render
thread on the X3D CCD and the guest on the other one). Every cache line the `memcpy` reads is
dirty in a remote core's cache, so each line is a cross-CCD transfer: 33 KiB is 528 lines, and
26 us over 528 lines is 49 ns a line, which is what a cross-CCD dirty-line fetch costs on Zen 5
once memory-level parallelism is counted. In a replay nothing writes those pages between loops:
the lines are cold in DRAM, and the hardware prefetcher streams them at 4 us per 33 KiB, about
8 ns a line. The two numbers fit the same mechanism.

Unproven. Step 0 below tests it in the replay in an hour. If it holds, the harness reproduces the
cost and every design below can be judged in seconds. If it does not, the next suspect is lock
contention with guest threads inside the tracker (`RegionManager::lock`) or the kernel calls
under real guest load, and the measurement that settles it is sub-zones inside the scan
(`memcpy`, protect, lock) recorded into the capture's prepare events by one game run.

### Step 0 result, September 12

The writer imitation ([frame-replay.md](frame-replay.md), "Writer imitation") raises a scan from
4.0 us to 9.3 us with the writer on the guest CPUs and to 8.7 us with it on the render CPUs;
twelve spinners on top take it to 12.1 us. Reports in
`_Runtime/_Diagnostics/replay/nexus-6/runs-20260912-012758` and `runs-20260912-012958`.
So fresh lines in another core's cache are real and worth about 5 us a scan, the die boundary
adds under 1 us, and about 14 us of the game's 26.8 us are still unexplained. Two candidates
the imitation does not cover: the guest writing the same page *while* the scan copies it (true
sharing, which costs far more per line than a one-time fetch; the game's bump allocators keep
writing the page the last draw dirtied), and the re-protection kernel calls or the tracker lock
under real guest load. The decisive measurement is the breakdown of the scan inside the game:
Tracy sub-zones for the tracker walk (lock and protect), the copy and the record, one warm run
with `--gpu-descriptors true`. That is step 0b; the design choice below waits for it, because E
only helps if the copy is where the time goes.

## Designs

Three ways to stop the render thread copying freshly written guest memory, from smallest to
largest. They are not exclusive: E is the step that makes stage 1 pay off now, B and A are the
mirror-free end state and each needs a number before it is chosen.

### E. The copy leaves the render thread (recommended first)

Keep the mirror, the staging ring and the dirty tracking. Change only who does step 2:

- The render thread still runs `ForEachUploadRange` (clear dirty, re-protect), still reserves the
  staging slice and still records the `copyBuffer` and its barriers. It no longer `memcpy`s: it
  pushes one job per copy (guest address, staging pointer, size) to an upload helper thread and
  moves on. Render-thread cost of a scan becomes the walk, the reservation and the record: about
  2 us on the replay's numbers.
- One helper thread, pinned with the guest affinity group (`Common::ApplyThreadAffinity`,
  `ThreadAffinityGroup::Guest`), pops jobs and performs the `memcpy` (`TryReadBacking`, and
  `TryReadPrtBacking` for aperture ranges, exactly the reads `UploadMemory` makes today). Its
  cost is off the critical path; at 26 us a scan it is 10 ms a frame of a core the guest is not
  saturating. Measure guest-group and render-group placement both; the cross-CCD transfer has to
  happen once either way, and where it is cheapest is an experiment.
- `CommandScheduler::Flush` (and therefore `FlushAndWait`, downloads and presents) waits for the
  helper's outstanding jobs before `vkQueueSubmit`. Jobs are microseconds and a command buffer
  spans many draws, so the wait is normally already satisfied. The staging ring's slice is
  reserved on the render thread, so ring reuse is fenced exactly as today; if `Commit` flushes
  non-coherent memory it moves to the helper, after the copy.

Semantics: today the re-protection precedes the `memcpy` by nanoseconds; with E by microseconds.
A guest write in that window faults, re-marks the page dirty and is uploaded by the next scan,
in both designs; the copy in flight carries a torn page in both designs. That is the console's
semantics too (the GPU reads whatever is in memory when it runs), so no game that works on the
console can depend on the narrower window.

Expected: 10.7 ms to about 1 ms of render-thread time on `--gpu-descriptors true`, so 103 ms
to about 93 ms. Still slower than off (86 ms) until the flat reads move in-shader (second half of
item 2), which removes most of the 31 ms of `EvaluateRuntimeSourcesImpl`.

Gate: a runtime setting, default off, listed in [settings.md](settings.md).

### B. The GPU copies from imported guest memory

`VK_EXT_external_memory_host`: import each committed guest range as a `VkDeviceMemory` with a
`VkBuffer` over it (`eTransferSrc`, later `eShaderDeviceAddress`), and record `copyBuffer` from the
import into the mirror. Nothing on the CPU reads the bytes; the DMA engine fetches them over
PCIe with cache snooping. The render thread's scan becomes the walk and the record, as in E, and
the staging ring is out of the upload path.

Needs: imports made at commit granularity, not map granularity, and released before any
decommit (`Common::VirtualMemory::Commit` and `Decommit` in src/common/virtualMemory.cpp are the
choke points; the memory pool commit and the PRT aperture paths must go through them);
`minImportedHostPointerAlignment` (4 KiB on NVIDIA, guest pages are 16 KiB) and sizes rounded to
it; the driver pinning several GB of guest memory at load, which costs time once and physical
memory permanently. Risk: a decommit while pinned leaves the import pointing at the old physical
pages, and a later commit silently reads stale data. That bookkeeping is the whole cost of B over
E, and E already removes the render-thread cost. B is worth it only as the road to A.

### A. No mirror for BDA reads

The BDA page table points at the imported guest memory instead of the mirror for pages that are
only read through BDA. No scan, no dirty tracking, no copy for those pages; the page table changes
only at map, unmap, commit and decommit. The shader reads system memory over PCIe, which is the
console's semantics exactly.

Unknown: what a dependent chain of uniform loads from host memory costs a wave. The SRT prologue
is three to five dependent loads; over PCIe each is about a microsecond rather than the
sub-microsecond of VRAM, and 9,300 draws a frame with small waves could turn that into tens of
milliseconds of GPU time, or into nothing if the driver caches host reads in L2 and draws overlap.
`bda_load_bench` (docs/investigations/bda-load-bench-2026-09-11.md) measured device-local memory
only. The number that decides A: the same bench with the 257 MB fixture imported from host memory,
uniform pattern, plus a single-wave dependent-chain variant for latency. Two hours. Not before
step 0 and E.

## Plan

0. **Writer imitation in replay.** `--replay-writer <none|guest|render>`, default none. A thread
   pinned to the named affinity group rewrites (load, store the same value, one store per 64-byte
   line) every byte of a dirty event's range immediately before the render thread applies the
   mark, so the scan that follows copies lines that are dirty in another core's cache. Synchronous
   handoff at the mark (the render thread waits for the writer, and the wait is timed and reported
   separately so it can be subtracted). Success: the per-scan cost in `replay-report.json` goes
   from 4 us to 15 us or more with `guest`; `render` tells whether same-CCD freshness is enough.
   Hard stop if it stays near 4 us: the hypothesis is wrong, instrument the scan in the game
   instead.
1. **E**, behind a setting, measured in replay with the writer on: render-thread scan cost back to
   about 2 us, `ms/loop gpu` on `--gpu-descriptors true` down by the difference times 462, image
   unchanged, `--gpu-descriptors false` unchanged.
2. **Flat SRT reads in-shader** (second half of item 2, recompiler work, its own brief): measured
   by `EvaluateRuntimeSourcesImpl` calls and time per loop in replay.
3. One end-to-end A/B against `--gpu-descriptors false` on the same build.
4. Bench for A when 1 to 3 are in.
