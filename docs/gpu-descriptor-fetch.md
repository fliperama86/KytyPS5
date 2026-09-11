# GPU-side descriptor fetch

Design for moving per-draw shader resource table (SRT) evaluation from the render thread into the
shaders. Decided 2026-09-11 after the two spikes below. Everything here is gated by a runtime
setting, default off, until parity holds.

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
