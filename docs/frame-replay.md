# Frame replay harness — scope, September 11, 2026

Record one frame of the parked Nexus once, replay it through the real render path in a loop
without the game. Turns the 40-minute end-to-end measurement into a bench that runs in under a
minute, deterministic, with Tracy and a screenshot diff. Motivation and the process it serves:
[performance-roadmap.md](performance-roadmap.md). Status: phases A (capture), B (replay),
C (validation), D (CPU-write timing) and E (buffer churn and scan timing) done. The bench runs from
one command, `_Build/replay-des.ps1`, and reproduces the parked-Nexus render thread to within a few
per cent. Phase E made the capture record the game's own BDA-scan timeline, so the harness is now
measured against the game run it came from instead of against a Tracy number from another build:
the replay makes **9479 preparations against 9479 recorded and 156 scans against 160**. The
`--gpu-descriptors` A/B still does not reproduce the end-to-end ordering, and phase E says why --
the game itself no longer scans 352 times a frame on this build. The capture to use is `nexus-5`
(format version 5).

## What it must do

1. **Capture.** In a normal game run, at a chosen frame, write everything the render path needs
   to reproduce that frame: guest memory, the frame's command submissions, the video-out
   registration, the command-processor register state, a screenshot of the frame.
2. **Replay.** Start the emulator without a game, restore the capture, feed the frame's
   submissions N times through the existing `GuestGpu` queue, report milliseconds per frame,
   and write the presented image after the last loop.
3. **Reproduce the stage 1 result.** Acceptance test: replaying a capture of the parked Nexus
   with `--gpu-descriptors true` and `false` must reproduce the measured ordering
   (9.81 versus 10.85 FPS, [gpu-descriptor-fetch.md](gpu-descriptor-fetch.md)) within 10%, in
   under one minute per configuration. Until it does, the harness is not done.

Success criteria: restore under 30 s; loop-to-loop jitter under 2%; the image after the second
loop matches the capture screenshot or the differences are listed and explained.

## Why it can work: what the render path actually reads

Everything the render thread consumes is reachable from three things, all in the emulator's
hands:

- **Guest memory.** Command buffers, indirect buffers (`IT_INDIRECT_BUFFER`,
  `CommandProcessor::ProcessIndirectBuffer`), SRTs, vertex and index data, textures, indirect
  arguments, labels. The guest address space has a fixed layout (`memoryAddressSpace.inc`, user
  range 0x1000000000 to 0xfbffffffff on Windows), mapped ranges are tracked in `VirtualRanges`
  (src/kernel/memory.cpp), direct memory is 13,824 MiB (`PhysicalMemory::TotalSize`). Same
  addresses can be restored in a fresh process.
- **Submissions.** `GuestGpu::Submit(draw, constant)`, `SubmitCompute(queue, commands)`,
  `SubmitFlipPreparation(request_id)`, `Done()` (src/graphics/guest_gpu/graphicsRun.cpp). Spans
  point into guest memory; the frame number is `m_done_num`. `GraphicsDbgDumpDcb` (src/libs/agc.cpp,
  `--command-buffer-dump`) already sits on this path and dumps PM4 as text; the recorder replaces
  it with a binary, replayable record.
- **Emulator state outside memory.** Video-out buffer registration
  (`VideoOutRegisterBuffers2`, src/graphics/presentation/videoOut.cpp), the command processors'
  register files (src/graphics/guest_gpu/hardwareContext.h, plain register structs), the flip
  request id.

The emulator already boots its subsystems before the game: `Init` in src/emulator.cpp brings up
Timer, Pthread, Memory, FileSystem and Graphics lifecycles, then `LoadElf` and the guest thread.
Replay mode runs `Init`, skips `LoadElf`, and starts a feeder thread instead of the guest thread.

## Capture design

Trigger: `--frame-capture <dir>`, plus either `--frame-capture-at <N>` for a fixed frame number or
a file named `trigger` dropped into `<dir>` while the game is parked. `--frame-capture-frames <K>`
(default 1) records K consecutive frames from the trigger frame on, and the snapshot is taken at the
`Done()` of the last of them. `--frame-capture-exit` (default true) ends the process once the
capture is flushed. All four are in [settings.md](settings.md). One capture is enough; it is reused
until the game's memory layout changes (a new save or build that changes the scene).

The on-disk format is [frameCaptureFormat.h](../src/graphics/replay/frameCaptureFormat.h); the
capture side is `src/graphics/replay/frameCapture.cpp`, driven from `GuestGpu::Done` and
`GuestGpu::Process`.

Taken at `Done()` of frame N, on the GPU thread:

1. **Quiesce.** `WaitForIdle()`, so frame N's GPU work is complete.
2. **Flush GPU-written memory back to guest memory.** Every buffer range the memory tracker
   marks GPU-modified (`MemoryTracker::ForEachDownloadRange`, `BufferCache::ReadMemory`) and
   every image the texture cache can read back. Ranges it cannot read back (image-owned ranges
   where `SyncGpuCleanBacking` returns false today) are listed in the capture manifest as gaps.
3. **Dump guest memory.** Walk `VirtualRanges`, write `ranges.json` (address, size, protection,
   name) and `memory.bin` sparse: 16 KiB pages, zero pages skipped, compressed. Expect a few GB
   and tens of seconds; acceptable for a one-time capture.
4. **Record the CPU-dirty page set of frame N** from the memory tracker, so replay can re-mark
   them dirty each loop and the dirty-upload path (`PrepareBda`) is exercised as in the game.
   Version 3 records, next to it, the frame's **CPU-dirty events**: every mark that reached
   `BufferCache::InvalidateMemory` or `BufferCache::MarkRegionAsCpuModified` during the frame, in
   arrival order, each keyed to the GPU thread's progress clock. The two are not the same set and
   neither replaces the other; see *Phase D* below.
4b. **Record what else moved the BDA generation and what it cost.** Version 4 adds
   `churn-events.bin`: every buffer registration and retirement (`BufferCache::ChangeRegister`) and
   every guest map and unmap (`GpuResourceManager::MapMemory`/`UnmapMemory`), keyed to the same
   clock. Version 5 adds `prepare-events.bin`: every `GpuResourceManager::PrepareBda` call of the
   frame, whether it scanned, and how many dirty ranges and buffers the scan walked. The first is
   diagnostics -- it is what proved the phase D churn hypothesis wrong -- and the second is the
   ground truth a replay is measured against. Both are in *Phase E* below.
5. **Dump emulator state.** Video-out registrations, the gfx and compute command-processor
   register files, the current flip request id, and the two things the render path needs that are
   neither in guest memory nor in the PM4 stream: the partially-resident-texture apertures
   (`prt.bin`, from `Memory::SnapshotPrtApertures`) and the AGC shader map (`shaders.bin`, from
   `ShaderSnapshotMap`). Both arrived with format version 2; see the section on them below for
   what happens without either.
6. **Screenshot** of frame N. *Not implemented on the capture side.* A capture has no `frame.png`
   or `frame.raw`; the manifest carries `width` and `height`, taken from the video-out attribute
   group. Phase B added the readback the capture would need
   (`Presenter::ReadLastPresentedFrame`, src/graphics/presentation/window/swapchain.cpp), so a
   capture-time screenshot is now a few lines: acquire the last presented frame and write it the
   way `--replay-image` does.

Taken during frame N (between `Done()` of N-1 and `Done()` of N), on the submit path:

7. **Record every submission** in the order the GPU thread *started processing* it, not the
   order of enqueue: type, queue, a copy of the command dwords and constant dwords, flip request
   id. Copying at enqueue is safe (the game must have the buffer ready at submit); ordering by
   processing start makes cross-queue `WAIT_REG_MEM` dependencies implicit, so replay can treat
   waits as satisfied. A `Done` record closes each frame and, from version 4, carries that frame's
   number and its progress count, so a multi-frame capture splits into frames without any per-frame
   index in the manifest.

Why snapshot at the end of frame N and replay frame N's submissions: the CPU-written inputs of
frame N (constants, SRTs, ring slots) are complete and not yet reused, and the GPU-produced
intermediates (culling results, indirect arguments, descriptors built by compute) regenerate on
replay. What this misses is documented under limits.

## Replay design

`kyty_emulator --replay <dir> [--replay-loops N] [--replay-frames M] [--replay-image out.raw]`,
plus the usual flags
(`--gpu-descriptors`, Tracy build, `--present-mode`). Flags in [settings.md](settings.md);
`--replay` and `--game` are mutually exclusive and `--replay` needs neither an app0 directory nor
an ELF. Everything below lives in `src/graphics/replay/frameReplay.cpp`.

1. `Init` as today, no ELF, no guest thread. `WindowRun()` still owns the main thread, so
   presentation works; a feeder thread does steps 2 to 5 and ends the process with `quick_exit`,
   the same exit the guest path takes: 0 after the report, non-zero on any replay error. The feeder
   first installs the host fault handler through `Loader::InstallHostFaultHandler()`. The ELF
   loader installs it for the guest path, and it is what turns an access violation on a guest page
   into `Memory::HandleGpuFault`; without it the first render-thread read of a page the GPU memory
   tracker has protected kills the process with no output. On Windows the replay also installs an
   unhandled-exception filter, so a fault nothing claims prints the faulting address, the access
   kind and the module offset instead of vanishing.
2. **Restore memory.** Each committed range goes through
   `Memory::RestoreRange(vaddr, size, prot, type, name)` (src/kernel/memory.cpp), which dispatches
   on the recorded `VirtualRangeType` to the map call the guest would have used
   (`KernelReserveVirtualRange`, `KernelAllocateDirectMemory` plus `KernelMapNamedDirectMemory`,
   `KernelMapNamedFlexibleMemory`, the memory-pool trio, or the private-committed path
   `AllocateProgramMemory`/`AllocateRuntimeMemory` use), always at the recorded address with the
   `MAP_FIXED` flag. Nothing bypasses `VirtualRanges`, the physical or flexible allocator, or
   `MapGpuRange`, so the memory tracker and the GPU page table see the range exactly as in the
   game. `memory.bin` is then streamed page by page — the file is never held in memory — and each
   page is written with `Memory::TryWriteBacking`, falling back to a direct copy with the host
   protection lifted for ranges that are committed rather than backing-store mapped. Restore time
   and bytes are printed.
3. **Restore state.** Video-out registrations replay through `VideoOutRegisterBuffers2` on a
   handle from the same `VideoOutOpen` path the game uses. Register files are copied into the
   command processors on the GPU thread through `GuestGpu::RestoreRegisterFile`; a record whose
   size is not `sizeof(HW::Context) + sizeof(HW::UserConfig) + sizeof(HW::Shader)` fails the
   replay with a message naming both sizes. Note that the graphics processor is `Reset()` again by
   the first `Submit` of every frame, in replay exactly as in the game, so the restore matters for
   the compute processors, which carry state across frames.
4. **Loop.** A loop is the recorded sequence of frames, in order; `--replay-frames M` replays the
   **last** M of them (default: all), because the last is the frame the memory snapshot ends on and
   every frame before it is that much further from the state the restore puts the guest in. For
   each frame of each loop: re-mark the recorded CPU-dirty pages
   (`BufferCache::MarkRegionAsCpuModified`, coalesced into runs and applied on the GPU thread) and,
   with them, that frame's recorded CPU-dirty events that arrived before its first draw; arm the
   rest of its events (applied inline on the GPU thread from version 5, by the marker thread for an
   older capture -- see *Phase D* and *Phase E*);
   then feed the frame's recorded submissions in file order (`Submit`, `SubmitCompute`, a fresh CPU
   flip where the capture recorded a `FlipPreparation`), wait for the GPU thread to drain, call
   `Done()`, wait for every flip queued on the video-out port to present, record wall time. Demon's Souls flips
   from its own graphics command stream, not through `VideoOutSubmitFlip`, so most captures carry
   no `FlipPreparation` record at all and the flip appears as the PM4 stream replays; the loop
   therefore ends on "the port's flip queue is empty", not on a recorded flip. A recorded flip
   request id belongs to the captured process and is never reused: where a capture does carry a
   `FlipPreparation`, the replay reserves its own request through the `VideoOutSubmitFlip` path.
   `WAIT_REG_MEM` executes as normal; with the snapshot's final label values and the recorded order
   it does not block. If a measured loop does not finish within 2 s the replay prints the last
   blocked wait's address, value, reference, mask and compare function and exits non-zero rather
   than spinning. The warm-up loop gets a much larger budget, because it compiles every pipeline
   the frame touches.
5. **Report.** One block on stdout and the same numbers as JSON in `replay-report.json` next to
   the capture. The first loop is a warm-up (cold pipelines, cold descriptor sets, cold history
   buffers) and is excluded from min, median, max and jitter. Two series are reported: `ms/loop`
   is first submit to presented flip, `ms/loop gpu` is first submit to GPU-thread idle, which is
   the render-path number and is free of the vblank thread's presentation cadence. Both are the
   sums over the loop's frames, and every frame also gets its own line and its own arrays in the
   JSON. A version 5 capture adds the line that matters most: the replay's own BDA preparations and
   scans against the ones the capture recorded in the game. Presentation is
   paced by the virtual vblank, so `ms/loop` is quantised to the vblank period and never falls
   below it; raise `--vblank-frequency` to shrink the quantum, or steer by `ms/loop gpu`. With Tracy
   attached the render thread's zone totals come from Tracy as usual.
6. **Image.** `--replay-image <path>` writes one image per frame of the last loop, as
   `<path>-f1.raw`, `<path>-f2.raw` and so on, plus `<path>` itself for the last of them. Each copies
   the presented swapchain frame into a host buffer
   (`Presenter::ReadLastPresentedFrame`) and writes it raw and tightly packed, in whatever video-out
   pixel format the guest chose (`VIDEO_OUT_FORMAT_POLICIES`, src/graphics/host_gpu/renderer/image/
   imageInfo.h), with a `<path>.json` sidecar carrying width, height, format, bytes per pixel and
   stride. Demon's Souls presents `A2B10G10R10_UNORM_PACK32`; reading that file as BGRA8 produces a
   picture with the right geometry and psychedelic colours, which is how phase C found that the
   sidecar used to name every non-RGBA8 format "BGRA8". There is no PNG encoder in the tree outside
   imgui's stb, so `_Build/replay-image-compare.py` converts and diffs the raw file.

### Report format

```
replay: <capture dir>
  frame          <first captured frame number>
  ranges         <n> restored, <n> skipped, <n> MiB committed, <n> ms
  memory         <n> pages, <n> MiB, <n> ms
  registers      <n> command processors, <n> bytes each
  video-out      <n> registrations, handle <h>, flip index <i>
  frames         <n> recorded (<a> to <b>), <m> replayed per loop (<c> to <b>)
  submissions    <n> records over the replayed frames (<n> graphics, <n> compute, <n> flips)
  dirty pages    <n> in <n> ranges
  prepares       <n> recorded over the replayed frames, <n> of them scanned (the ground truth)
  dirty events   <n> recorded (<n> timed in the replayed frames) over <n> draws
  dirty timing   applied inline on the GPU thread at each progress tick
  loops          <n> (<n> measured, loop 1 excluded), <n> flips presented
  ms/loop        min <a>  median <b>  max <c>  jitter <d>%
  ms/loop gpu    min <a>  median <b>  max <c>  jitter <d>%
  frame <i> <n>  gpu median <a>  min <b>  max <c> | loop median <d> | events <n> (<n> late) | progress <n> of <n>
  frame <i>      prepares <n> (<n> recorded), scans <n> (<n> recorded), <a> dirty ranges and <b> buffers a scan (recorded <c> and <d>)
  dirty events   <n> late of <n> timed (<p>%)
  total          <n> s
```

`replay-report.json` carries `capture`, `frame`, `loops`, `warmup_loops`, `recorded_frames`,
`frames`, `frame_numbers`, `restore_ranges_ms`, `restore_pages_ms`, `restore_pages`,
`restore_bytes`, `submissions`, `dirty_pages`, `dirty_events`, `dirty_events_timed`,
`dirty_events_late`, `progress_events_captured`, `progress_events_replayed`, the full `loop_ms` and
`gpu_ms` arrays, and `summary` / `gpu_summary` objects with `min_ms`, `median_ms`, `max_ms`,
`jitter` and `total_ms`, so two runs diff without re-parsing the console. Per replayed frame it
carries `frame_gpu_ms`, `frame_loop_ms`, `frame_gpu_summary`, `frame_summary`,
`frame_dirty_events_timed`, `frame_dirty_events_late`, `frame_progress_captured`,
`frame_progress_replayed`, and -- the phase E numbers -- `frame_prepares`, `frame_scans`,
`frame_scan_dirty_ranges`, `frame_scan_synchronized` with a `_recorded` counterpart for each.

### Emulator state outside guest memory, and what version 2 records

Two things the render path needs are neither in guest memory nor in the PM4 stream, and both stop
a replay dead. Format version 2 records them; a version 1 capture still opens, and the replay
either recovers the state or says it could not.

- **The AGC shader map** (`shaders.bin`). Every draw and dispatch resolves its shader through
  `ShaderMap` (src/graphics/shader/shader.cpp), which the game fills from `sceAgcCreateShader`; a
  replay runs no guest code, so without it the first dispatch exits with "is missing from
  ShaderMap". The capture writes one `ShaderRecord` per entry — the code address plus the guest
  `ShaderUserData*` and `ShaderSemantic*` and the three sizes — and the replay registers them back.
  For a version 1 capture the replay falls back to recovering the map from guest memory: every
  field of an entry comes from the guest AGC shader header, and `sceAgcCreateShader` rewrites that
  header in place before the capture, so the replay scans the restored readable ranges for the
  header signature (`file_header` 0x34333231 followed by `version` 0x18), validates each candidate
  against the ranges it restored, and re-registers it. On title-1 that recovered 4374
  registrations in 1.5 s, with no address claimed by two headers.
- **The partially-resident-texture apertures** (`prt.bin`). A streamed texture is a small committed
  head — one or two 64 KiB Direct ranges, protection 0xf3 — followed by a large type-0 reserved
  hole, and `BufferCache::ObtainBufferForImage` reads it through
  `Memory::TryReadPrtBacking`, which refuses unless `IsInPrtAperture` says the address is inside an
  aperture the game registered with `sceKernelSetPrtAperture`. The apertures live in
  `g_prt_apertures` (src/kernel/memory.cpp), not in guest memory, so a replay that does not restore
  them fails every such image with "BufferCache: failed to read mapped guest image backing". The
  capture snapshots them through `Memory::SnapshotPrtApertures()`; the replay puts them back with
  `KernelSetPrtAperture` before the first submission. A version 1 capture has no apertures and the
  replay says so in its report.
- **`RangeRecord` has no direct-memory offset or memory type.** Replay allocates a fresh physical
  block per direct range, so two virtual ranges that aliased one physical block in the game become
  two independent copies. Harmless for a frame that does not write through one alias and read
  through the other; a `uint64_t offset` and an `int memory_type` in the record would remove the
  doubt, and the offset is already in `VirtualRanges::Range`.
- **`SubmissionRecord::flip_request_id` is process-local.** It cannot be replayed, and for a title
  that flips from the command stream there is no `FlipPreparation` record at all. Nothing is lost
  today, because the PM4 stream carries the video-out handle and buffer index itself; a capture of
  a title that flips through `VideoOutSubmitFlip` would want the buffer index in the record, since
  the replay currently guesses the first registered index.
- **`RegisterFileRecord` does not say what the bytes are.** Replay reads them as `HW::Context`
  then `HW::UserConfig` then `HW::Shader`, in that order, and rejects any other size. The structs
  hold guest addresses only, no host pointers, so the raw copy is safe. Const RAM
  (`CommandProcessor::m_const_ram`) is not part of the record and is not captured.
- **No version stamp outside `manifest.json`.** The `.bin` streams have no magic, so a capture
  whose manifest is missing or edited is only caught by record-size arithmetic. A missing optional
  stream is indistinguishable from a version 1 capture, which is why the replay prints which of the
  two it used.

## What a capture actually contains

Two captures taken on September 11, 2026 with the build at `1cd3295-dirty`, RTX 5090 / Ryzen 9
9950X3D, under RDP. Both were written from `GuestGpu::Done()` with the flags in
[settings.md](settings.md), into `_Runtime/_Diagnostics/replay/`:

| | title screen, `--frame-capture-at 300` | parked Nexus, `trigger` file |
| --- | --- | --- |
| frame | 300 | 2069 |
| capture directory | 2.69 GiB | 6.96 GiB |
| write time | 3.75 s | 5.95 s |
| mapped ranges / committed | 1206 / 958 | 7068 / 5673 |
| non-zero 16 KiB pages | 176 206 (2.69 GiB) | 455 615 (6.96 GiB) |
| zero pages skipped | 198 978 | 203 466 |
| CPU-dirty pages of the frame | 19 538 | 75 489 |
| submissions | 25 | 40 |
| buffer bytes flushed back | 139 MiB | 672 MiB |
| images read back | 75 | 107 |
| gaps | 33 (167 MiB) | 56 (940 MiB) |

Both captures are under ten seconds and well inside the 30 s restore budget if restore is no worse
than a linear read.

Four things phase B should know before it reads a capture:

- **This title never calls `SubmitFlipPreparation`.** Neither capture has a `FlipPreparation`
  record: Demon's Souls flips from the graphics command stream
  (`VideoOutDriver::SubmitFlipFromGpu`, driven by a PM4 packet), not through
  `sceVideoOutSubmitFlip`. A replay loop must wait for the GPU-side flip the recorded packets
  trigger, not for a record in `submissions.bin`. The `FlipPreparation` kind stays in the format
  because the CPU path exists and other titles use it.
- **`constant_dwords` is always zero here.** `agc.cpp` only ever calls `Submit(dcb, {})`, so the
  constant-engine span is empty; const RAM is written by packets inside the draw stream.
- **Graphics and compute interleave inside one frame.** The Nexus frame is 40 records over the
  graphics queue and seven compute queues (`0x20`, `0x28`, `0x30`, `0x38`, `0x40`, `0x48`,
  `0x50`), in the order the GPU thread started them. That order is the whole point of the file:
  replayed on one thread it satisfies every `WAIT_REG_MEM` the original run satisfied.
- **The gaps are almost all images.** `page not readable` covers a few dozen 16 KiB pages in the
  private `Stack`/`Code`/`Runtime` ranges that neither the backing-store alias nor a
  `VirtualQuery`-guarded read could reach. The large gaps are render targets the texture cache
  could not stage: `BufferCache`'s download buffer is 32 MiB, and a 3840x2160 BGRA8 target is
  33.2 MiB, so the biggest surfaces — exactly the history buffers under *Limits* below — always
  fall out. Making them fit needs either a bigger download buffer or a chunked
  `TextureCache::TryDownloadImage`; both are cheap and belong with the phase B image diff, where
  the difference is visible.

## Limits, known up front

- **First loop is wrong for history buffers.** Textures the frame reads before writing
  (temporal effects) start with whatever the flush produced; from the second loop on they are
  the replay's own previous output, as in the game. Measure and diff from loop 2.
- **CPU writes during the frame are not replayed as writes.** The data is there, but the
  page-protection faults and uploads they cause in the game happen only through the re-marked
  dirty set. Close enough for render-thread work; not a model of guest-thread cost.
- **Guest threads are absent.** The 12 core equivalents of game threads are not simulated, so
  replay under-represents contention. Render-thread time is the metric, not FPS.
- **Image-owned ranges the texture cache cannot read back** are gaps in the snapshot; the
  manifest lists them. If a gap feeds a draw, the picture diff shows it.
- **Register state.** If the frame relies on registers set before frame N that its own constant
  buffer does not re-set, the register-file restore covers it; if the restore is incomplete, the
  diff shows it. The register structs hold no host pointers: `HW::Context`, `HW::UserConfig` and
  `HW::Shader` are trivially copyable and every address in them is a guest address, which the
  capture asserts. Two things phase B must know about them:
  - The **graphics** register file is not interesting. `GuestGpu::Submit` sets
    `reset_processor` from `m_graphics_done`, which `Done()` sets, so the first graphics
    submission of every frame calls `CommandProcessor::Reset` and clears the context, user-config,
    shader and const-RAM state. Frame N rebuilds it from its own packets.
  - The **compute** register files do carry over: `SubmitCompute` never resets. The capture takes
    them at `Done()` of frame N, which is the state frame N+1 starts from, not frame N. For a
    loop that is the consistent choice; the first loop can differ.
  - Const RAM (`CommandProcessor::m_const_ram`, 48 KiB) is not in `registers.bin`. It is cleared
    by the same per-frame `Reset` on the graphics queue and rebuilt by the frame's constant-engine
    packets, which the capture records.
- **Streamed mip content is not captured.** `memory.bin` holds the pages of committed ranges. The
  resident tail of a partially resident texture is committed and comes back; the streamed mips live
  in the sparse backing of the reserved part of the aperture, which the capture does not walk. With
  the apertures restored, `TryReadPrtBacking` zero-fills that part, so the frame runs and the
  picture lacks streamed detail. Dumping those pages needs a sparse-backing write path on the
  replay side, which the kernel does not have (`TryReadSparseBacking` has no counterpart), so
  closing this means recording the sparse mappings too, not just their contents.
- **A replay starts with an idle device, so its first loops are too fast.** On title-2 the first
  eight or nine measured loops sit near 11 ms of GPU-thread time, one loop spikes to 90 ms, and the
  run then settles at a stable 28 ms. Phase C found the cause and it is not the replay's
  bookkeeping (an empty dirty-page set reproduces it), not shader compilation (every "Shaders:"
  line falls in loop 1) and not the GPU resource garbage collector (a build that returns from both
  `RunGarbageCollector` bodies reproduces it spike for spike, and VRAM peaks at 3.9 GiB against a
  12 GiB GC trigger). Tracy names it: the whole 16 ms step is
  `CpOpDispatchIndirect::SyncArguments` (src/graphics/guest_gpu/command_processor/pm4Handlers.cpp),
  +16.7 ms per loop between the two regimes, 14 calls a loop going from 0.2 ms to 1.2 ms each. That
  zone is `SyncGpuCleanBacking` → `BufferCache::ReadMemory` → `Scheduler::WaitPriorityOperations`:
  the CPU reading back GPU-written indirect dispatch arguments, which drains the device. While the
  device queue is empty the drain returns immediately; once the replay has queued a few loops of
  work ahead, every drain waits for the device to catch up, and the spike is the first loop that
  pays off the whole backlog. So the stall is real — it is roadmap item 1, and the game pays it 245
  times per Nexus frame — but the *fast* loops are the artifact, not the slow ones. Consequences:
  exclude more than one loop when a capture's CPU cost is below its GPU cost, and read the plateau,
  not the median, on such a capture. It does not arise on nexus-2, where the render thread needs
  72 ms and the device only 28: the CPU never gets ahead, and the loop is flat from loop 3.
- **One machine, console session.** Replays are compared with replays, on the same build type.

## Running the bench

One command per comparison, from the repository root:

```
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '.\_Build\replay-des.ps1' `
    -Capture '_Runtime\_Diagnostics\replay\nexus-5' -Loops 60 -Repeats 2 -Image `
    -Configs '--gpu-descriptors false','--gpu-descriptors true'"
```

`-Command`, not `-File`: `-File` hands each argument to the script as a bare token, so a
configuration string that starts with a dash is taken for a parameter name and the call fails. From
an interactive PowerShell prompt, `& .\_Build\replay-des.ps1 -Configs '--a b','--c d'` is enough.
Without `-OutputRoot` the reports land in `<capture>\runs-<yyyyMMdd-HHmmss>`.

`replay-des.ps1` runs each flag set in turn — never two emulator processes at once, and it refuses
to start if one is already running — moves each `replay-report.json` into a timestamped
`runs-<date>-<time>` subfolder of the capture as `<config>-run<n>.json`, keeps each run's console
log beside it, writes `summary.json`, and prints one table: config, run, gpu median, gpu min, gpu
max, gpu jitter, loop median, report file. Parameters: `-Loops` (default 60), `-Configs` (one string
per flag set, split on whitespace; `''` means the defaults), `-Repeats` (how many times to walk the
whole list, so repeats of a configuration interleave instead of clustering), `-VblankFrequency`
(default 360; see `--vblank-frequency` in [settings.md](settings.md) — presentation is paced by the
virtual vblank, so at the default 60 Hz every `ms/loop` is quantised to 16.7 ms), `-Image` (writes
the last presented frame of the first repeat of each configuration next to the reports),
`-OutputRoot`, `-Exe`.

`_Build/replay-image-compare.py` turns a `--replay-image` dump into a PNG and diffs it against a
reference screenshot: it decodes every format the presenter can produce, including the packed
10-bit ones, crops the window grab's title bar off the reference, prints the mean absolute
difference per channel and writes a side-by-side PNG. It needs Pillow
(`python -m pip install Pillow`).

For Tracy, run the replay with `--profiler-direction Network` and capture it the usual way
(`_Build/profiling-tools/capture/tracy-capture.exe -a 127.0.0.1 -p 8086 -s 90 -f -o <file>`, then
`csvexport/tracy-csvexport.exe -e <file>` piped through `Out-File -Encoding utf8`). The warm-up
loop is seconds long and swamps a per-loop average, so take two captures at different `-Loops` and
difference their zone totals; that is what every replay zone number below is.

## Phase C validation, September 11, 2026

Build `646ccdc` plus the presented-format fix in this commit, RTX 5090 / Ryzen 9 9950X3D, Remote
Desktop session, Release. `ctest --test-dir _Build/windows -R frame_replay` passes. All artifacts
under `_Runtime/_Diagnostics/replay/`.

### The capture: nexus-2

`nexus-1` is format version 1 and cannot replay. `nexus-2` is the same scene recaptured on this
build: launch with `--frame-capture <abs>/nexus-2`, navigate with
`_Build/des-navigate.ps1 -NoElevate -WaitForFrame 1000 -HoldSeconds 4 -Presses 12 -GapSeconds 3`,
confirm the parked Nexus with `_Build/des-window.ps1 -Show` and then `-Shot` (screenshotting a
window the launcher left unshown gives a black PNG), save that PNG as `nexus-2/reference.png`, and
create `nexus-2/trigger`. The emulator exits by itself.

| | nexus-1 (v1) | nexus-2 (v2) |
| --- | --- | --- |
| frame | 2069 | 2113 |
| write time | 5.95 s | 5.50 s |
| mapped ranges / committed | 7068 / 5673 | 6344 / 4929 |
| non-zero 16 KiB pages | 455 615 (6.96 GiB) | 420 384 (6.41 GiB) |
| CPU-dirty pages of the frame | 75 489 | 74 625 |
| submissions | 40 | 40 (23 graphics, 16 compute, 0 flips) |
| PRT apertures | — | 1 |
| shader-map registrations | — | 9821 |
| buffer bytes flushed back / images | 672 MiB / 107 | 682 MiB / 107 |
| gaps | 56 | 55 |

Restore: 6344 ranges (9759 MiB committed) in 0.87 s, 420 384 pages (6.4 GiB) in 4.7 s, one aperture
and 9821 shader registrations in 1 ms. Under 6 s against the 30 s budget. The warm-up loop is 6.0 s
on a warm driver pipeline cache and carries all 507 `Shaders:` lines; 60 loops end to end take
10.3 s, so a two-configuration, two-repeat bench is four minutes.

### Acceptance A/B

`--replay-loops 60 --vblank-frequency 360`, two configurations, each run twice, interleaved; the
whole set run twice, `ab-phasec/` and then `ab-phasec-final/` on the binary this commit builds.
`ms/loop gpu` medians, loop 1 excluded:

| set | config | run 1 | run 2 | run-to-run | gpu min | median |
| --- | --- | --- | --- | --- | --- | --- |
| `ab-phasec/` | `--gpu-descriptors false` | 72.03 | 72.03 | 0.00% | 70.32 | 72.03 |
| `ab-phasec/` | `--gpu-descriptors true` | 71.25 | 70.58 | 0.95% | 68.85 | 70.92 |
| `ab-phasec-final/` | `--gpu-descriptors false` | 71.01 | 71.05 | 0.05% | 69.74 | 71.03 |
| `ab-phasec-final/` | `--gpu-descriptors true` | 71.57 | 70.96 | 0.86% | 69.45 | 71.27 |

Reports:
`nexus-2/ab-phasec{,-final}/gpudescriptors-{false,true}-run{1,2}.json`, with the printed table in
`summary.json` beside them.

Ratio true/false: **0.985** in the first set, **1.003** in the second. End to end it is **1.10**
(102 ms against 92 ms of render thread, 9.81 against 10.85 FPS,
[gpu-descriptor-fetch.md](gpu-descriptor-fetch.md)). **Acceptance is not met**: the ordering does
not reproduce, the two configurations are inside each other's run-to-run noise, and the ratio is
10% off rather than within 10%.

Everything else the scope asked for does hold, and the harness is not noisy — it is the
measurement that is being asked the wrong question. Restore is 6 s against a 30 s budget. Two runs
of one configuration land within 0.05% (false) and 0.9% (true) of each other. Across 59 measured
loops the standard deviation is 0.7 ms around a 71 ms median for `false`, so the scope's "jitter
under 2%" is met on the standard deviation; on the report's max-minus-min definition it is 4 to 8%
for `false` and 23% for `true`, in both cases from one or two outlying loops, the first measured
loop included (79.9 ms while the second is 72.9). Absolute level: 71 ms of replayed render thread
against 92 ms measured end to end, as the limits predicted, because no guest thread is competing
for the machine.

### Why the A/B does not reproduce, measured

Tracy self time, per frame in the game (`_Runtime/_Diagnostics/gpufetch/real3-self.csv`, 146 frames,
`--gpu-descriptors true`) against per loop in the replay (`nexus-2/tracy/nexus2-gd{,5}-*.csv`, a
30-loop capture minus a 5-loop one, so the warm-up cancels):

| zone | game, gd=true | replay, gd=false | replay, gd=true |
| --- | --- | --- | --- |
| `EvaluateRuntimeSourcesImpl` (SrtWalker.cpp) | 28.1 ms, 20 350 calls | 23.9 ms, 20 867 | 24.1 ms, 20 867 |
| `RenderExecutor::RebindBuffers` (descriptors.cpp) | 4.49 ms, 20 201 | 5.35 ms, 20 726 | 1.37 ms, 20 717 |
| `CpOpDispatchIndirect::SyncArguments` (pm4Handlers.cpp) | 3.65 ms, 245 | 6.44 ms, 245 | 9.96 ms, 245 |
| `GpuResourceManager::SynchronizeBdaBuffers` | **10.47 ms, 352 calls** | 0.53 ms, 9 | **0.23 ms, 28** |

The first three lines are the harness working: the same frame, the same call counts to within a few
per cent (245 indirect-argument syncs on the nose), the same dominant cost, and stage 1 visibly
doing what it claims — `RebindBuffers` drops by 4 ms and all of descriptors.cpp by 5 ms when
`--gpu-descriptors` goes on.

The last line is why the A/B fails. The BDA scan runs when the buffer or mapped generation changes.
In the game those changes come from guest CPU writes faulting pages *during* the frame, 352 times,
at 30 µs each. The replay re-marks the whole recorded dirty set in one batch before the loop starts,
so the generation moves 28 times, each scan has less to do, and the total is 0.23 ms instead of
10.47. That missing **10.2 ms** is almost exactly the **10 ms** that separates the end-to-end `true`
and `false` frames. The harness is not mismeasuring the render path; it is not modelling *when* the
guest dirties pages, and this particular A/B is made of nothing else.

Closing it means recording the dirty set per submission instead of per frame: the capture would
snapshot the memory tracker's CPU-dirty delta at each submission boundary and the replay would
re-mark each slice between submissions. That is a format version 3 change, so it is written up as
the next item rather than attempted here. Until then the rule is: **the harness measures
render-path work faithfully, and must not be used to judge a change whose cost lives in the guest's
page-write pattern.** Stage 1 of the GPU descriptor fetch is exactly such a change; items 1, 3 and 4
of the roadmap are not.

### Image (phase C)

`--replay-image` on one run of each configuration, decoded and diffed with
`_Build/replay-image-compare.py` against `nexus-2/reference.png`. The scene matches: same camera,
same geometry, same HUD, same archstone glyphs, same particles. The two configurations agree to the
eye and nearly bit for bit: 1.3% of sampled pixels differ at all, by 0.018 of a level out of 1023
averaged over every sampled channel, worst case 74 — floating-point ordering in the fetched path,
not a rendering difference. Mean absolute difference against the reference is R 10.9, G 8.7, B 4.7
out of 255,
from two causes, both predicted under *Limits*: the replay is slightly brighter, because the
auto-exposure history buffer is among the 940 MiB of gaps the capture could not read back, and two
item icons in the bottom-left HUD are missing, because their streamed mips live in the sparse part
of a PRT aperture the capture does not walk. Loop count does not change it — the dumps after 2, 4,
10 and 60 loops differ by less than 0.1 of a level in mean difference, so the history buffers reach
their fixed point in the first loop and do not drift. Side by side:
`nexus-2/ab-phasec-final/gpudescriptors-false-vs-reference.png`.

This is also how phase C found that the image sidecar lied. It named every format that is not
`R8G8B8A8Unorm` "BGRA8"; this title presents `A2B10G10R10_UNORM_PACK32`, and reading that as BGRA8
yields a picture with perfect geometry and psychedelic colour, which reads as a rendering bug and is
not one. `Presenter::ReadLastPresentedFrame` now names the real format and carries its bytes per
pixel, which also fixes a latent quarter-size readback for the `R16G16B16A16Sfloat` video-out
policy.

### The sawtooth

Reproduced on title-2 on this build: loops 2 to 9 at 11 to 12 ms of GPU-thread time, one loop at
90 ms, then a stable plateau at 28 ms for the rest of the run (`title-2/sawtooth/base.json`, 40
loops: min 11.0, median 27.8, max 91.1). What it is not:

- not the dirty-page re-marking — phase B reproduced it with an empty dirty set;
- not shader compilation — every `Shaders:` line falls in loop 1;
- not the GPU resource garbage collector. A build that returns immediately from both
  `RunGarbageCollector` bodies reproduces it spike for spike (`title-2/sawtooth/gc-off.json`:
  median 28.11 against 27.97 with the collector on), and `nvidia-smi` polled every 100 ms through a
  200-loop run shows VRAM peaking at 3.9 GiB against the texture cache's roughly 12 GiB trigger and
  device utilisation at 21 to 23% (`title-2/sawtooth/gpu-util.csv`). The collector never runs, and
  the device is not the limit either.

Tracy names it. Differencing the per-zone totals of a 10-loop and a 40-loop capture
(`title-2/sawtooth/title2-{10,40}.csv`), the extra 30 loops cost **+16.7 ms each in
`CpOpDispatchIndirect::SyncArguments`** — the entire 16 ms step, in one zone, 14 calls a loop going
from about 0.2 ms to 1.2 ms each. That zone is `SyncGpuCleanBacking` → `BufferCache::ReadMemory` →
`DownloadBufferMemory` → `Scheduler::WaitPriorityOperations`: the CPU reading GPU-written indirect
dispatch arguments back, which drains the device.

So the stall is real and it is roadmap item 1; what is a replay artifact is the *fast* phase. A
fresh replay starts with an idle GPU, so the early drains return at once; once a few loops of work
are queued ahead, every drain waits for the device, and the 90 ms loop is the one that pays off the
backlog. Read the plateau on such a capture, not the median. It does not arise on nexus-2, where the
render thread needs 71 ms and the device about 28, so the CPU never gets ahead and the loop is flat
from loop 3 — one more reason the parked Nexus, not the title screen, is the capture to measure on.

## Phase D, CPU-write timing, September 11, 2026

Phase C left one hypothesis: the A/B fails because the replay does not model *when* the guest
dirties pages. Phase D recorded that timing, replayed it, and measured the result. The timing is
now reproduced exactly and the hypothesis is **half right**: it accounts for 1.3 ms of the missing
10.2, and the measurement below says where the rest is.

### The progress clock

Wall time means nothing across a game run and a replay, so the capture keys each CPU write to the
GPU thread's own progress: `GuestGpu::Progress()`, a per-frame counter bumped once per draw
(`CommandProcessor::DrawIndex`, `DrawIndexAuto`) and once per dispatch (`DispatchDirect`), reset by
`Done()`. It is one relaxed atomic increment and the only always-on addition phase D makes; a run
with neither `--frame-capture` nor `--replay` is otherwise untouched.

The clock reproduces perfectly. The Nexus frame is **11 259** draws plus dispatches, and every
replay loop of it ends on 11 259 as well.

### The v3 stream

`dirty-events.bin` is a packed stream of `{uint32 progress; uint32 submission; uint64 vaddr;
uint64 size}` in arrival order (`frameCaptureFormat.h`). A record is appended by
`Replay::RecordDirtyEvent`, called from `BufferCache::InvalidateMemory` and
`BufferCache::MarkRegionAsCpuModified` — the point every guest CPU write reaches, whether it came
through the host page-fault handler (`GpuResourceManager::HandleFault`) or a kernel write path
(`Memory::InvalidateMemory`). When no capture is armed the path costs one relaxed atomic load.
`dirty-pages.bin` is unchanged, so a v1 or v2 reader is unaffected, and the manifest gains
`dirty_events` and `progress_events`. A capture without the stream replays as before, behind a
one-line warning.

On replay, a **marker thread** walks the events in arrival order, spins on the progress counter
until it reaches the value the event carries, and invalidates the range through
`BufferCache::InvalidateMemory` — the game's own path, not a bare mark: a range the GPU still owns
has to be flushed back before its CPU state changes, and marking it outright from another thread
trips the tracker's "CPU dirty state conflicts with GPU dirty state" check on the first loop.
Events recorded at progress 0 arrived before the frame's first draw and are applied with the batch,
on the GPU thread, outside the loop timing. At the end of a loop the thread stops waiting, applies
whatever is left and reports it as *late*; the report prints the count, so a loop that outran the
marks is visible rather than silent.

### What a frame's CPU writes actually are

The first v3 capture corrected the phase C picture immediately. The recorded dirty **page set** and
the recorded CPU **writes** are different things, and the replay needs both:

| | nexus-3 |
| --- | --- |
| CPU-dirty pages at `Done()` (`dirty-pages.bin`) | 72 444 |
| CPU-dirty marks during the frame (`dirty-events.bin`) | 2 941 |
| ... of which before the frame's first draw | 2 010 |
| ... 16 KiB pages they cover | 884 |
| ... of those, also in the 72 444 | **5** |
| mark sizes | 2 931 of 1 byte, 10 larger |

Almost none of the 72 444 pages is dirty because the frame wrote it: they are pages nothing has
uploaded yet. So the batch re-mark stays — it is what makes a loop upload the frame's working set —
and the events go on top of it. And a faulting write marks a whole 4 KiB tracker page and unprotects
it, so a page faults at most once between uploads: 2 941 marks a frame is the real number, not an
undercount.

The events are also heavily **front-loaded**: 2 010 at progress 0, 851 more inside the first 2 000
draws, and 80 over the remaining 9 259. That is the guest thread's shape, not an artifact — it
builds frame N+1 while the GPU starts frame N, then blocks in `Done()` → `WaitForIdle()` for the
rest of the frame.

### The capture: nexus-3

Same scene, same procedure as nexus-2, on the build of this branch.

| | nexus-2 (v2) | nexus-3 (v3) |
| --- | --- | --- |
| frame | 2113 | 2025 |
| write time | 5.50 s | 5.72 s |
| mapped ranges / committed | 6344 / 4929 | 6355 / 4940 |
| non-zero 16 KiB pages | 420 384 (6.41 GiB) | 425 177 (6.49 GiB) |
| CPU-dirty pages of the frame | 74 625 | 72 444 |
| CPU-dirty events / progress events | — | 2 941 / 11 259 |
| submissions | 40 | 40 (23 graphics, 16 compute, 0 flips) |
| PRT apertures / shader registrations | 1 / 9821 | 1 / 9815 |
| buffer bytes flushed back / images | 682 MiB / 107 | 690 MiB / 109 |
| gaps | 55 | 52 |

Restore is 6355 ranges in 0.86 s and 425 177 pages in 4.9 s, as before.

### Acceptance A/B

`--replay-loops 60 --vblank-frequency 360`, two configurations, each run twice, interleaved
(`nexus-3/ab-phased/`). `ms/loop gpu` medians, loop 1 excluded:

| capture | config | run 1 | run 2 | median | gpu min |
| --- | --- | --- | --- | --- | --- |
| nexus-2 (phase C) | `--gpu-descriptors false` | 71.01 | 71.05 | 71.03 | 69.74 |
| nexus-2 (phase C) | `--gpu-descriptors true` | 71.57 | 70.96 | 71.27 | 69.45 |
| nexus-3 (phase D) | `--gpu-descriptors false` | 81.56 | 81.16 | **81.36** | 74.38 |
| nexus-3 (phase D) | `--gpu-descriptors true` | 77.24 | 77.69 | **77.47** | 74.77 |

Ratio true/false: **0.952**, against 1.003 in phase C and **1.10** end to end. Replaying the writes
made both configurations slower — 10 ms for `false`, 6 ms for `true` — and moved the ratio the wrong
way. Late events: **0** of 931 per loop, in every run; the replayed progress is 11 259 of 11 259
recorded. The picture is unchanged: mean absolute difference against `nexus-3/reference.png` is
R 10.5, G 8.3, B 4.4, against R 10.9, G 8.7, B 4.7 in phase C, from the same two causes.

### Zones, per loop against the game per frame

30-loop minus 5-loop Tracy captures of both configurations (`nexus-3/tracy/nexus3-gd{,5}-*.csv`)
against `_Runtime/_Diagnostics/gpufetch/real3-self.csv`, 146 frames, `--gpu-descriptors true`:

| zone | game, gd=true | nexus-2, gd=true | nexus-3, gd=true |
| --- | --- | --- | --- |
| `GpuResourceManager::SynchronizeBdaBuffers` | **10.47 ms, 352.1** | 0.23 ms, 28 | **1.56 ms, 172.2** |
| `RenderCompute::PrepareBda` (calls only) | 351.9 | — | 352.0 |
| `EvaluateRuntimeSourcesImpl` | 28.14 ms, 20 350 | 24.1 ms, 20 867 | 26.40 ms, 20 816 |
| `RenderExecutor::RebindBuffers` | 4.49 ms, 20 201 | 1.37 ms, 20 717 | 3.22 ms, 20 682 |
| `CpOpDispatchIndirect::SyncArguments` | 3.65 ms, 244.9 | 9.96 ms, 245 | 4.53 ms, 245.0 |

With `--gpu-descriptors false` the replay scans 17.7 times a loop for 1.03 ms.

### Acceptance: not met, and what the rest of the gap is

- The ratio of `ms/loop gpu` medians is **0.952**; within 10% of the game's 1.10 means 0.99 to 1.21.
- `SynchronizeBdaBuffers` runs **172.2** times a loop; within 25% of 352 means 264 to 440.

Neither is met, and nothing was tuned to chase them. The measurement above says why, and it is not
the write timing, which is now exact:

1. **The scan count is short by half, in the part of the frame that has no CPU writes.** In the game
   `RenderCompute::PrepareBda` is called 351.9 times a frame and `SynchronizeBdaBuffers` runs 352.1
   times: the BDA generation has moved before *every single* compute preparation. The replay makes
   the same 352.0 preparations and finds a changed generation only 172 times. Since 80 of the
   frame's 931 timed writes fall in its last 9 259 draws, the game's generation cannot be moving on
   CPU writes there — something else is moving it.
2. **That something else is buffer churn.** The generation is bumped by
   `BufferCache::ChangeRegister` on every buffer registration and every buffer retirement
   (`bufferCache.cpp:99` and `:102`) and by `GpuResourceManager::MapMemory`, not only by the
   CPU-write path. In the game the guest's allocators move constantly, so buffers are created and
   retired all through the frame and guest threads map and unmap memory. A replay loop runs over the
   buffer set loop 1 built, maps nothing, and retires almost nothing, so all of those bumps are
   missing — and they are exactly the ones spread evenly across the frame.
3. **The cost per scan follows from the same cause.** The game pays 29.7 µs a scan, the replay
   9.07 µs. A scan resynchronizes the buffers intersecting the ranges accumulated since the last
   one; in the game those are the ranges of the buffers it has just built, in the replay they are
   the 884 pages of a frame's worth of 1-byte marks, only five of which are in the frame's dirty
   set. Half the calls at a third of the cost is 1.56 ms against 10.47.

> **Points 2 and 3 above are wrong, and phase E measured why.** There is no buffer churn in the
> parked Nexus at all -- zero registrations, retirements, maps and unmaps in every one of six
> consecutive frames -- so nothing was missing from the *sources* of the generation bumps. What was
> missing was the *resolution* of the clock the marks are keyed to, and, above all, a way to check
> the replay against the game's own scan timeline instead of against a Tracy number taken on
> another build. See *Phase E* below; the measurements in this section stand, the explanation of
> the residue does not.

So the rule from phase C narrows rather than lifts: **the harness measures render-path work
faithfully, and still must not be used to judge a change whose cost lives in the guest's memory
behaviour.** Stage 1 of the GPU descriptor fetch is such a change; roadmap items 1, 3 and 4 are not.
Phase E narrows it further: the behaviour in question is *when the guest's page writes arrive
relative to the render thread's BDA preparations*, and that the harness now reproduces.

## Phase E, buffer churn and scan timing, September 11, 2026

Phase D closed on a hypothesis: the replay's BDA scan count is short because a replay loop never
registers, retires, maps or unmaps anything, and those generation bumps are what fill the part of
the game's frame that has no CPU writes. Format version 4 measured the hypothesis and it is
**false**. Format version 5 then made the capture record the game's own scan timeline, which is
what the harness should have been measured against all along.

### There is no churn to reproduce

`churn-events.bin` records every `BufferCache::ChangeRegister` (registration and retirement) and
every `GpuResourceManager::MapMemory` and `UnmapMemory`, each keyed to the progress clock, for every
captured frame. `nexus-4` is six consecutive parked-Nexus frames
(`--frame-capture-frames 6`, frames 1774 to 1779):

| frame | 1774 | 1775 | 1776 | 1777 | 1778 | 1779 |
| --- | --- | --- | --- | --- | --- | --- |
| buffer registrations | 0 | 0 | 0 | 0 | 0 | 0 |
| buffer retirements | 0 | 0 | 0 | 0 | 0 | 0 |
| guest maps / unmaps | 0 | 0 | 0 | 0 | 0 | 0 |
| CPU-dirty marks | 2677 | 2681 | 2654 | 2663 | 2660 | 2684 |
| draws plus dispatches | 10 845 | 10 849 | 10 849 | 10 845 | 10 847 | 10 847 |

**Zero, of every kind, in every frame.** The recorder is not broken: a capture of frames 30 to 32 of
the same run, while the game is still building its scene, records 15 registrations over three frames
(`_Runtime/_Diagnostics/replay/churn-check`), and `nexus-5`, taken with `--gpu-descriptors true`,
records 8 registrations and 9 retirements in its frame -- two orders of magnitude below the hundreds
the phase D hypothesis needed. In the parked Nexus the buffer set and the guest mapping set are
static. There is no ring rotating, so **no number of looped frames can add churn that the game does
not have**, and the multi-frame capture, useful as it is for measuring this, cannot close the gap.

The analysis script is `_Runtime/_Diagnostics/replay/nexus-4/analyse-churn.py` (it reads any capture:
`python analyse-churn.py ../nexus-5`). It prints the churn per kind per frame, the overlap between
the registration addresses of frame *i* and frames *i-1* to *i-4* -- the ring-period question, which
in this scene has no data to answer -- the mark sizes, and the distribution of marks over the frame.

Since every other generation bump (`InvalidateMemory`, `MarkRegionAsCpuModified`, and so the host
fault handler and every kernel write path) is already recorded as a CPU-dirty mark, and churn is
zero, **every BDA scan the game pays comes from a CPU write the capture already holds.** The
question stopped being "what is missing" and became "when exactly does each mark arrive".

### The ground truth: the game's own scan timeline

Version 5 records `prepare-events.bin`: one record per `GpuResourceManager::PrepareBda` call of the
captured frame -- `{frame, progress, scanned, dirty ranges, buffers synchronized, dirty bytes}`.
`scanned` is exactly one entry of the `GpuResourceManager::SynchronizeBdaBuffers` Tracy zone, so a
capture now carries the number the acceptance test is about, measured inside the game run that
produced the capture. `nexus-5`, game frame 1739, taken with `--gpu-descriptors true`:

| | nexus-5, in the game |
| --- | --- |
| BDA preparations | **9479** (352 of them from compute dispatches, the rest from draws) |
| of those, scans | **160** (1.7%) |
| per scan | 4.3 dirty ranges, 4.3 buffers, 107.6 KiB |
| per frame | 693 dirty ranges, 16.8 MiB synchronized |
| CPU-dirty marks | 2743, of which 1991 before the first draw |
| timed marks | 752 on 301 distinct progress ticks |

The shape is the point. Preparations are spread evenly over the frame -- about 1080 per tenth --
while the scans are not: 127 of the 160 fall in the second tenth, exactly where the guest's writes
are (470 of the 752 timed marks). The scan count is a function of *where the guest's page writes
fall relative to the preparations*, and of nothing else.

### The captures

`nexus-4` and `nexus-5` follow the phase C procedure exactly (launch with `--frame-capture <abs>`,
`_Build/des-navigate.ps1 -NoElevate -WaitForFrame 1000 -HoldSeconds 4 -Presses 12 -GapSeconds 3`,
`des-window.ps1 -Show` then `-Shot` as `reference.png`, then create `trigger`); `_Build/capture-frame.ps1`
wraps the launch with the environment the root launcher sets, which the game needs to reach gameplay.

| | nexus-3 (v3) | nexus-4 (v4) | nexus-5 (v5) |
| --- | --- | --- | --- |
| frames | 1 (2025) | 6 (1774 to 1779) | 1 (1739) |
| flags | default | default | `--gpu-descriptors true` |
| write time | 5.72 s | 5.78 s | 5.66 s |
| mapped ranges / committed | 6355 / 4940 | 6394 / 4974 | 6425 / 5005 |
| non-zero 16 KiB pages | 425 177 (6.49 GiB) | 426 663 (6.51 GiB) | 424 329 (6.47 GiB) |
| CPU-dirty pages | 72 444 | 76 058 | 75 272 |
| CPU-dirty events | 2941 | 16 032 (2654 to 2684 a frame) | 2755 |
| churn events | -- | **0** | 21 (8 registrations, 9 retirements) |
| prepare events / scans | -- | -- | **9479 / 160** |
| submissions | 40 | 240 | 40 |
| PRT apertures / shaders | 1 / 9815 | 1 / 9824 | 1 / 9810 |

The picture is unchanged by any of it: the last frame of nexus-4 replayed against its reference is
R 11.3, G 9.2, B 5.0 mean absolute difference, and nexus-5 is R 11.0, G 8.5, B 4.4, against phase
D's R 10.5, G 8.3, B 4.4 -- the same two causes, the auto-exposure history buffer among the capture's
gaps and two streamed HUD icons in the sparse part of a PRT aperture.

### The finer clock and inline application

Two changes make a replay able to reproduce that:

- **The progress clock ticks twice per draw and dispatch** (version 5): once when it starts and once
  after its resource preparation stage. The second tick is unconditional, not tied to whether the
  stage needed a BDA preparation, so the clock does not depend on `--gpu-descriptors`. A mark that
  arrives before a draw's `PrepareBda` and one that arrives after it used to collapse onto one
  value; now they do not. On nexus-5 the frame is 21 638 ticks against 10 819 draws and dispatches.
  The always-on cost of the clock is now two relaxed atomic increments per draw and dispatch instead
  of one, plus a relaxed load of a null hook pointer at each; a run with neither `--frame-capture`
  nor `--replay` is otherwise untouched, and `PrepareBda` gains one relaxed load of a null sink.
- **The marks are applied inline on the GPU thread**, from the progress hook, at the tick they were
  recorded at, through the same `InvalidateMemory` the game's own writes reach. The phase D marker
  thread waits for the counter to pass a value and then marks from another thread, so it could
  arrive after the preparation it was meant to precede. A capture older than version 5 keeps the
  marker thread, because its progress values mean something else.

### The result

`--replay-loops 40 --vblank-frequency 360 --gpu-descriptors true`, nexus-5, 39 measured loops:

| | game (recorded) | replay | |
| --- | --- | --- | --- |
| BDA preparations a frame | 9479 | **9479** | exact |
| BDA scans a frame | 160 | **156** | -2.5% |
| progress ticks a frame | 21 638 | 21 638 | exact |
| late marks | -- | 0 of 752 a loop | |
| dirty ranges a scan | 4.3 | 17.2 | 4.0x |
| buffers synchronized a scan | 4.3 | 17.2 | 4.0x |
| `SynchronizeBdaBuffers` | -- | 1.01 ms, 156 calls, 6.5 us each | Tracy, 30 minus 5 loops |

With `--gpu-descriptors false` the same replay makes 357 preparations and 16 scans: the flag is what
decides how many stages need a BDA preparation at all, which is why the ground truth was captured
with it on.

The count is reproduced. The **cost per scan is not**: the replay's scans walk four times as many
dirty ranges as the game's, because every replayed frame re-marks the whole recorded CPU-dirty page
set (76 058 pages) to make the loop upload the frame's working set, and those ranges land in the
frame's first scans. They are cheap ranges -- already-resident buffers -- so the replay pays 6.5 us
a scan, 1.0 ms a frame. That is a fidelity question about the *inputs* of a scan, not about its
timing, and it is the next thing to close if the BDA zone ever needs to be judged in replay.

### The 352 scans, and what acceptance means now

The acceptance target -- 352 scans at 29.7 us, 10.47 ms a frame -- comes from
`_Runtime/_Diagnostics/gpufetch/real3-self.csv`, a Tracy capture of the game on the build of
September 10 (51 402 zone entries over 146 frames). In that run `SynchronizeBdaBuffers` (352.1 a
frame) and `RenderCompute::PrepareBda` (351.9 a frame) are the same number: *every compute
preparation scanned*, and the draw path made no BDA preparations at all.

On the build of this branch the same scene with the same flag makes 9479 preparations a frame, 9127
of them from draws, and 160 of them scan. The replay's Tracy agrees with its own counter
(`RenderCompute::PrepareBda` 351.8 a loop, `SynchronizeBdaBuffers` 156). So **the game itself no
longer does 352 scans a frame**: the target is a number from another build, and the harness is now
measured against the ground truth the capture carries.

Acceptance, restated:

- Scans within 25% of the recorded ground truth: **160 recorded, 156 replayed, met.**
- Scans within 25% of 352: **not met, and not meetable** -- the game does 160 on this build.
- `--gpu-descriptors` true/false ratio within 10% of 1.10 (that is 0.99 to 1.21), `ms/loop gpu`
  medians, `_Build/replay-des.ps1 -Loops 40 -Repeats 2` (`nexus-5/ab-phasee/`):

| config | run 1 | run 2 | median | gpu min | scans a frame |
| --- | --- | --- | --- | --- | --- |
| `--gpu-descriptors false` | 71.54 | 71.61 | **71.58** | 69.73 | 16 |
| `--gpu-descriptors true` | 71.99 | 71.93 | **71.96** | 69.78 | 156 |

Ratio true/false **1.005**. It is inside the stated window by arithmetic, and it does not reproduce
what the window was for: end to end, `true` is 10% *slower* than `false` (102 against 92 ms of
render thread); in replay it is 0.5% slower, within run-to-run noise (0.1%). The 10 ms the A/B is
made of was 352 scans at 29.7 us on the old build. Today the game's own scan budget for that frame
is 160 scans, and the replay pays 1.0 ms of it. **The A/B as written cannot be reproduced, because
the effect it measures is no longer there to reproduce; re-baselining it needs a fresh end-to-end
Tracy run of the game on this build.** That run is also the only way to get today's cost per scan in
the game, which the capture does not record -- adding the scan's duration to `PrepareEventRecord` is
a one-line extension and the obvious next step if the number is wanted without a 40-minute run.

### The multi-frame loop, and why K is 1 on this title

`--frame-capture-frames K` and `--replay-frames M` work, and the six-frame nexus-4 is what measured
the churn above. But **only the last frame of a capture can be replayed on this title.** The memory
snapshot is taken at the end of the last captured frame, and the guest advances a label in guest
memory once or twice per frame; an earlier frame's `WAIT_REG_MEM` then waits for a value the
snapshot has already passed, with `func` 3 (equal), and never completes:

| replayed frame | frames before the snapshot | waits for | memory holds |
| --- | --- | --- | --- |
| 1779 (the last) | 0 | -- | replays, 0 late marks, 78.8 ms |
| 1778 | 1 | 0x6f1 | 0x6f3 |
| 1774 | 5 | 0x6ed | 0x6f3 |

The label is guest-CPU-written, and a replay reproduces the *dirtying* of guest memory, not its
data, so no ordering of the loop helps: after the last frame the state is the snapshot's again, and
frame 1 of the next iteration is as stale as before. `--replay-frames` therefore keeps the **tail**
of a capture, and a multi-frame capture is a diagnostic instrument rather than a longer loop. Making
K > 1 replayable means recording, per frame, the values of the labels that frame's waits read, and
writing them back before the frame -- the same shape of mechanism as `RestoreRange`, and worth doing
only for a title whose frames actually differ.

## Phases

| Phase | Deliverable | Estimate |
| --- | --- | --- |
| A. Capture | flag, quiesce and flush, sparse memory dump, submission recorder in processing order, state dump, manifest; format in `frameCaptureFormat.h`. Done except the screenshot, which needs a presenter readback path | 1 agent-day |
| B. Replay | `--replay`, memory and state restore, feeder thread, loop timing and report, image readback | 1 to 2 agent-days |
| C. Validation | capture the parked Nexus on the current build; 60 loops; compare ms per loop with the 106 ms render-thread frame from `real3-self.csv`; A/B `--gpu-descriptors`; image diff; one command. Done, and the A/B does not reproduce — see *Phase C validation* above | half a day |
| D. CPU-write timing | format version 3: a progress clock on the GPU thread, `dirty-events.bin`, a marker thread that replays each write where it happened. Done; the timing is exact (0 late events, 11 259 of 11 259 progress) and the BDA scan goes from 28 to 172 calls a loop, but the A/B still does not reproduce — see *Phase D* above | half a day |
| E. Buffer churn and scan timing | format version 4: `churn-events.bin` and multi-frame captures, which measured the churn at **zero** and refuted the phase D explanation. Format version 5: `prepare-events.bin` (the game's own scan timeline), a clock that ticks twice per draw and dispatch, and inline application of the marks on the GPU thread. Done; the replay makes 9479 preparations against 9479 recorded and 156 scans against 160, and the A/B's 352-scan target is shown to belong to an older build — see *Phase E* above | one day |

Phase B status, September 11, 2026: `--replay` replays a real capture end to end. On title-2, the
title screen at frame 300 captured with format version 2, it restores 1190 ranges (6123 MiB) in
206 ms, 182,741 pages (2855 MiB) in 2.0 s, 1 PRT aperture and 4374 shader registrations, then runs
5 loops, presents 5 flips, writes `replay-report.json` and a 3840x2160 BGRA8 image, and exits 0.
Measured loops: **ms/loop gpu min 11.26, median 11.49, max 13.68** (jitter 21%), ms/loop 16.3 to
16.7 (vblank-paced). The warm-up loop is 2.24 s on a cold shader cache.

Three things had to be added before a real capture would run, all of them emulator state the
format did not carry:

1. **The host fault handler.** `KytyExceptionHandler` (src/loader/runtimeLinker.cpp) is what turns
   an access violation on a guest page into `Memory::HandleGpuFault`, and only `LoadProgram`
   installed it. Without an ELF the first render-thread read of a page the GPU memory tracker had
   protected killed the process with no output. Exported as
   `Loader::InstallHostFaultHandler()` and called by the replay.
2. **The AGC shader map**, now `shaders.bin` (format v2), with the guest-memory scan as the
   fallback for version 1 captures.
3. **The PRT apertures**, now `prt.bin` (format v2). This was the cause of
   "BufferCache: failed to read mapped guest image backing" on title-1 and nexus-1, which looked
   like decommitted memory and is not: a streamed texture is a 64 or 128 KiB committed Direct head
   followed by a reserved hole, read through `Memory::TryReadPrtBacking`, which refuses outside a
   registered aperture. Restoring the one aperture Demon's Souls registers fixes every such image.

That recapture is done. **`nexus-5`, format version 5, is the capture to use**; `nexus-4` (format 4,
six frames) is the churn measurement, `nexus-3` is the phase D reference, `nexus-2` the phase C one
and `nexus-1` is kept only because its screenshot documents the scene.

Code goes under `src/graphics/replay/` (capture and replay), flags in
[settings.md](settings.md), the capture format and the report format in this file. Both flags
are opt-in; the game path is untouched when they are off.

## How it changes the process

Every item on the roadmap is measured in replay first. An agent's brief states the expected
milliseconds per frame, runs replay before and after, and stops if the number does not move.
One end-to-end run per integrated item confirms the game still boots, plays and renders; the
full A/B on the console session is reserved for milestones.
