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
| gpuResourceManager.cpp | 10.7 | 10% | BDA dirty-page sync (`SynchronizeBdaBuffers`), new with stage 1 |
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
`_Runtime/_Diagnostics/replay/nexus-6` (format version 5, frame 5180, 6.5 GiB, 40 submissions,
captured after the protocol's five-minute warm-up).

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
count. The parked Nexus has **no buffer churn at all** — zero registrations, retirements, maps and
unmaps in each of six consecutive captured frames — so the residue was never a missing source of
BDA-generation bumps. The capture now records the game's own scan timeline and cost
(`prepare-events.bin`: every `PrepareBda` call, whether it scanned and for how long), the progress
clock ticks twice per draw and dispatch, the marks are applied inline on the GPU thread, and the
recorded dirty page set is marked once at restore rather than every frame. On a capture taken after
the protocol's warm-up the replay makes **9845 preparations against 9845 recorded and 462 scans
against 474**, with 0 late marks, on 2.9 dirty ranges a scan against 3.2.

The end-to-end side was re-baselined the same day on this build (`e2e-rebaseline/`, Remote Desktop,
five-minute warm-up, 30 s sample plus a 15 s Tracy capture): **11.63 FPS with `--gpu-descriptors
false` and 9.67 with `true`, ratio 1.202**, with `SynchronizeBdaBuffers` at **399 calls and 10.68 ms
a frame** (26.8 us a scan) against 21.2 calls and 3.98 ms with the flag off. The replay reaches
**1.068** of the 1.202, so the A/B is close but still outside the 10% window, and the residue is one
number: a replayed scan costs **4.1 us where the game's costs 25.8 us** on the same ranges and bytes,
because in a replay the buffers are resident and no guest thread competes for the upload path. Items
1, 3 and 4 are safe to measure in replay; item 2's BDA half is not, and needs the end-to-end
protocol. Evidence in [frame-replay.md](frame-replay.md), "Phase E".

One trap this found, which applies to every capture: **a frame captured before the warm-up is a cold
frame.** The same scene captured 40 s after navigating records 160 scans and captured five minutes
later records 474, because while the driver is still compiling the guest finishes its writes early
in the frame instead of spreading them through it.

## Items, in recommended order

Expected gains are projections from the self-time shares above unless a measurement is cited.
Measure each item before starting the next; the protocol is at the bottom.

### 1. Indirect draws and dispatches consumed by the GPU

Status, September 12, 2026: **the dispatch half is done and measured; the draw half is written,
measured neutral, and parked behind its own flag.** Two settings, both default off
([settings.md](settings.md)):

| Config on nexus-6, 60 loops, 3 repeats | ms/loop gpu | ms/loop | drains a loop | GPU-range syncs |
| --- | --- | --- | --- | --- |
| `--gpu-indirect false` | **71.80** | 73.74 | 14 | 10 248 |
| `--gpu-indirect true` | **68.57** | 70.35 | 3 | 9 961 |
| `--gpu-indirect true --gpu-indirect-draws true` | **71.77** | 73.66 | 0 | 986 |

Medians of the per-run medians, loop 1 excluded; reports in
`_Runtime/_Diagnostics/replay/nexus-6/runs-20260912-003652/`. The presented image is the same in
all three: mean absolute difference against `reference.png` R=9.8 G=7.8 B=4.1, and the pixel
difference between two configurations (5.9k to 24k pixels of 8.3M, maximum channel delta 26) sits
inside the run-to-run band two identical flag-off runs already show (22.5k, maximum 26). No `EXIT`,
no warning beyond the pre-existing wave64 line.

**The dispatch half pays.** `CpOpDispatchIndirect` and `CommandProcessor::DispatchIndirect` hand
the guest argument address to `RenderExecutor::DispatchDirect`, which already recorded
`vkCmdDispatchIndirect`; the `DISPATCH_INITIATOR` thread-dimensions form keeps the readback, since
the host divides those counts by the shader's group size. **3.2 ms a loop, 4.5%.** On title-2 the
steady state goes from 11.3 to 10.0 ms and the drains from 6 to 0.

**The draw half does not, in replay.** `CommandProcessor::DrawIndirect` passes the address through
and `RenderExecutor` records one `vkCmdDrawIndexedIndirect`/`vkCmdDrawIndirect`, binding the index
buffer over its whole `INDEX_BUFFER_SIZE` range so the device's `firstIndex` indexes into it. That
removes the last 9 262 argument syncs and the last 3 drains and gives **3.2 ms straight back**: it
trades one per-draw buffer-cache walk (`SyncGpuCleanBacking`) for another (`ObtainBuffer` of the
argument block, 8 794 times a loop) and widens the per-draw index binding from the draw's slice to
the whole declared buffer. Hence the separate flag. The case for finishing it is not visible here:
in replay the draw-argument syncs never drain (14 drains a loop for 245 dispatch syncs and 8 794
draw syncs), so the replay cannot show the thing step 3 exists to prevent — the game's draw syncs
draining once the dispatch download no longer cleans the pages for them. Settling that needs the
end-to-end protocol.

What stays on the readback path, decided in `CommandProcessor::IndirectDrawUsesGpuArgs`
(src/graphics/guest_gpu/graphicsRun.cpp): `DRAW_INDIRECT_MULTI` in both forms (zero calls in
`real3-self.csv`, and the count form needs `drawIndirectCount` and `multiDrawIndirect`, neither
enabled at device creation); `kQuadListLegacy` and `kRectListLegacy`, which fan out or substitute
the vertex count per index on the CPU; 8-bit indices, widened to 16 index by index; NGG/mesh
assembly (`SHADER_STAGES` bit 5), where the index count becomes a mesh workgroup count; a custom
primitive-reset index, which makes the host scan the index buffer before it can pick the pipeline;
and a device without `VkPhysicalDeviceFeatures::drawIndirectFirstInstance`, which is now requested
when the device has it because a guest argument block may carry a non-zero
`start_instance_location`. One deviation to know about: on the native path `m_num_instances` is not
updated, because the CPU never learns the draw's instance count, so a later short-form draw that
leaves `instance_count` at 0 uses the last `IT_NUM_INSTANCES` packet's value instead of the
previous indirect draw's.

Where the barrier is: a per-draw `eIndirectCommandRead` buffer barrier cannot be recorded, because
`vkCmdPipelineBarrier` inside a dynamic-rendering pass may only name framebuffer-space stages and
`eDrawIndirect` is not one, and ending the pass per draw would cost more than the change saves.
The global `ShaderAccessBarrier` every dispatch already emits covers it (`eShaderWrite` to
`eMemoryRead` over `eAllCommands`); `MakeShaderWriteDependency` gained `eIndirectCommandRead` so a
graphics shader that writes another draw's arguments is covered too.

Diagnostics added: `SyncGpuCleanBacking` counts the ranges it actually downloads and the GPU-range
calls it makes, and the replay report prints both per loop (`drains N a loop of M GPU-range syncs`,
`drains_per_loop` / `syncs_per_loop` / `frame_drains` in `replay-report.json`, two more columns in
`replay-des.ps1`). That is what corrected the handoff's number: the parked Nexus makes 245
`CpOpDispatchIndirect::SyncArguments` calls a loop but only **14** of them download anything, so
the zone's 3.65 ms is 0.26 ms in each of fourteen drains, not 15 us in each of 245 syncs.

Not measured: Vulkan validation. `VK_LAYER_KHRONOS_validation` is not installed on this machine
(`no validation layer: VK_LAYER_KHRONOS_validation` with `--printf-direction Console`), so
`--vulkan-validation true` silently runs without it. A validation pass is still owed.

### 2. Finish stage 1 of GPU-side descriptor fetch

Status: stage 1 landed behind `--gpu-descriptors` (default off), measured slower, causes known.
[gpu-descriptor-fetch.md](gpu-descriptor-fetch.md), "Stage 1 measured" and its re-baseline of
September 11 (11.63 FPS off against 9.67 on, ratio 1.202).

Two pieces, both foreseeable from the stats dump:

- Move the flattened SRT scalar reads (`srt_reads`, the `flat_reads` roots) in-shader with the
  same lowering as the descriptor roots. Then `EvaluateRuntimeSourcesImpl` no longer walks the
  chain for fetched programs. Removes most of the 28 ms.
- Make the per-draw dirty sync cheap: track dirtied pages as a list instead of scanning dirty
  ranges against mapped ranges on every generation change. Re-measured on the build of
  September 11: **399 scans a frame at 26.8 µs, 10.68 ms** (`PrepareBda` runs 9845 times a frame and
  4.8% of those find a changed generation; with the flag off it is 21.2 scans and 3.98 ms). A scan
  walks only 3.2 dirty ranges and 34 KiB, so the cost is not the scan's inputs: the same scan in a
  replay, with the same ranges, costs 4.1 µs, and putting twelve spinning threads on the guest CPUs
  does not close that gap (4.8 µs), so it is not simply contention from running threads either
  ([frame-replay.md](frame-replay.md), phase E). A scan makes about two page re-protections in
  replay; what the game's scan makes is not recorded yet and is the measurement that would settle
  where the 26 µs goes. Removes most of the 10.7 ms.

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

As of September 12, 2026 item 1's dispatch half is done and worth 3.2 ms of the parked Nexus's
71.8 ms loop, its draw half is written and parked behind `--gpu-indirect-draws` because it gives
that back in replay, and the choice between continuing item 2 or stopping is still open. Two things
item 1 leaves owed: an end-to-end run, which is the only way to see whether the draw half earns its
place once the dispatch download no longer cleans the argument pages, and a Vulkan validation pass,
which this machine cannot do because `VK_LAYER_KHRONOS_validation` is not installed. Nothing in
this roadmap is pushed to the fork beyond 0db0ff6. Commits 5df1f8c and earlier on `main` hold
stage 1 and its diagnostics.

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
