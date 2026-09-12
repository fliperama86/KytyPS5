# GPU-side descriptor fetch

Design for moving per-draw shader resource table (SRT) evaluation from the render thread into the
shaders. Decided 2026-09-11 after the two spikes below. Everything here is gated by a runtime
setting, default off, until parity holds. Where this sits among the other remaining work, and
the frame budget it has to meet, is in [performance-roadmap.md](performance-roadmap.md).

## Why

Parked Nexus at 11.2 FPS: the render thread is the frame. Every one of ~8,700 draws and ~1,900
dispatches per frame reconstructs on the CPU what the console's command processor and shader
scalar unit do in hardware: walk the SRT through guest memory, decode the V#/T#/S# descriptors,
look the memory up in the buffer and texture caches, write and bind Vulkan descriptor sets.
`MaterializeResources` alone is 40% of the thread and runs every draw, every stage, even fully
cached. The profile is flat per-draw overhead times draw count. Shaving it further tops out near
20 FPS (see performance-handoff-2026-09-10.md). Removing it is the only path above 30.

## Spikes, both passed

1. GPU cost of guest-memory reads through the BDA page table
   (docs/investigations/bda-load-bench-2026-09-11.md, `bda_load_bench`). RTX 5090: at most 3.1x a
   bound SSBO read, and only on lane-uniform L1 hits where the whole delta is 0.05 ms per 200 M
   reads. Free (1.00x) on scattered reads. An in-shader 3-level SRT chase costs ~3 ps per
   invocation. The GPU is not the wall.
2. Lowering the flat SRT program to SPIR-V (`spirvEmitterSrt.cpp`, test
   `srt_flat_program_spirv`). All 46 `SrtFlatOp`s lower; 13 synthetic plans match the CPU
   evaluator bit for bit, including out-of-range, unmapped-page and address-underflow rejections.
   Known deviations: a shader cannot "fail" a root, so each root carries a validity bool and
   invalid roots read as zero; unmapped pages also set a fault bit; the page table covers 40 bits
   where the CPU masks 48; clean-context (specialization) reads have no shader equivalent.

## What exists to build on

- BDA page table and fault buffer: `DefineGetBdaPointer`, `LoadBdaDword`
  (spirvEmitterMemory.cpp), `BufferCache::ChangeRegister` writes one 64-bit device address per
  16 KiB page of every cached buffer, `FaultManager::ProcessFaultBuffer` compacts fault bits on
  the GPU and registers faulted pages through a deferred callback, no GPU drain. A faulted page
  becomes visible to shaders one or more frames later; the faulting read yields 0.
- Incremental dirty upload: `GpuResourceManager::PrepareBda` uploads dirty guest pages for
  registered buffers, gated by a generation pair, called per draw and per dispatch for `uses_dma`
  shaders.
- The flat SRT program (`CompileSrtPlan`) with one root per descriptor source. Roots are
  independent schedules, so the CPU can evaluate a subset of roots.
- Push constants (32 dwords shared by the pipeline's stages) carry the user-data SGPRs the shader
  reads directly; overflow goes to a per-draw `ShaderData` storage buffer.

## Facts that shape the design (from the code map)

- Buffer descriptor metadata is baked at shader compile time: `packed_stride`, `descriptor_format`,
  `descriptor_swizzle` form the shader specialization key. Typed loads decode the format into
  per-component loads at compile time. A GPU-side fetch cannot re-decode formats cheaply.
- Buffers are bound as one storage-buffer array; bounds come from `OpArrayLength` of the bound
  range, not from `num_records`.
- Vertex buffers are real Vulkan vertex input bindings, resolved per draw on the CPU
  (`AcquireVertexBuffers`), and the attribute layout is part of the pipeline key.
- Images need the CPU for view creation and layout transitions; samplers need a VkSampler.

## Design

### Stage 1: buffers in-shader

Per shader, at compile time, for every buffer resource whose descriptor source has a valid flat
root:

1. Emit the root in the shader prologue (uniform, once per invocation). Decode the V#: base
   (dword0 | (dword1 & 0xffff) << 32), stride, swizzle, `num_records`, dword3 fields.
2. Replace the storage-buffer access for that resource with BDA loads at base + byte offset. The
   byte offset is computed as today. Bounds: byte offset against `num_records` (raw) or record
   index against `num_records` (structured), matching the GCN rules the CPU applies now.
3. Speculate the specialization. Keep baking stride, format and swizzle exactly as today, chosen
   from the last tuple seen for this shader. Emit a compare of the runtime V# fields against the
   baked constants; on mismatch, set a bit in a per-shader feedback buffer (same shape as the
   fault buffer, indexed by a shader slot id). The CPU reads the feedback buffer with the fault
   buffer, re-materializes that shader on the CPU path for the next draw, and adds the tuple to
   the variant set. One frame renders with the wrong variant; measure how often (the stats dump).
4. Invalid roots (unmapped page, out-of-range) read zero. The fault bit maps the page for the next
   frame. First touch of any buffer therefore renders one frame late; the SRT stats run must show
   the tuple and address churn is low enough for this to be a startup effect only.

CPU side per draw: skip buffer roots in `MaterializeResources` (evaluate image and sampler roots
only), skip `FindBuffers`/`RebindBuffers` for those resources, call `PrepareBda` for every stage
(all shaders become `uses_dma`), keep user-data push constants, pipeline memo and the draw.

Fallback: a shader whose plan has a clean-context read, a user-data register outside the layout,
or an invalid root at compile time stays on the CPU path, per shader. The setting
`--gpu-descriptors` (default off) enables the whole thing; a second setting forces the CPU path
for a shader hash for bisecting.

Expected win: `MaterializeResources` buffer share, `RebindBuffers` (7%), the buffer cache lookups
and the GPU-drain readbacks of GPU-written SRT data that the CPU can no longer avoid reading
(indirect dispatch arguments stay). Sized by the stats dump.

### Stage 1 interface (landed, `--gpu-descriptors`, default off)

The pieces the recompiler and the renderer share, so the two sides can be built in parallel:

- `BufferResource::gpu_fetch`: set by the resource-plan side for a buffer resource whose
  descriptor source has a valid flat root with no clean-context read, no user-data register
  outside the binding layout, and whose resource is read-only (not `written`, not `atomic`) in
  stage 1. The emitter evaluates the root in the shader prologue and routes the resource's loads
  through `LoadBdaDword`. The renderer binds nothing for it and skips its root on the CPU.
- `ShaderInfo::gpu_descriptors`: true when any buffer has `gpu_fetch`. The binding layout then
  carries `BdaPagetable`, `FaultBuffer` and `DescriptorFeedback`, the module uses the physical
  addressing model, and the renderer calls `PrepareBda` before every draw or dispatch of the
  program, exactly as for `uses_dma`.
- `DescriptorBindingKind::DescriptorFeedback`: a storage buffer of u32 bits, one bit per program
  feedback slot. The shader compares the runtime V# fields it speculated on (`packed_stride`,
  `descriptor_format`, `descriptor_swizzle`) against the baked constants and sets its slot bit on
  any mismatch. The renderer owns the buffer, allocates slot ids per program-cache entry, and
  reads the bits back on the fault-buffer schedule.
- `BindingLayout::feedback_slot_dword`: the shader-data dword (push constants or the `ShaderData`
  overflow buffer, same as user-data registers) where the renderer packs the program's slot id.
  `NoFeedbackSlot` when the stage has no `gpu_fetch` resource. `ShaderDataDwords()` covers it.
- Renderer policy on a feedback bit: the next draw of that program materializes on the CPU path,
  which yields the new tuple and selects or compiles the matching variant; the shader then
  resumes GPU fetch. More than eight mismatches for one program in a session pins it to the CPU
  path. First encounter of a program always goes through the CPU path, since the tuple is needed
  to compile.
- Setting: `--gpu-descriptors <t|f>` (`Config::gpu_descriptors_enabled`,
  `GpuDescriptorsEnabled()`). Off: no resource gets `gpu_fetch`, nothing else changes.

### Stage 1 renderer (landed)

Where each piece of the host half lives.

- Feedback buffer: `DescriptorFeedback` (`host_gpu/renderer/cache/descriptorFeedback.{h,cpp}`),
  owned by `BufferCache` beside `FaultManager`. 65,536 slots, 8 KiB of device-local u32 bits.
  `GetBuffer()` zeroes it on the first bind, so a session with the setting off records nothing for
  it. `CommitBindings` (`pipeline/descriptors.cpp`) binds it for every stage whose layout declares
  `DescriptorFeedback`, exactly where `BdaPagetable` and `FaultBuffer` are bound; the compute path
  goes through the same function.
- Readback: `BufferCache::ProcessDescriptorFeedback` from
  `GpuResourceManager::RunGarbageCollector`, under the same `m_fault_process_pending` gate as
  `ProcessFaultBuffer`. The bits are copied into one area of an eight-deep host-visible ring and
  cleared on the GPU right behind the copy, so a bit is cleared exactly when it has been read; the
  scan runs in a `DeferOperation` callback once that submission retires. No GPU drain. The render
  context routes each set slot to `PipelineCache::ReportFeedbackSlot`.
- Slot ids: `ProgramCache::AssignFeedbackSlot` hands a dense id to a program-cache entry the first
  time one of its permutations compiles with `gpu_descriptors`, and `feedback_slots` maps the id
  back to the entry (the map is node based, so the pointer is stable and nothing is erased). A
  program that cannot get a slot has no way to report a mismatch and is pinned to the CPU path.
  The id travels to the draw on `ShaderStageRuntime::feedback_slot` and is packed into shader data
  at `bindings.feedback_slot_dword` by `RenderExecutor::PackFeedbackSlot`, called from
  `PrepareBindings` and again from `RebindBuffers`, whose memory-offset fill covers the same dword.
- Per-draw policy: `ProgramCache::Get`. The entry keeps `last_specialization`, `cpu_next`,
  `mismatches` and `pinned_cpu`. A draw takes the GPU path when the entry has `gpu_descriptors`,
  the setting is on, there is a last specialization and neither `cpu_next` nor `pinned_cpu` is
  set; `MaterializeResources` then gets a `GpuFetchOverride` naming the `gpu_fetch` buffers and
  the tuples to keep, so the same permutation is selected whatever the guest has since written to
  the SRT. Any other draw materializes as before and republishes `last_specialization`.
- Bindings: `FindBuffers` and `RebindBuffers` skip `gpu_fetch` resources entirely (no descriptor
  decode, no `FindBuffer`, no `ObtainBuffer`, no dirty upload) and bind the buffer cache's
  16-byte `NULL_BUFFER_ID` dummy in their descriptor slots. The shader never reads those slots.
- `PrepareBda` runs before a draw or dispatch when any bound stage has `uses_dma` **or**
  `gpu_descriptors` (`PrepareGraphicsBindings`, `RenderExecutor::DispatchDirect`). The two compute
  fill recognizers (`ResolveComputeBufferFill`, `ResolveComputePatternFill`) reject
  `gpu_descriptors` shaders: they read the snapshot descriptors, which a GPU-fetch shader no
  longer takes its addresses from.
- Counters: `KYTY_DEBUG_SRT_STATS` gains `gpu_fetch_draws`, `gpu_fetch_dispatches`,
  `gpu_fetch_cpu_first`, `gpu_fetch_cpu_feedback`, `gpu_fetch_cpu_pinned`,
  `gpu_fetch_feedback_bits` and `gpu_fetch_programs_pinned` in the JSON summary, and one console
  line every 600 frames with the run totals and the window since the previous line.

### Stage 1 measured, September 11, 2026

Parked Nexus, Remote Desktop session, same build, 4 minute warm-up, 30 s sample:

| run | FPS | GPU busy |
| --- | --- | --- |
| `--gpu-descriptors false` | 10.85 | 27% |
| `--gpu-descriptors true` | 9.81 | 29% |

The GPU path was taken for about 9,300 draws per frame, with zero feedback mismatches and zero
pinned programs, and the picture is correct (`_Runtime/_Diagnostics/gpufetch/real3.png`). Compute
shaders are excluded: a fetched read that misses yields zero, which is one wrong frame for a
consumer but permanent for a producer, and the game's descriptor-building compute shader stored a
null V# that a later CPU walk died on
(investigations/gpu-descriptors-stage1-crash-2026-09-11.md).

The Tracy self-time profile of the steady state (`real3-self.csv`) says why it is slower, not
faster:

- `EvaluateRuntimeSourcesImpl` is still 26% of the render thread at 1.4 µs per stage event,
  unchanged. The flattened SRT scalar reads (`srt_reads`, about two thirds of the reads per
  event in the stats dump) still run on the CPU, and they share the pointer chase with the
  descriptor roots. Skipping the roots only saves the four adjacent V# dwords at the end of a
  chain that is walked anyway.
- `GpuResourceManager::SynchronizeBdaBuffers` is 10%, new: `PrepareBda` runs before every draw
  of a fetched program and the generation changes often enough for about 340 real scans per
  frame at 30 µs each.

#### Re-baselined on the build of September 11, 2026

The numbers above are from build `fffb4ce` (`real3-self.csv`). Stage 1 was measured again on the
build of `840f02d`, same scene, same Remote Desktop session, same protocol (five-minute warm-up,
30 s sample, then a 15 s Tracy capture with no other client attached, one configuration per launch,
closed through the window so the pipeline cache is written). Artifacts in
`_Runtime/_Diagnostics/replay/e2e-rebaseline/`, script `_Build/e2e-rebaseline.ps1`:

| run | FPS | ms a frame | GPU busy | CPU cores |
| --- | --- | --- | --- | --- |
| `--gpu-descriptors false` | **11.63** | 86.0 | 29% | 12.6 |
| `--gpu-descriptors true` | **9.67** | 103.4 | 30% | 12.6 |

The build is faster than `fffb4ce` (10.85 / 9.81) and the gap stage 1 opens is wider: **1.202
against 1.105**. Per frame, from the Tracy zone totals divided by the `Presenter::Present` count
(170 and 147 frames):

| zone | gd=false | gd=true |
| --- | --- | --- |
| `GpuResourceManager::SynchronizeBdaBuffers` | 3.98 ms, 21.2 calls, 187.6 us each | **10.68 ms, 399 calls, 26.8 us each** |
| `EvaluateRuntimeSourcesImpl` | 26.30 ms, 20 864 calls | 31.04 ms, 20 492 calls |
| `RenderExecutor::RebindBuffers` | 6.35 ms, 20 691 calls | 4.04 ms, 20 389 calls |
| `RenderCompute::PrepareBda` | 351.9 calls | 351.3 calls |
| `CpOpDispatchIndirect::SyncArguments` | 3.90 ms, 245.0 calls | 3.88 ms, 245.0 calls |

So the shape is unchanged: stage 1 saves 2.3 ms of `RebindBuffers` and pays 6.7 ms of BDA scan, and
`EvaluateRuntimeSourcesImpl` is 4.7 ms *worse* rather than unchanged. The scan is the item-2 target
and it is now measurable per frame without Tracy at all: a frame capture records every `PrepareBda`
call, whether it scanned and what it cost
([frame-replay.md](frame-replay.md), phase E), and the replay reproduces 462 of the 474 scans of the
captured frame.

What stage 1 needs to pay off, both foreseeable from the stats dump:

1. Move the flattened SRT reads in-shader as well: the same lowering over the `flat_reads`
   roots, the shader reading through BDA instead of the `FlattenedSrt` binding. Then the CPU
   evaluation for a fetched program is images, samplers and conditions only.
2. Make the per-draw dirty sync cheap: track dirtied pages as a small list instead of scanning
   dirty ranges against mapped ranges on every generation change.
   Superseded by [bda-sync-design.md](bda-sync-design.md): the scan's inputs are already small;
   the cost is the copy of freshly written guest memory on the render thread.

Projected from the profile: 26% + 10% of the thread down to a few percent, about 16 FPS in this
scene. Bring-up diagnostics (permutation log, CPU retry, fault ring, stub) are in the tree behind
the setting and are cheap to keep until the stage is settled.

### Stage 2: vertex fetch in-shader

The vertex shader reads vertex data through BDA using the V#s from its own roots, fetch-shader
style. Attribute formats are speculated like buffer formats. The pipeline loses its vertex input
state (fewer pipelines), `AcquireVertexBuffers` goes away. The index buffer stays CPU-bound (its
address comes from the draw packet, one `ObtainBuffer` per draw) or moves to the mesh path's
in-shader index read.

### Stage 3: images and samplers bindless

One descriptor-indexed array of image views and one of samplers (update-after-bind, partially
bound). The shader evaluates the T# root, hashes the 8 dwords, looks the hash up in a GPU-side
table to get the array index; a miss appends the T# to a request list and sets a fault bit. The
CPU drains the request list with the fault buffer, creates the view, inserts it. Same for S#.
Sampled images live in one layout (General); render-target to texture hazards are covered by a
barrier at every render-target switch, which the CPU still sees in the context registers.

After stage 3 the CPU does per draw: pipeline memo, index buffer, push constants, one draw call.
About 1 µs. Stage 4, merging consecutive same-pipeline draws into multi-draw indirect, then
follows naturally because draws no longer differ in bindings.

## Risks

- Speculation churn: if a shader's buffer layouts change per draw, stage 1 thrashes. Stats dump
  decides; the fallback is per-shader CPU evaluation.
- One-frame-late resources: first touch of a page or texture. Startup effect if churn is low.
- Every dword load does a page-table lookup. The bench says this is cheap; if a real shader shows
  otherwise, hoist one lookup per aligned 16 KiB span.
- Loss of the CPU's "absent descriptor" failure: draws with invalid roots now render with zeros
  instead of being skipped. Watch for new artifacts and keep the per-shader fallback switch.
- Guest addresses above 2^40 fail in-shader. Not seen in this game; the fallback covers it.

## Measurement protocol

Same as docs/reaching-the-nexus.md: parked Nexus, 100 s warm-up, 30 s sample, one run at a time.
A/B each stage against `--gpu-descriptors=false` on the same build. Record the feedback-buffer
mismatch count and fault count per frame alongside FPS.
