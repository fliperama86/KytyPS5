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
| SrtWalker.cpp | 28.1 | 26% | SRT evaluation on the CPU (`EvaluateRuntimeSourcesImpl`). Almost all of it is a tail: 0.53% of the calls carry 79.8% of the zone, and every one of them is a read fault on a GPU-written page that drains the device (item 1b below). The 1.2 us an event that "Packed flat program" in [gpu-descriptor-fetch.md](gpu-descriptor-fetch.md) measured is that tail averaged over every call, not a per-event memory latency |
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
`start_instance_location`. Two deviations to know about. `m_num_instances` is not updated on the
native path, because the CPU never learns the draw's instance count, so a later short-form draw
that leaves `instance_count` at 0 uses the last `IT_NUM_INSTANCES` packet's value instead of the
previous indirect draw's. And a degenerate indirect draw is no longer dropped: `DrawIndex` returns
early on a zero index or instance count, which the CPU can no longer see, so those draws now bind
their resources and record a command the device retires as a no-op. The replay counts them --
`progress 22382 of 22368` against `22368 of 22368` with the flag off -- so the parked Nexus has
**14 of them a loop**, out of 8 794 indirect draws.

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

### Item 1b: GPU readbacks inside the SRT evaluation

Status, September 12, 2026: **characterised, fixed the only way the fix could work, measured
neutral, and parked behind `--gpu-readback-producer-wait` (default off); periodic submits added
the same day, worth 3.2 ms of 73.7 and parked behind `--gpu-submit-interval` (default 0).** The cost is real and
large -- 22 to 27 ms of the parked Nexus's 73 ms replay loop -- but it is the device executing the
draw the render thread has just recorded, not a wait the readback's shape can shorten.

**What the finding is.** `EvaluateRuntimeSourcesImpl` has a mean of 1.48 us a call and a median of
321 ns. Its time is a tail, not a rate. In the game capture
`_Runtime/_Diagnostics/replay/e2e-rebaseline/scan-breakdown/gd-true.tracy` (147 frames, 20 831
calls a frame, 30.9 ms a frame):

| per-call time | calls a frame | share of the calls | ms a frame | share of the zone |
| --- | --- | --- | --- | --- |
| over 5 us | 110.5 | 0.53% | 24.6 | **79.8%** |
| over 100 us | 33.6 | 0.16% | 19.6 | 63.6% |
| over 1 ms | 5.6 | 0.03% | 12.8 | 41.6% |

The maximum is 11.5 ms. **100% of the calls over 100 us contain a `PageManager::ProtectCall` on the
render thread, and that protect is 0.35% of their time** -- the protect is the marker, not the
cost. Replay reproduces it: in `nexus-6/tracy-prefetch/pf-base-a.tracy` (29 steady-state loops)
`SrtEval::Execute` is 23.4 ms a loop, 94.5% of it in the 87 calls a loop over 5 us, 36 over 100 us,
5.6 over 1 ms; 100% of those over 100 us contain a render-thread protect at 0.3% of their time.

So the evaluator is not slow. About ninety times a loop it reads a guest page the GPU wrote, the
read faults (the page is no-access while GPU-dirty), and `GpuResourceManager::HandleFault`
(gpuResourceManager.cpp) calls `BufferCache::ReadMemory` -> `ReadMemoryOnGpu` ->
`DownloadBufferMemory` (bufferCache.cpp), which records the copy on the current command buffer,
`Finish()`es it and waits: a full CPU-to-GPU drain. It is the mechanism item 1 removed for indirect
arguments, at 90 a loop instead of 14.

#### The characterisation

`--gpu-readback-diagnostics` ([settings.md](settings.md)) counts and times every read fault the
render thread takes, and records with each one the shader hash, stage and root kind the evaluator
was on, the faulting address, the bytes the 512 KiB download window covers, the submission tick and
the draw-and-dispatch clock (`GuestGpu::Progress`) that produced the page, whether that submission
had retired (`CommandScheduler::IsFree`), and the wait. It writes `readback-faults.json` beside
`replay-report.json`. nexus-6, 30 loops, `--gpu-descriptors false`, loop 1 excluded (29 loops,
2 809 faults):

| | |
| --- | --- |
| read faults a loop | **96.9**, all on the render thread |
| time in `ReadMemory` for them | **26.9 ms a loop**, of which **26.2 ms** is the drain |
| bytes downloaded | 15.0 MiB a loop; 155 KiB mean a fault, 293 KiB median |
| distinct pages faulted | **26** (85 distinct faulting addresses) |
| distinct producer submissions | 2 436 of 2 809 faults |

**Were the producers complete?** No, mostly not:

| producer at the moment of the read | faults a loop | ms a loop | mean | median |
| --- | --- | --- | --- | --- |
| already retired | 28.6 (29.5%) | 11.63 | 406 us | 136 us |
| still pending | 68.2 (70.5%) | 15.31 | 224 us | 86 us |
| unknown | 0 | | | |

**1 972 of the 1 979 pending producers have `producer_tick == CurrentTick()`**: the draw or dispatch
that wrote the page is still in the *open, unsubmitted* command buffer. The tick gap between
producer and read is 0 for 70% of faults (p90 117, max 213), and the draw-and-dispatch clock gap is
**1 for every decile up to the 70th** (p80 23, p90 341, max 18 903) -- one half-tick, the producer
is the draw the render thread has just finished recording. The CPU dispatches a shader and reads
what it wrote a moment later.

That is the whole answer to "how much of the queue is between the producer and the read": nothing
is *after* the producer, and the entire open command buffer -- about 230 draws, one loop's 22 368
draws divided by its 97 faults -- is *before* it. A readback that waits for its producer waits for
all of that, because a command buffer cannot signal in the middle.

**Which shaders and which roots.** By root kind, per loop:

| root kind | faults a loop | ms a loop | producers pending |
| --- | --- | --- | --- |
| flat read (`srt_reads`) | 79.9 | 15.73 | 1 885 of 2 316 |
| descriptor source | 12.0 | 10.30 | 94 of 348 |
| outside any evaluation | 5.0 | 0.91 | 0 of 145 |

Top shaders by time, per loop:

| shader | stage | faults a loop | ms a loop | producers pending |
| --- | --- | --- | --- | --- |
| `0x74e4490fe7af0970` | cs | 48.0 | 5.34 | 1 392 of 1 392 |
| `0xdc9c7811548c9754` | vs | 7.0 | 3.49 | 94 of 203 |
| `0x560c7a298b6a497b` | ps | 1.0 | 2.84 | 29 of 29 |
| `0xd8d85e14931ab3d4` | cs | 1.0 | 2.33 | 29 of 29 |
| `0xd669aa446e6b6137` | vs | 2.0 | 2.01 | 0 of 58 |
| `0xebfe7348a8127d63` | cs | 1.0 | 1.38 | 0 of 29 |
| `0x58d0dca35178894d` | vs | 1.0 | 1.29 | 0 of 29 |
| `0xfbcf030a4c3f0725` | cs | 0.97 | 0.99 | 0 of 28 |
| `0x5979d5395f497370` | cs | 1.0 | 0.96 | 0 of 29 |
| (no evaluation) | - | 5.0 | 0.91 | 0 of 145 |

One compute shader accounts for half the faults, always on a producer it has just dispatched, and
is cheap each time (111 us); the expensive faults are the rarer ones whose window is a whole
512 KiB.

#### The fix, and why it does not pay

`--gpu-readback-producer-wait` records the download copy on a **separate command buffer**, submitted
on the graphics queue waiting on the master timeline at the producer's tick alone and signalling a
**separate timeline semaphore** of its own (`CommandScheduler::BeginReadback` /
`SubmitReadbackAndWait`), and the fault handler waits on that submission. The open command buffer
is left open and unsubmitted, so the render thread stops waiting for the work it recorded since the
last drain.

The separate timeline matters: the master timeline's value is what recycles pooled command buffers,
so a readback that signalled it out of order would let `CommandPool::Commit` hand out a buffer that
is still recording or still running.

Two hazards, and what covers them:

- **The producer's writes must be visible to the copy.** The timeline wait on the producer's tick
  is the dependency, and the copy's own `vkCmdPipelineBarrier` pair (`Buffer::CopyFrom`, now also
  available on a bare command buffer) is the memory dependency.
- **Later recorded work must not overwrite the source before the copy runs.** This is the case the
  timeline wait does not cover, and in this capture it cannot arise. A range becomes GPU-owned in
  exactly one place, `ObtainBuffer` with `is_written`, which moves the range's producer tick; so if
  anything recorded after the producer could write these bytes, the producer tick would be that
  later submission's and the gate would refuse. The render thread is the only submitter and it
  blocks on the readback, so nothing can be handed to the queue between the copy's submission and
  its completion. The path is refused outright unless the newest submission that wrote **every**
  byte being copied is known and has already retired.

Knowing the producer needs a map. The buffer cache keeps a direct-mapped table of 16 384 entries,
one per 64 KiB block, written wherever `ObtainBuffer` claims a range for the GPU, plus one scalar
for a write too wide to record block by block (over 512 MiB; none occur here). A block with no entry
or an unrecorded wide write newer than the answer refuses the fast path -- a miss costs the fast
path, never correctness. The table exists only while one of the two settings is on. A first version
used a single "newest GPU write anywhere" scalar instead and was refused on 663 of the 830 eligible
faults, which is what the refusal counters in the report are for.

Measured, nexus-6, 40 loops, two repeats, `-Image`, medians of the per-run medians with loop 1
excluded (`nexus-6/runs-rb-ab/`, images from `nexus-6/runs-rb-image/`):

| `ms/loop gpu` | producer wait off | producer wait on |
| --- | --- | --- |
| `--gpu-descriptors false`, min | **71.87 / 71.51** | 71.59 / 71.87 |
| `--gpu-descriptors false`, median | 74.00 / 73.62 | 74.20 / 73.86 |
| `--gpu-descriptors true`, min | **75.30** | 75.51 / 75.52 |
| `--gpu-descriptors true`, median | 78.38 | 78.41 / 79.01 |
| image vs `reference.png` | R 9.8 G 7.8 B 4.1 | R 9.8 G 7.8 B 4.1 |

**Neutral to 0.3 ms worse**, and the diagnostics say exactly why (`nexus-6/readback/`, 30 loops
each):

| nexus-6, 29 loops | off | on |
| --- | --- | --- |
| read faults a loop | 96.9 | 96.9 |
| faults on a retired producer | 28.6 a loop, **11.63 ms** | 24.9 a loop, **3.26 ms** |
| faults on a pending producer | 68.2 a loop, 15.31 ms | 72.0 a loop, **18.03 ms** |
| own-submission path taken | 0 | 721 of 2 809 |

The fast path does what it was built to do: the faults it takes cost 131 us instead of 406, **6.6 ms
a loop less**. The loop does not move because that time comes straight back on the other side. Not
draining leaves the open command buffer longer, and the next fault whose producer sits in it drains
that longer buffer. The device's work per loop is unchanged, and the render thread is serialised
against it either way -- about 26 ms of a 73 ms loop is spent waiting for a device that the CPU
never lets run ahead.

title-2, 40 loops, two repeats (`title-2/runs-rb/`): `ms/loop gpu` median 254.70 / 247.38 off
against 248.00 / 247.49 on, minimum 11.30 / 11.92 against 11.17 / 11.40. Neutral there too.

#### What this rules out, and what is left

The hard stop in this item's brief fired, in a sharper form than it was written. The producers are
pending for 70% of the faults, and it is not that most of the queue sits between the producer and
the read -- nothing does. The producer *is* the last thing recorded, and the whole open command
buffer sits before it. The wait is the device executing that buffer, and no arrangement of the
readback's own submission can remove it.

Three things this leaves, in order of how much they would be worth:

- **Do not read it on the CPU at all.** The bytes being faulted on are SRT scalar reads and
  descriptor sources -- 79.9 of the 96.9 faults a loop are flat reads. A shader that fetches its own
  descriptors and its own flat slots never asks the CPU for them. That is stage 1b's and stage 3's
  case, and this measurement is the first one that prices it in device time rather than in
  evaluator time: 26 ms of a 73 ms loop, against the 0.8% stage 1b moved of the evaluator's own
  zone. Note that stage 1b is *not* enough on its own -- `--gpu-descriptors true` still takes the
  same 97 faults a loop, because the host keeps evaluating a program's remaining roots.
- **Let the device run ahead.** The 26 ms is the device's own work, done in ninety synchronous
  instalments. Submitting the open command buffer periodically instead of only at a drain would put
  that work in flight while the render thread records, so a fault would wait for a small tail
  instead of 230 draws. It does nothing for the median fault, whose producer is the immediately
  preceding dispatch, but it is the only lever that touches the 70%. **Built and measured; see
  *Periodic submits* below. Worth 3.2 ms a loop, below this item's 5 ms hard stop, parked behind
  `--gpu-submit-interval`.**
- **Coalesce the faults.** 96.9 faults a loop land on **26 distinct pages**, and the download
  already widens to a 512 KiB window. A page read three or four times a loop is being re-dirtied
  between reads, so widening further would not help; batching the reads a single evaluation makes
  might.

Artifacts: `_Runtime/_Diagnostics/replay/nexus-6/readback/` (the two characterisation runs and their
`readback-faults-{off,on}.json`), `.../runs-rb-ab/` (the 40-loop bench), `.../runs-rb-image/` (the
images and the `--gpu-descriptors true` pair), `_Runtime/_Diagnostics/replay/title-2/runs-rb/`.

#### Periodic submits

September 12, 2026: **built, measured, worth 3.2 ms a loop of 73.7, and parked behind
`--gpu-submit-interval` (default 0).** The brief's hard stop was 5 ms; it fired.

This is the second of the three things the characterisation left: let the device run ahead. Today
the open command buffer is handed to the queue only when something drains it or at a guest
submission boundary -- 197 `vkQueueSubmit`s a loop for 22 368 draws -- so between drains the device
idles while the render thread records, and at each of the 97 read faults the render thread waits
for everything it has recorded since the last one. `--gpu-submit-interval N` counts the draws and
dispatches the render thread records and, every N of them, ends the open command buffer and
submits it **without a wait** (`CommandScheduler::NoteDrawRecorded` -> `Flush`), called from
`CommandProcessor::DrawIndex`, `DrawIndexAuto` and `DispatchDirect` after the executor has
recorded the command. `--gpu-submit-after-writes` submits after any draw or dispatch that claimed
a range for the GPU (`ObtainBuffer` with `is_written`) as well.

**The one condition that decides whether it pays: only outside a dynamic-rendering pass.** The
first version submitted wherever the counter said so, ending the open pass as `CommandBuffer::End`
does anyway. That measured **7.6 ms a loop worse** at N=32 (81.1 against 73.5) even though it cut
the fault wait by 5 ms: cutting a pass in half makes the device store every attachment and reload
it in the next buffer, and a pass whose load operations clear would clear a second time, because
two consecutive draws that share a render state re-enter `BeginRendering` as a no-op today.
Restricting the submit to boundaries where no pass is open turned the same flag from -7.6 ms to
+3.0 ms, and removes the clear hazard outright: outside a pass the next draw calls
`BeginRendering` either way. A dispatch ends the pass before it records, so the producers this
exists for -- the compute passes the SRT evaluation reads back from -- are exactly the boundaries
that are free.

nexus-6, `--gpu-descriptors false`, 40 loops, two repeats, both runs shown, loop 1 excluded
(`_Runtime/_Diagnostics/replay/nexus-6/runs-submit/`):

| `--gpu-submit-interval` | `ms/loop gpu` min | `ms/loop gpu` median | `ms/loop` median | submits a loop | read faults a loop | wait ms a loop |
| --- | --- | --- | --- | --- | --- | --- |
| 0 (baseline) | 71.94 / 71.72 | 73.91 / 73.57 | 75.16 / 74.69 | 197 | 96 | 38.35 / 38.11 |
| 8 | 69.31 / 68.85 | 71.35 / 70.49 | 73.36 / 72.40 | 423 | 96 | 29.69 / 30.75 |
| **16** | **68.78 / 68.24** | **70.79 / 70.41** | **72.40 / 71.69** | 318 | 96 | 31.60 / 32.08 |
| 32 | 69.44 / 69.13 | 71.22 / 70.78 | 73.29 / 72.39 | 264 | 96 | 32.83 / 33.33 |
| 64 | 69.08 / 69.38 | 70.81 / 71.30 | 72.17 / 72.79 | 237 | 96 | 33.86 / 33.41 |
| 128 | 70.21 / 70.51 | 72.31 / 72.18 | 73.91 / 73.24 | 219 | 96 | 34.59 / 35.43 |
| 32, `--gpu-submit-after-writes true` | 93.91 / 93.66 | 95.72 / 95.23 | 98.24 / 98.09 | 1 738 | 96 | 23.15 / 21.29 |

The presented image is the same in every configuration: mean absolute difference against
`reference.png` R=9.8 G=7.8 B=4.1, and the pixel difference against the flag-off image (28k to 55k
pixels of 8.3M, maximum channel delta 31 to 37) sits inside the band two runs of the same
configuration already show on this build (59k, maximum 32). Images and their runs in
`.../runs-submit-image/` and `.../runs-submit-band/`. No `EXIT`, no stderr output at all in any of
the 14 runs.

**What the table says.** A submission costs about 20 us, and the wait it buys back saturates. The
first 120 extra submissions a loop (0 -> 16) return 6 ms of fault wait for 2.4 ms of submission
cost. The next 1 500 (16 -> after-writes, and an `--gpu-submit-interval 1` probe at 1 916
submissions a loop and 17.4 ms of wait) return 9 more milliseconds of wait for 30 of submission.
That is the whole shape: the 26 to 38 ms of device wait is the device's own work, and the most a
submission schedule can do is overlap it with the recording, not remove it. The floor the probes
reach -- about 17 ms a loop of wait -- is the producers themselves, which is exactly what the
characterisation predicted for the 70% whose producer is the immediately preceding dispatch.

`--gpu-descriptors true`, same capture and protocol (`.../runs-submit-gd/`):

| | `ms/loop gpu` min | `ms/loop gpu` median | `ms/loop` median | submits a loop | wait ms a loop |
| --- | --- | --- | --- | --- | --- |
| interval 0 | 75.57 / 75.98 | 79.80 / 79.55 | 81.09 / 80.26 | 197 | 44.04 / 43.34 |
| interval 16 | 72.03 / 71.65 | 76.91 / 74.70 | 77.95 / 77.02 | 318 | 36.06 / 39.22 |

title-2, 40 loops, two repeats (`_Runtime/_Diagnostics/replay/title-2/runs-submit/`): **neutral.**
`ms/loop gpu` median 246.87 / 246.31 at interval 0 against 246.87 / 250.36 at 16, minimum 11.24 /
11.27 against 10.76 / 10.57, `ms/loop` median 252.74 / 250.14 against 252.85 / 255.90; 62
submissions a loop against 78. That capture takes 6 read faults a loop and 232 ms of wait in them,
so its drains are not mid-frame and there is nothing for an interval to overlap.

**Recommendation: keep the default at 0.** The win is real, reproducible and image-clean on the
parked Nexus in both descriptor modes, but it is 4.3% rather than the 14% the hypothesis priced,
it is nothing on the other capture, and it changes the shape of every submission the emulator
makes for every title. It is worth turning on for Demon's Souls once an end-to-end run confirms
the replay number, and worth revisiting if item 2 or item 3 removes enough of the CPU side that
the 32 ms of remaining fault wait becomes the frame.

Diagnostics: the replay report now prints `submits N a loop (vkQueueSubmit, graphics timeline);
read faults N a loop, N.NN ms of device wait in them` under the drains line, and
`submits_per_loop`, `read_faults_per_loop`, `read_fault_wait_ms_per_loop`, `gpu_submit_interval`
and `gpu_submit_after_writes` in `replay-report.json`. The fault count and the wait are counted in
every build now, not only under `--gpu-readback-diagnostics`: one relaxed add and one
`steady_clock` pair per fault, about a hundred a loop.

Artifacts: `_Runtime/_Diagnostics/replay/nexus-6/runs-submit/` (the seven-configuration bench),
`.../runs-submit-image/` and `.../runs-submit-band/` (images and the run-to-run band),
`.../runs-submit-gd/` (`--gpu-descriptors true`), `.../smoke-si/` and `.../smoke-si2/` (the
in-pass version and the pass-boundary version at the same interval),
`_Runtime/_Diagnostics/replay/title-2/runs-submit/`.

#### End-to-end, September 12, 2026

The A/B the three levers above were waiting for, run in the game: one launch with every new setting
off, one with `--gpu-indirect true --bda-async-protect true --gpu-submit-interval 16`, back to back
in the same session, same save, same parked Nexus, five-minute warm-up until the pipeline cache
stopped growing, 30 s sample with no profiler attached, closed through the window so the cache is
written. Build: the working tree that became `a759712` (the window title says `f15ab28-dirty`).

| run | FPS | ms a frame | GPU busy | GPU power | CPU cores |
| --- | --- | --- | --- | --- | --- |
| A, defaults | **3.93** | 254.4 | 17.6% | 83.9 W | 10.50 |
| B, `--gpu-indirect true --bda-async-protect true --gpu-submit-interval 16` | **3.93** | 254.5 | 15.8% | 82.6 W | 11.30 |

**No difference: 0.1 ms a frame of 254, B/A = 0.9995.** Replay predicted about 7.7 ms a frame in
B's favour -- 3.2 (indirect dispatch) + 1.5 (async protect, projected from the syscall count) + 3.2
(periodic submits) -- that is 86 ms down to 78. The flags were in effect: the console log lists the
design P helper thread (`affinity: derived -> Thread_BdaProtect`) in B and not in A.

**But the pair does not test that prediction, because the scene did not run at 86 ms.** It ran at
254 ms a frame, three times the 86 ms the same protocol measured on the build of September 11
([gpu-descriptor-fetch.md](gpu-descriptor-fetch.md), "Re-baselined on the build of September 11").
Three things say where that factor of three is not:

- **Not the binary's render path.** The same binary replays nexus-6 at **72.91 ms/loop gpu** median
  (73.95 ms/loop, 14 drains, 10 248 syncs), which is the 71.80 / 73.74 this document records for
  `--gpu-indirect false` on this build. Report in `.../day-2026-09-12/replay-check/`.
- **Not the window or the compositor.** Minimising the game window and restoring it changed
  nothing: 3.93 fps visible, 3.93 fps minimised, 3.93 fps restored, 118 frames in each 30 s.
- **Not the scene, the save or the warm-up.** The screenshots are the parked Nexus at the archstone
  in the framing of `scan-breakdown/gd-true-warm.png`, and both runs ended on a
  `pipeline cache: saved ... (0 new pipelines)`.

The one recorded difference from every previous game number is the **session type**: the September
11 re-baseline and the September 12 scan breakdown were taken over Remote Desktop, these two on the
local console session (`query session`: `console dudu 1 Active`). The protocol already says samples
from the two are not comparable ([reaching-the-nexus.md](reaching-the-nexus.md), "Measuring"), and
this is the first pair taken on the console. The boot differs the same way: the opening cinematic
ran at 4 fps and took about thirteen minutes of wall clock to reach the point the navigation skips
from, against about sixty seconds at 60 fps under Remote Desktop, so the navigation waits had to be
stretched. Per second the process burns the same CPU as the re-baseline (10.5 to 11.3 cores against
12.6) and less GPU (83 W against 123 W); it is the frames that are three times rarer.

So the three levers are **unmeasured end-to-end**, not refuted. What this run settles is that the
game on the console session is not the 86 ms frame the replay bench and every projection in this
document are calibrated against, and the pair has to be repeated over Remote Desktop -- or the
console/Remote Desktop gap itself explained -- before `--gpu-indirect`, `--bda-async-protect` and
`--gpu-submit-interval` can be turned on for Demon's Souls. (It was repeated the same day, see
"Repeated on the unlocked console" below: the gap was the lock screen, and the repeat does price
the three levers.)

Artifacts: `_Runtime/_Diagnostics/replay/e2e-rebaseline/day-2026-09-12/` --
`a-defaults.json` / `b-flags.json` (the samples), `*-console.log`, `*-phase.log` (the driven
protocol, timestamped), `*-after-nav.png` and `*-warm.png` (the scene), `replay-check/` (the
replay-bench sanity check). Scripts: `_Build/e2e-phase.ps1` drives one phase of a run against an
already-running emulator, which is how these two were paced; the detached launcher of
`_Build/scan-breakdown-run.ps1` does not survive the tool session that starts it.

#### Repeated on the unlocked console, September 12, 2026

The pair above was taken while the machine sat on its lock screen. Repeated a couple of hours later
with the machine unlocked at the desk, same binary, same save, same protocol, same session type
(`query session`: `console dudu 1 Active`, no Remote Desktop client attached), driven phase by
phase with `_Build/e2e-phase.ps1`.

**The factor of three was the lock screen, not the console session.** The opening cinematic ran at
55 to 60 fps and reached the skip point in about 90 seconds, against 4 fps and thirteen minutes on
the locked console; the parked Nexus then sampled at 11.73 fps, which is the 11.63 fps the
September 11 re-baseline recorded over Remote Desktop. A locked console still advances the frame
counter, so nothing in the protocol flagged it -- the frame rate itself is the only signal, and it
is worth checking against the baseline before the warm-up rather than after the sample.

| run | FPS | ms a frame | GPU busy | GPU power | CPU cores |
| --- | --- | --- | --- | --- | --- |
| A, defaults | **11.73** | 85.25 | 28.6% | 131.6 W | 12.60 |
| B, `--gpu-indirect true --bda-async-protect true --gpu-submit-interval 16` | **11.91** | 83.93 | 33.8% | 134.9 W | 12.66 |

**B wins 1.32 ms a frame, B/A = 1.016 on frame rate.** That is the right sign and about **17% of
the 7.7 ms replay predicted** (3.2 indirect dispatch + 1.5 async protect + 3.2 periodic submits,
86 ms down to 78). GPU busy rises 5 points and power 3 W in B with the frame only 1.3 ms shorter,
which is the shape periodic submits are meant to produce: the same work reaching the device
earlier and spread wider, rather than a shorter CPU frame.

The flags were in effect and the runs were otherwise identical: B's console log lists the design P
helper thread (`affinity: derived -> Thread_BdaProtect`) and A's does not, both runs warmed 300 s
to a settled pipeline cache and closed through the window on
`pipeline cache: saved ... (0 new pipelines)`, and the warm screenshots are the same parked Nexus
at the archstone with no image difference.

**This is one pair, not a band.** 1.32 ms is 1.6% of the frame, the replay bench's own run-to-run
band on this scene is of that order, and neither run was repeated, so the honest reading is that
the three levers together are worth something between nothing and about 3 ms a frame here -- far
short of the 7.7 ms projection, and not yet enough on its own to justify turning them on by
default for Demon's Souls. What it does settle is that the end-to-end protocol now reproduces the
86 ms baseline the replay bench is calibrated against, so the next A/B of these levers is a
repetition, not a re-derivation.

Artifacts: `_Runtime/_Diagnostics/replay/e2e-rebaseline/day-2026-09-12-b/` --
`a-defaults.json` / `b-flags.json` (the samples), `a-defaults.csv` / `b-flags.csv` and
`sample-lines.txt` (the per-second raw lines), `*-console.log`, `*-phase.log` (the driven protocol,
timestamped), `*-boot.png`, `*-after-nav.png` and `*-warm.png` (the scene). Scripts:
`_Build/e2e-launch.ps1` starts the emulator detached and prints the pid, `_Build/e2e-phase.ps1`
drives each phase against it.


### 2. Finish stage 1 of GPU-side descriptor fetch

Status, September 12, 2026: **both halves are written and measured, neither pays in replay, and
the packed evaluator that followed says why.** The evaluator's per-event cost is the guest SRT
pages, not the plan: a packed sequential form of the flat program cuts the synthetic cold cost by
60% and the replay loop by 2 ms, and the same instrumentation prices the plan at 68 ns an event
against 1 205 ns of first touch on guest memory
([gpu-descriptor-fetch.md](gpu-descriptor-fetch.md), "Packed flat program"). Skipping roots removes
reads, not cache lines, which is why stages 1 and 1b moved the zone by less than 1%.
Stage 1 landed behind `--gpu-descriptors` (default off), measured slower, causes known
([gpu-descriptor-fetch.md](gpu-descriptor-fetch.md), "Stage 1 measured" and its re-baseline of
September 11: 11.63 FPS off against 9.67 on, ratio 1.202). Stage 1b, the flat SRT reads in-shader,
landed behind `--gpu-srt-reads` (default off) and is measured in the same document, "Stage 1b
measured": 128 programs and 6,926 of 7,017 slots lowered, no feedback mismatch, no pinned program,
the image unchanged, and **`EvaluateRuntimeSourcesImpl` down 0.8%**, not the 30%+ the projection
below assumed. The sync half is design P behind `--bda-async-protect`, also default off and also
unpriced by replay.

**What the stage 1b measurement changed about this item.** The 28 ms of `SrtWalker.cpp` is not the
SRT walk: `gd=false` evaluates every descriptor source and every flat slot at 1.44 us an event,
`gd=true` skips 6.9 of the 8.5 descriptor dwords and costs 1.58, and adding the slot skip (17.8 of
21.5 slots gone) leaves it at 1.57. `srt_evaluator_bench` prices the whole evaluation of a
Nexus-shaped plan at 228 ns, of which the slot skip is worth 75. Split by zone on September 12,
the fixed per-call work is 149 ns of the 1 500 and the rest is the cache misses the flat program
takes on its own plan -- 40 to 60 lines of it an event, across 493 plans cycled a loop -- so the
same evaluation runs in 188 ns out of L1 and 1 159 ns out of DRAM
([gpu-descriptor-fetch.md](gpu-descriptor-fetch.md), "Where the evaluator's 1.5 us goes").
Either way the cost does not follow the roots out of the host. The plan-cold reading of that
measurement was tested on September 12 by packing the flat program into one sequential 16-byte-an
-instruction schedule: the bench improved 60%, the replay loop 2 ms, and splitting the packed run
into a pass that touches only the plan and a pass that reads guest memory priced them at **68 ns
and 1 205 ns** an event. So the levers left are calling it less often (once a program a frame
instead of once a stage a draw, which also makes the guest lines warm across the program's draws)
or removing the last host consumer of the walk, which is stage 3.

Two pieces, both foreseeable from the stats dump:

- Move the flattened SRT scalar reads (`srt_reads`, the `flat_reads` roots) in-shader with the
  same lowering as the descriptor roots. **Done, and it removes none of the 28 ms** -- see the
  status above and "Stage 1b measured". It removes 0.5 ms a loop of `RebindBuffers` and
  `FindBuffers`, because a program with every slot lowered loses the `FlattenedSrt` binding and the
  upload that fills it.
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

The sync half, measured on September 12: the scan is 71% `NtProtectVirtualMemory`, the page
re-protection after each upload, a TLB shootdown across the guest's threads (5.4 us a call, 3.7
calls a scan); the copy is 4%. Design and plan: [bda-sync-design.md](bda-sync-design.md), design P
(re-protection off the render thread).

Design P is implemented behind `--bda-async-protect` (default off) and measured in replay
([gpu-descriptor-fetch.md](gpu-descriptor-fetch.md), "Design P measured, September 12, 2026"): the
render thread's upload path makes 0 protect calls with it on against 926 a loop off, the image is
unchanged in both `--gpu-descriptors` settings, and the projected render-thread saving in the game
is about 5 ms of a 103 ms frame -- 10.3 ms of syscall latency moved off the render thread, 5.2 ms
of second uploads and boundary scans paid back. The replay cannot see the saving, only the price,
so the end-to-end A/B is what settles it and it has not been run yet.

Expected: the design document's projection was about 16 FPS in this scene. It assumed the 26% of
the render thread in `EvaluateRuntimeSourcesImpl` follows the descriptor and read roots out of the
host; the measurement above says it does not, so what is left of this item is the sync half's
projected 5 ms a frame, and the per-event cost itself belongs to item 3.

Decision state, September 12 evening: item 2 as "finish stage 1" is closed (both halves landed
behind flags, neither pays); the frame is the 90 sync points of item 1b, and the path that removes
them without artifacts is [sync-points-design.md](sync-points-design.md), "After the bench", built on
the host-memory bench (docs/investigations/bda-host-memory-bench-2026-09-12.md). Step 1 of that path, the prologue
page table over imported guest memory, is landed behind `--gpu-prologue-table` and measured:
neither page table misses any more, in loop 1 as well as in the steady state, the image is
unchanged and the bookkeeping is free -- and it costs 2.4 ms a loop against the 1 ms it was
allowed, all of it in the prologue page-table lookup every root and flattened read makes, so it
stays off and the path waits on that decision
([sync-points-design.md](sync-points-design.md), "Step 1"). Step 2, the side-effect rule lifted for
compute programs behind `--gpu-fetch-side-effects`, is landed and measured the same evening: it
takes the read faults from 89 a loop to **22** and removes item 1b's flat-read faults entirely,
which is what it was for, and its hard stop fired -- the loop goes from 78.3 to 97.3 ms because
every wave of a marked dispatch re-evaluates up to 287 SRT chains, its safety net fires 160 to
2,090 times a loop instead of never, and one 40-loop run in two died on a texture descriptor a
producer left behind. It stays off. The remaining 22 faults a loop are image and sampler roots of
pixel and vertex shaders plus 6 outside any evaluation, which is the number stage 3 should be
sized from ([sync-points-design.md](sync-points-design.md), "Step 2").

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

Step 2 of the sync-points path is landed and parked the same evening (above, item 2's status):
the read faults fall from 89 a loop to 22 and item 1b's flat-read faults go to zero, and the hard
stop fired anyway -- 19 ms a loop slower, a safety net that fires thousands of times a loop, and a
producer-written descriptor that killed one run in two. What the measurement leaves is a sized
target for stage 3 (22 faults a loop, image and sampler roots) and a clear statement of what the
next shape has to be: the SRT evaluated once per dispatch on the GPU, not once per wave.

Item 1b, added the same day, changes what the biggest remaining number means. The 26% of the render
thread in `SrtWalker.cpp` is not evaluation work at all: it is about ninety synchronous device
drains a loop, taken when the evaluator reads a page the GPU has just written, and 26 ms of a 73 ms
loop is the device doing work the render thread never lets it start early. The producer-only
readback built for it is correct and measures neutral, because 70% of those drains wait for a draw
the render thread has only just recorded. What is left is to stop the CPU reading those bytes
(stages 1b and 3, now priced in device time). Recording a whole command buffer before submitting
any of it has now been measured too: submitting every sixteen draws and dispatches, at the
boundaries where no dynamic-rendering pass is open, takes the loop from 73.7 to 70.5 ms with the
image unchanged -- real, reproducible, 4.3%, nothing on title-2, and below the 5 ms the experiment
was run for. It is parked behind `--gpu-submit-interval`, default 0.

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
