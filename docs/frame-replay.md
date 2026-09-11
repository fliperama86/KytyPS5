# Frame replay harness — scope, September 11, 2026

Record one frame of the parked Nexus once, replay it through the real render path in a loop
without the game. Turns the 40-minute end-to-end measurement into a bench that runs in under a
minute, deterministic, with Tracy and a screenshot diff. Motivation and the process it serves:
[performance-roadmap.md](performance-roadmap.md). Status: phase A (capture) implemented,
phase B (replay) not started.

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
a file named `trigger` dropped into `<dir>` while the game is parked. `--frame-capture-exit`
(default true) ends the process once the capture is flushed. All three are in
[settings.md](settings.md). One capture is enough; it is reused until the game's memory layout
changes (a new save or build that changes the scene).

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
5. **Dump emulator state.** Video-out registrations, the gfx and compute command-processor
   register files, the current flip request id.
6. **Screenshot** of frame N (`des-window.ps1 -Shot` equivalent inside the emulator, or the
   presenter's readback). *Not implemented.* The presenter has no readback path at all: its
   `Frame::image` is `eTransferSrc`, but nothing copies it to a host-visible buffer and
   `Presenter` exposes no entry point for it. A capture therefore has no `frame.png` or
   `frame.raw`; the manifest still carries `width` and `height`, taken from the video-out
   attribute group. Adding the readback belongs with phase B, which needs the same code to write
   the image after the last loop.

Taken during frame N (between `Done()` of N-1 and `Done()` of N), on the submit path:

7. **Record every submission** in the order the GPU thread *started processing* it, not the
   order of enqueue: type, queue, a copy of the command dwords and constant dwords, flip request
   id. Copying at enqueue is safe (the game must have the buffer ready at submit); ordering by
   processing start makes cross-queue `WAIT_REG_MEM` dependencies implicit, so replay can treat
   waits as satisfied.

Why snapshot at the end of frame N and replay frame N's submissions: the CPU-written inputs of
frame N (constants, SRTs, ring slots) are complete and not yet reused, and the GPU-produced
intermediates (culling results, indirect arguments, descriptors built by compute) regenerate on
replay. What this misses is documented under limits.

## Replay design

`kyty_emulator --replay <dir> [--replay-loops N] [--replay-image out.raw]`, plus the usual flags
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
4. **Loop.** For each loop: re-mark the recorded CPU-dirty pages
   (`BufferCache::MarkRegionAsCpuModified`, coalesced into runs and applied on the GPU thread),
   then feed the recorded submissions in file order (`Submit`, `SubmitCompute`, a fresh CPU flip
   where the capture recorded a `FlipPreparation`, `Done`), wait for the GPU thread to drain and
   for every flip queued on the video-out port to present, record wall time. Demon's Souls flips
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
   the render-path number and is free of the vblank thread's presentation cadence. Presentation is
   paced by the virtual vblank, so `ms/loop` is quantised to the vblank period and never falls
   below it; raise `--vblank-frequency` to shrink the quantum, or steer by `ms/loop gpu`. With Tracy
   attached the render thread's zone totals come from Tracy as usual.
6. **Image.** `--replay-image <path>` copies the last presented swapchain frame into a host buffer
   (`Presenter::ReadLastPresentedFrame`) and writes it as raw tightly packed 8-bit BGRA or RGBA,
   with a `<path>.json` sidecar carrying width, height, format and stride. There is no PNG encoder
   in the tree outside imgui's stb, so a script converts or diffs the raw file.

### Report format

```
replay: <capture dir>
  frame          <captured frame number>
  ranges         <n> restored, <n> skipped, <n> MiB committed, <n> ms
  memory         <n> pages, <n> MiB, <n> ms
  registers      <n> command processors, <n> bytes each
  video-out      <n> registrations, handle <h>, flip index <i>
  submissions    <n> records (<n> graphics, <n> compute, <n> flips)
  dirty pages    <n> in <n> ranges
  loops          <n> (<n> measured, loop 1 excluded)
  ms/loop        min <a>  median <b>  max <c>  jitter <d>%
  ms/loop gpu    min <a>  median <b>  max <c>  jitter <d>%
  total          <n> s
```

`replay-report.json` carries `capture`, `frame`, `loops`, `warmup_loops`, `restore_ranges_ms`,
`restore_pages_ms`, `restore_pages`, `restore_bytes`, `submissions`, `dirty_pages`, the full
`loop_ms` and `gpu_ms` arrays, and `summary` / `gpu_summary` objects with `min_ms`, `median_ms`,
`max_ms`, `jitter` and `total_ms`, so two runs diff without re-parsing the console.

### Gaps in the capture format the replay works around

`src/graphics/replay/frameCaptureFormat.h` is fixed for version 1; these are the places where the
replay has to infer what the capture does not record, listed so a version 2 can close them.

- **The shader map is not captured.** This is the one gap that would otherwise stop a replay dead.
  Every draw and dispatch resolves its shader through `ShaderMap` (src/graphics/shader/shader.cpp),
  which the game fills from `sceAgcCreateShader`; a replay runs no guest code, so the first
  dispatch exits with "is missing from ShaderMap". Every field of an entry, though, comes from the
  guest AGC shader header, and `sceAgcCreateShader` rewrites that header in place before the
  capture, turning its offsets into absolute pointers that land back at the same guest addresses on
  restore. So the replay scans the restored readable ranges for the header signature
  (`file_header` 0x34333231 followed by `version` 0x18), validates each candidate against the
  ranges it restored, and re-registers it. On the title-1 capture this recovers 4374 registrations
  in 1.5 s, with no address claimed by two headers. A `shaders.bin` stream of
  `{code_address, header_address}` pairs in format v2 would make the scan unnecessary.
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
  whose manifest is missing or edited is only caught by record-size arithmetic.

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
- **One machine, console session.** Replays are compared with replays, on the same build type.

## Phases

| Phase | Deliverable | Estimate |
| --- | --- | --- |
| A. Capture | flag, quiesce and flush, sparse memory dump, submission recorder in processing order, state dump, manifest; format in `frameCaptureFormat.h`. Done except the screenshot, which needs a presenter readback path | 1 agent-day |
| B. Replay | `--replay`, memory and state restore, feeder thread, loop timing and report, image readback | 1 to 2 agent-days |
| C. Validation | capture the parked Nexus on the current build; 100 loops; compare ms per loop with the 106 ms render-thread frame from `real3-self.csv`; A/B `--gpu-descriptors`; image diff | half a day |

Phase B status, September 11, 2026: `--replay` restores and feeds both existing captures. nexus-1
restores 7068 ranges (10,299 MiB) in 1.8 s, 455,615 pages (7119 MiB) in 5.1 s and 9353 shader
registrations in 2.6 s, then replays far enough to compile 125 compute, 1 vertex and 1 pixel
shader before it stops on the decommitted-range limit above. The loop, the report and the image
readback are exercised end to end on a synthetic capture (`frame_replay_tests --write-capture`).

Code goes under `src/graphics/replay/` (capture and replay), flags in
[settings.md](settings.md), the capture format and the report format in this file. Both flags
are opt-in; the game path is untouched when they are off.

## How it changes the process

Every item on the roadmap is measured in replay first. An agent's brief states the expected
milliseconds per frame, runs replay before and after, and stops if the number does not move.
One end-to-end run per integrated item confirms the game still boots, plays and renders; the
full A/B on the console session is reserved for milestones.
