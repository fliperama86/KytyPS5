# Cheap BDA sync (roadmap item 2, first half)

Status, September 12, 2026: cause measured (below); design P (re-protection off the render thread)
landed behind `--bda-async-protect` (default off) and measured in replay, see "P measured". The
flat SRT reads are now in-shader too, behind `--gpu-srt-reads`, and measured: they do not move
`EvaluateRuntimeSourcesImpl` ([gpu-descriptor-fetch.md](gpu-descriptor-fetch.md), "Stage 1b
measured"), so the end-to-end A/B of design P is what is left of this item.
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
  the render thread starts *processing* a guest submission (`GuestGpu::Process`, after
  `BufferInit`; not the guest's enqueue points), it waits for the helper to drain and forces one
  scan, which re-uploads every landed page before the submission's first draw. The landing
  itself bumps nothing: a per-landing generation bump was tried and cost 83 extra scans a frame
  for no gain (`c571c72`). This closes the only window in which a draw could see a stale mirror: a write to a
  U page in the microseconds between its upload and the helper's landing, read by a draw recorded
  in those same microseconds, which cannot happen across a submission boundary.
- **Everything else unchanged**: dirty marks, the generation pair, `PrepareBda`'s gate, the
  staging ring, the record. Faults on the guest threads are untouched by this design (a
  follow-up: larger tracker pages or `MEM_WRITE_WATCH` would cut those, and they are 10 ms a
  frame of guest thread time, not render thread time).

Expected on the render thread, as first written: the scan from 28.6 to about 8 us, 103 to about
95 ms. Measured (next section): the syscalls do leave the render thread, but the second uploads
are a real line item, and the net is about 5 ms a frame, not 8.

Gate: `--bda-async-protect <true|false>`, default false, and `--bda-async-protect-affinity
<guest|render>` for the helper's placement, both in [settings.md](settings.md). Uploads with
`is_written` (the GPU claims the range in the same call and it is protected no-access anyway) keep
the synchronous path; 0 to 2 calls a loop.

### P measured, September 12 (replay, nexus-6, `gpu min` over 40 loops x 2)

| per loop | gd=true off | gd=true on | gd=false off | gd=false on |
| --- | --- | --- | --- | --- |
| render-thread protect calls, upload path | 926 | **0** | 316 | **0** |
| helper protect calls (pages) | - | 696 (3,288) | - | 273 (2,399) |
| second uploads | - | 2,246 | - | 1,901 |
| boundary drains / waits / forced scans | - | 39 / 0 / 39 | - | 39 / 0 / 39 |
| `ms/loop gpu` | 76.9 | 82.1 | 72.5 | 75.0 |
| image vs reference | R9.8 G7.8 B4.1 | identical | identical | identical |

Reports: `_Runtime/_Diagnostics/replay/nexus-6/runs-p-nobump{,2}/` (shipped variant),
`runs-p-final/` (per-landing bump), `runs-p-affinity/` (guest 83.3 against render 85.4).
Off is byte-identical to the previous build. The capture analysis behind the design
(`nexus-6/analyse-protect.py`): 2,816 tracker pages dirtied a frame, two thirds of a re-protected
page is dirty again within two scans, runs of adjacent pages are short (82% one page).

What replay prices fairly is the design's overhead: 5.2 ms a loop on gd=true, almost all of it the
2,246 second uploads at about 2.3 us each (walk, staging, record; the copy itself is a fraction),
plus 1.1 ms of forced boundary scans. What replay cannot price is the saving, so it is projected
from the game's breakdown: 1,342 render-thread calls a frame inside scans x 5.4 us = 7.3 ms, plus
about 3 ms of per-bind syncs outside scans, 10.3 ms off the render thread. **Net about 5 ms a frame
on `--gpu-descriptors true` (103 to about 98 ms) and about 1.5 ms on `false`.** A replayed scan
grows from 4.1 to 6.4 us with the second uploads, so expect about 11 us in the game, not 8.

Why the second uploads are many: the game rewrites the same pages within one or two scans, so
under the old policy each rewrite cost a fault on the guest thread and a re-protection on the
render thread, and under P it costs one more copy. A cheaper trust rule needs to know whether a U
page was written, which only hardware dirty bits could say; the remaining lever on this path is
the per-upload overhead (one barrier pair and one `copyBuffer` per buffer per scan), and the way
to remove the path is A.

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

## A, no mirror for BDA-read pages: benched September 12, viable for prologue reads

Bench: docs/investigations/bda-host-memory-bench-2026-09-12.md. Imported host memory serves the
prologue's descriptor and SRT reads for about 0.2 ms of GPU time a frame; it must not serve data
reads (300 to 400x slower than the mirror). The design that follows from it, the page table
following the tracker, is in [sync-points-design.md](sync-points-design.md), "After the bench".
The paragraph below is the original note.


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
2. P behind a setting, measured in replay: done (above).
3. Flat SRT reads in-shader (second half of item 2, recompiler work, its own brief).
4. One end-to-end A/B against `--gpu-descriptors false` on the same build.
5. Bench for A.
