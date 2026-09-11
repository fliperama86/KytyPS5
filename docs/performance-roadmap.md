# Demon's Souls performance roadmap — September 11, 2026

The single page that ranks what is left. Each item points at the document that studied it. Read
this first, then the linked document, then the code. The measurement protocol is in
[reaching-the-nexus.md](reaching-the-nexus.md) with the corrections at the end of this page.

## Goal and budget

Goal: the GPU, not the render thread, decides the frame rate in the parked Nexus.

Facts from the steady-state Tracy capture `_Runtime/_Diagnostics/gpufetch/real3.tracy`
(`real3-self.csv`, 15 s, build 3fabe81 with `--gpu-descriptors true`, Remote Desktop session):

- One thread, `Thread_Gpu`, parses the PM4 stream and records Vulkan. Its zone self time sums to
  15.57 s over the 15 s capture: it is saturated. 146 presents, 106 ms per frame.
- Per frame: 9,306 draws (8,794 of them indirect), 1,647 dispatches, about 11,000 events. That
  is about 9 µs of render thread per event.
- GPU device utilization 28% (`real3-steady-clean.json`), so about 28 ms of GPU work per frame.
  GPU-bound in this scene is therefore about 35 FPS, and the render thread must spend under
  about 28 ms per frame, about 2.5 µs per event.
- The twelve guest threads spin waiting for the GPU (12.5 core equivalents). That is the game,
  not the emulator, and it does not change the budget above.

Where the 106 ms goes, by source file, self time:

| File | ms/frame | share | what it is |
| --- | --- | --- | --- |
| SrtWalker.cpp | 28.1 | 26% | SRT evaluation on the CPU (`EvaluateRuntimeSourcesImpl`) |
| renderDraw.cpp | 13.7 | 13% | per-draw recording: index buffer, shader refresh, depth target, draw |
| pm4Handlers.cpp | 13.6 | 13% | PM4 packet handlers, indirect argument sync |
| gpuResourceManager.cpp | 10.5 | 10% | BDA dirty-page sync (`SynchronizeBdaBuffers`), new with stage 1 |
| descriptors.cpp | 9.9 | 9% | Vulkan binding: rebind buffers and images, commit |
| videoOut.cpp | 7.1 | 7% | flip queue |
| graphicsRun.cpp | 6.6 | 6% | command processor loop |
| ResourceMaterialization.cpp, pipelineCache.cpp, shader.cpp | 9.7 | 9% | materialization, program lookup, shader params |
| rest | 7.4 | 7% | compute recording, swapchain, render targets, allocator |

No single item closes the gap. The per-event cost has to fall about four times, which means the
render thread stops touching descriptors per draw, stops issuing one Vulkan call per draw, and
stops parsing on the critical path.

## Item 0: the frame replay harness

Status, September 11, 2026: **built and validated**, with one qualification. Scoped and measured in
[frame-replay.md](frame-replay.md). Record one parked-Nexus frame, replay it in a loop without the
game. Every item below is measured in replay first; one end-to-end run per integrated item; the
full A/B only at milestones.

How to use it: `_Build/replay-des.ps1 -Capture _Runtime/_Diagnostics/replay/nexus-2 -Loops 60
-Repeats 2 -Configs '<flags A>', '<flags B>'` prints the comparison table and keeps the reports. A
60-loop run is 10 s plus a 6 s warm-up; a two-configuration, two-repeat bench is four minutes
against the 40 minutes an end-to-end A/B costs. The capture to use is
`_Runtime/_Diagnostics/replay/nexus-5` (format version 5, frame 1739, 6.6 GiB, 40 submissions).

What it reproduces: the parked-Nexus render thread at 71 ms a loop against 92 ms measured
end to end (no guest threads compete in replay), with the frame's zone structure intact — 20 867
`EvaluateRuntimeSourcesImpl` calls at 24 ms a loop against 20 350 at 28.1 ms a frame, 245
`CpOpDispatchIndirect::SyncArguments` a loop against 245 a frame. Run-to-run repeatability is 0.05
to 0.9%. The presented image matches the capture screenshot bar exposure and two streamed HUD icons.

What it does not reproduce: **anything whose cost is the guest's memory churn within a frame.**
Phase D (format version 3, September 11, 2026) records every CPU write of the frame against a
draw-and-dispatch progress clock and replays it from a marker thread at the same point, exactly —
0 late events, 11 259 of 11 259 progress reached — which took
`GpuResourceManager::SynchronizeBdaBuffers` from 28 calls a loop for 0.23 ms to 172 for 1.56 ms,
against 352 for 10.47 ms in the game, and moved the `--gpu-descriptors` ratio from 1.00 to 0.95
against 1.10 end to end. The remainder is not page writes: the BDA generation is also bumped by
every buffer registration and retirement and by every map, and a replay loop reuses the buffer set
loop 1 built. Items 1, 3 and 4 below do not depend on any of this and are safe to measure in replay;
item 2 is not, and needs the end-to-end protocol. Evidence in
[frame-replay.md](frame-replay.md), "Phase D, CPU-write timing".

Phase E (format versions 4 and 5, September 11, 2026) corrected that last paragraph and closed the
count. The parked Nexus has **no buffer churn at all** -- zero registrations, retirements, maps and
unmaps in each of six consecutive captured frames -- so the residue was never a missing source of
BDA-generation bumps. The capture now records the game's own scan timeline (`prepare-events.bin`:
every `PrepareBda` call and whether it scanned), the progress clock ticks twice per draw and
dispatch, and the marks are applied inline on the GPU thread: the replay makes **9479 preparations
against 9479 recorded and 156 scans against 160**, with 0 late marks. The 352-scan, 10.5 ms target
belongs to the September 10 build, where the draw path made no BDA preparations at all; on this
build the game itself scans 160 times a frame, so the `--gpu-descriptors` A/B (replay ratio 1.005)
cannot reproduce an effect that is no longer there and needs re-baselining end to end. What is still
not reproduced is the *cost* of a scan: the replay's scans walk 17.2 dirty ranges against the game's
4.3, because a replayed frame re-marks the whole recorded dirty page set.

## Items, in recommended order

Expected gains are projections from the self-time shares above unless a measurement is cited.
Measure each item before starting the next; the protocol is at the bottom.

### 1. Indirect draws and dispatches consumed by the GPU

Status: not started. Studied here and in
[performance-handoff-2026-09-10.md](performance-handoff-2026-09-10.md) ("What is left", item 1).
Phase C of the replay harness added direct evidence that these readbacks serialize the CPU on the
device: on the title-screen capture, where the render thread is faster than the GPU, the loop time
steps from 11 ms to 28 ms as soon as a few loops of work are queued ahead, and every one of those
16 ms lands in `CpOpDispatchIndirect::SyncArguments`
([frame-replay.md](frame-replay.md), "The sawtooth").

The game is GPU-driven: its compute shaders write the draw and dispatch arguments, and 8,794 of
the 9,306 draws per frame are `DRAW_INDIRECT` packets. The emulator reads every argument block
back on the CPU and re-issues the work directly:

- `CommandProcessor::DrawIndirect` and `DrawIndirectMulti` (src/graphics/guest_gpu/graphicsRun.cpp)
  call `SyncGpuCleanBacking` (src/kernel/memory.cpp), which downloads the range when it is GPU-dirty
  (`BufferCache::ReadMemory` → `DownloadBufferMemory` → `Scheduler::WaitPriorityOperations`, a
  wait for the GPU), then `memcpy` the arguments and call `DrawIndex`/`DrawIndexAuto`.
- `CpOpDispatchIndirect` (pm4Handlers.cpp) and `CommandProcessor::DispatchIndirect` do the same
  and call `DispatchDirect`. The handoff measured about 245 GPU drains per frame on this path
  (`CpOpDispatchIndirect::SyncArguments`, 3.4% of the thread in `real3-self.csv`).
- The host renderer has one native indirect call, `dispatchIndirect` in
  src/graphics/host_gpu/renderer/renderCompute.cpp, reached only when `indirect_args` is set.
  There is no `drawIndexedIndirect` anywhere in src/graphics/host_gpu.

Work: obtain the argument buffer from the buffer cache, add the indirect-read barrier, record
`drawIndexedIndirect`/`drawIndirect`/`dispatchIndirect`, and stop reading the arguments on the
CPU. The CPU still binds resources per draw, so this removes the readback and the waits, not the
per-draw cost. Per-draw state that today comes from the arguments (`m_num_instances`, the
`IndirectArgs` offset source) has to come from the packet or from the shader instead; check
`DrawIndexAuto` and `DrawIndex` for every use of the copied `args`.

Expected: unknown in FPS. It removes CPU-to-GPU serialization points, so the GPU should stop
idling between waits and CPU and GPU should overlap. The capture has no GPU timestamps, so
how much of the 72% GPU idle time the waits cause is not measured. Cheap enough to do and
measure first. Gate: a runtime setting (see [settings.md](settings.md)).

### 2. Finish stage 1 of GPU-side descriptor fetch

Status: stage 1 landed behind `--gpu-descriptors` (default off), measured slower, causes known.
[gpu-descriptor-fetch.md](gpu-descriptor-fetch.md), "Stage 1 measured".

Two pieces, both foreseeable from the stats dump:

- Move the flattened SRT scalar reads (`srt_reads`, the `flat_reads` roots) in-shader with the
  same lowering as the descriptor roots. Then `EvaluateRuntimeSourcesImpl` no longer walks the
  chain for fetched programs. Removes most of the 28 ms.
- Make the per-draw dirty sync cheap: track dirtied pages as a list instead of scanning dirty
  ranges against mapped ranges on every generation change (`PrepareBda`, about 340 scans per
  frame at 30 µs). Removes most of the 10.5 ms.

Expected: about 16 FPS in this scene (projection in the design document).

### 3. Stages 2 and 3: vertex fetch in-shader, bindless images and samplers

Status: designed, not started. [gpu-descriptor-fetch.md](gpu-descriptor-fetch.md), "Stage 2" and
"Stage 3".

After stage 3 the CPU does per draw: pipeline memo, index buffer, push constants, one draw call.
Removes much of descriptors.cpp (9.9 ms) and of the remaining materialization and shader-param
work (9.7 ms).

Expected: about 20 FPS.

### 4. Stage 4: multi-draw indirect

Status: sketched in one sentence in the design document, not costed. Depends on items 1 and 3:
draws must no longer differ in bindings, and the arguments must already live on the GPU.

Consecutive draws with the same pipeline become one `drawIndexedIndirect` with a count, the
shader indexing its per-draw data by draw ID. Removes most of renderDraw.cpp (13.7 ms).

Expected: about 24 FPS. Needs a stats dump of run lengths (consecutive same-pipeline draws)
before committing; the stats collector in `srtStats.cpp` is the place to add the counter.

### 5. Split the render thread

Status: studied in [performance-handoff-2026-09-10.md](performance-handoff-2026-09-10.md)
("What is left", item 2, "front-stage threading"), not started.

PM4 parsing and evaluation on one thread, Vulkan recording on another, pipelined. Hides
pm4Handlers.cpp plus graphicsRun.cpp (about 20 ms). The handoff lists the blocker
(`IsGpuThread` gate) and the safety argument (CP memory writes are eager). Ceiling measured
there as about 1.6x on its own.

Expected: with items 1 to 4 done, this is where the GPU becomes the limit, about 35 FPS.

### Parked: first-encounter stutter

Not on the frame-rate path. The driver pipeline cache persists (`_PipelineCache/<title>.bin`,
Release builds only, saved periodically and at exit), but the list of shaders seen, the GCN to
SPIR-V translation and the resource plan are not, so every session re-translates each shader on
its first use, on the render thread. The `Shaders: VS | PS | CS` console line counts them. The
fix is a recorded shader list replayed at boot on a background thread; how much of the stutter
is translation versus the game's own asset loading is not measured (one fresh-session capture
of the first sword swing would tell). The handoff's item 3 (async compile with an interpreter
fallback) is the larger version of the same idea.

## Decision state

As of September 11, 2026 the user has not chosen between continuing item 2 or stopping; item 1
was recommended as the cheapest next measurement. Nothing in this roadmap is pushed to the fork
beyond 0db0ff6. Commits 5df1f8c and earlier on `main` hold stage 1 and its diagnostics.

## Measurement protocol, corrections

Same as [reaching-the-nexus.md](reaching-the-nexus.md) and the design document, with what the
stage 1 runs taught:

- Warm up at least 4 minutes on a build's first run, not 100 s: the driver's cold pipeline
  compiles (up to 1.5 s each, `PipelineCache::CreatePipeline(Compute)`) storm for minutes and
  the driver cache is only written periodically. Measure only after the console reports
  `0 new pipelines` on a save.
- Record the session type. Remote Desktop caps presentation near 30 FPS and its samples are only
  comparable with other Remote Desktop samples; anything at or above 30 FPS must be re-measured
  on the console session.
- One measurement at a time on this machine. The stats collector (`KYTY_DEBUG_SRT_STATS=1`)
  costs render-thread time; never leave it on in an FPS sample.
- Tracy: `_Build/profiling-tools/capture/tracy-capture.exe -a 127.0.0.1 -p 8086 -s 15 -f -o
  <file>`, then `csvexport/tracy-csvexport.exe -e <file> > <csv>` (the CSV is UTF-8 with a BOM;
  PowerShell `>` writes UTF-16 unless redirected through `Out-File -Encoding utf8`). There are
  no GPU timestamp zones yet; adding them is the first thing item 1 needs.
- Artifacts of the stage 1 runs: `_Runtime/_Diagnostics/gpufetch/{base,real3}-steady-clean.json`,
  `real3.tracy`, `real3-self.csv`, `real3.png`; stats dump
  `_Runtime/_Diagnostics/srt-stats-20260911-092544.json`.
