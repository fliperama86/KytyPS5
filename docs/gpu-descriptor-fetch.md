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

### Stage 1b: flat reads in-shader (landed, `--gpu-srt-reads`, default off)

The second half of roadmap item 2. Stage 1 moved the buffer *descriptor* roots into the shader and
left the flattened SRT scalar reads (`ResourcePlan::srt_reads`, the `flat_reads` roots) on the
render thread, where `EvaluateRuntimeSourcesImpl` writes them into the `FlattenedSrt` storage
buffer the shader reads with `ReadConst`. Stage 1b lowers those too, with the same machinery.

**Marking.** `MarkGpuFetchBuffers` (ir/passes/GpuDescriptorFetch.cpp) now also fills
`ShaderInfo::gpu_read_slots`, one byte per flat slot. A slot is marked when `--gpu-srt-reads` is on
(which also needs `--gpu-descriptors`), the program has no side effects (the same rule stage 1
applies: not `uses_dma`, no written or atomic buffer or image), the slot is not in
`clean_flat_slots`, and `GpuFetchReadLowerable` accepts its `flat_reads[slot]` root by the same
criteria as a descriptor root -- valid, no clean-context read, no `GetShaderBase`, every user-data
register inside the binding layout, every result register produced by the schedule. A program with
any marked slot gets `info.gpu_descriptors`, so it carries `BdaPagetable`, `FaultBuffer` and
`DescriptorFeedback`, uses the physical addressing model and gets a `PrepareBda` before every draw,
**even when no buffer was marked**: a program can now qualify on its reads alone.

**Emitter.** `EmitGpuFetchDescriptors` (spirvEmitterGpuFetch.cpp) lowers each marked slot with
`EmitSrtFlatRoot` in the function prologue, narrows the single result to u32, selects zero when the
root is invalid -- exactly what an invalid descriptor root does, and the unmapped page it failed on
has already set its fault bit -- and parks the id in `EmitterState::gpu_read_values[slot]`. The
`ReadConst` case of spirvEmitterMemory.cpp takes that id when the slot has one and falls back to
the `FlattenedSrt` load otherwise. `IR::UsesFlattenedRuntime` (BindingLayout.h) is now the single
decision both `AllocateBindings` and the SPIR-V validator use: the binding exists only while some
slot is still evaluated on the host or an image needs the indirect search table, so a program with
every slot lowered and no indirect image loses the descriptor entirely and the renderer uploads
nothing for it.

**Renderer.** `EvaluateRuntimeSources` gains `skip_flat_slots` beside `skip_sources`; a skipped slot
is left zero in `snapshot.flattened_srt` and the evaluator never runs its root.
`GpuFetchOverride::flat_slots` carries the mask and `ProgramCache::Get` passes the override on
**both** paths -- the GPU-fetch path and the CPU path -- because a module compiled with in-shader
reads never loads those slots from the buffer, so the host never has to produce them whatever the
draw does with its descriptors. `specialization` is null on the CPU path, which keeps `skip_sources`
empty there, so the first-encounter and feedback behaviour of stage 1 is unchanged.

**What stays on the CPU, and why.** Clean slots: a clean-context read goes through the host's
specialization reader and has no shader equivalent. Slots of a program with side effects: the whole
program stays on the CPU path, as in stage 1. Slots whose root reads `GetShaderBase` or a user-data
register outside the layout. And the indirect-image search table, which is not a slot at all: it is
appended to the same buffer past the slots (`indirect_mapping_offset`), so any program with
`indirect_search_iterations` keeps the `FlattenedSrt` binding whatever its slots do. In the parked
Nexus this leaves **91 slots of 7,017** on the host across the 128 programs that lower anything,
1.3%.

**Stats.** `KYTY_DEBUG_SRT_STATS` adds `flat_read_slots` and `gpu_read_slots` per shader in the
JSON, `flat_read_slots_sum` / `gpu_read_slots_sum` in the summary, `gpu_read_programs`,
`gpu_read_slots_shader` and `gpu_read_slots_cpu` in the summary and in the 600-frame console line.

#### Stage 1b measured, September 12, 2026

**It does not pay, and the reason is not the reads.** Replay bench, nexus-6, 40 loops, two repeats
interleaved, `-Image`; reports in `_Runtime/_Diagnostics/replay/nexus-6/runs-1b/`:

| per loop | `gd=false` | `gd=true` reads off | `gd=true` reads **on** |
| --- | --- | --- | --- |
| `ms/loop gpu`, min of the two runs | **73.06** | **76.90** | **77.52** |
| `ms/loop gpu`, median | 74.83 / 75.09 | 81.62 / 80.74 | 80.21 / 80.59 |
| drains / GPU-range syncs | 14 / 10 248 | 14 / 10 248 | 14 / 10 248 |
| BDA scans of the recorded frame | 0.4 | 11.6 | 11.6 |
| image vs `reference.png` | R 9.8 G 7.8 B 4.1 | R 9.8 G 7.8 B 4.1 | R 9.8 G 7.8 B 4.1 |

The image is unchanged. The three raw dumps are not byte-identical, but neither are two runs of the
same configuration: two 40-loop runs of `gd=true` with reads on differ in 0.248% of pixels with a
maximum channel difference of 24, and reads on against reads off differ in 0.267% with the same
mean, so the difference is the replay's own run-to-run noise floor, not a change in the picture.
Loop 1's image is the same as loop 40's in every configuration (identical hashes), so nothing
renders a frame late.

**Tracy, per loop**, from the slope of the zone totals over four captures a configuration (two at 30
loops, two at 60; `tracy-1b/`). One 30/60 pair has about +-6 ms of noise on this zone -- the same
configuration differenced four ways spans 27.6 to 40.2 ms -- so the number below is the least-squares
slope through all four points, whose intercepts agree to 0.1 ms:

| zone, ms/loop (calls/loop) | `gd=false` | `gd=true` reads off | `gd=true` reads **on** |
| --- | --- | --- | --- |
| `EvaluateRuntimeSourcesImpl` | 29.82 (20 739) | 32.80 (20 739) | **32.53** (20 739) |
| `RenderExecutor::RebindBuffers` | 2.88 (20 598) | 1.82 (20 639) | **1.46** (20 512) |
| `RenderExecutor::FindBuffers` | 1.03 (20 248) | 0.60 (18 244) | **0.42** (19 800) |
| `RenderExecutor::PrepareBindings` | 1.11 | 1.28 | 1.26 |
| `RenderExecutor::CommitBindings` | 1.86 (11 148) | 2.52 | 2.27 |
| `MaterializeSnapshot` | 1.61 | 1.66 | 1.57 |
| `ProgramCache::MaterializeResources` | 0.51 (9 500) | 1.00 | 0.99 |
| `GpuResourceManager::SynchronizeBdaBuffers` | 0.14 (16) | 0.32 (462) | 0.30 (465) |

`EvaluateRuntimeSourcesImpl` falls by **0.8%**, against the 30% the hypothesis needed. What stage 1b
does buy is the `FlattenedSrt` upload: `RebindBuffers` and `FindBuffers` together drop 0.54 ms a
loop, which is the whole measured effect.

**The stats run** (`KYTY_DEBUG_SRT_STATS=1`, three loops, 33 frames, 62,034 stage events;
`runs-1b/srt-stats-20260912-033257.json`):

| | |
| --- | --- |
| programs with reads lowered | **128** of 493 shaders |
| slots lowered / kept on the host | **6,926 / 91** |
| flat slots an event | 21.47, of which **17.77** lowered |
| descriptor sources an event | 1.97 (1.72 buffer, 0.15 image, 0.09 sampler) |
| descriptor dwords an event | 8.46 |
| flat program instructions an event | 23.73, of which 21.52 memory reads |
| `gpu_fetch_draws` / dispatches | 28,428 of 28,500 / 0 |
| feedback bits / pinned programs | **0 / 0** |
| `gpu_fetch_cpu_first` | 133 |

So 83% of the slots the host used to evaluate are gone, with no feedback mismatch and no pinned
program, and the zone that evaluates them does not move.

**Why, measured.** `srt_evaluator_bench` prices the same skip synthetically (400,000 iterations a
shape, `--iterations=400000`). `body_slots` are slots no descriptor source consumes; `body_skip` is
the evaluator with those skipped, `all_skip` with every slot skipped:

| shape | sources | slots | walker ns | flat ns | body_skip ns | all_skip ns |
| --- | --- | --- | --- | --- | --- | --- |
| `typical-ps` | 14 | 80 | 7685 | 1079 | 1092 | 829 |
| `heavy-ps` | 28 | 160 | 22044 | 2528 | 2532 | 2054 |
| `nexus-mix` | 3 | 25 | 1097 | **228** | **153** | **116** |

`nexus-mix` reproduces the parked Nexus average above (2 buffers, 1 sampler, 13 body-only slots).
The skip works and is worth **33%** of the evaluator there -- and 0% on the shapes where every slot
is also a descriptor dword, because then the source root runs the same instruction anyway. But
228 ns is the *whole* evaluation of that shape, while the game and the replay spend **1.44 to 1.58
us** in this zone per stage event. Skipping 17.8 reads of 21.5 saves 75 ns of it.

The third row of the Tracy table says the same thing from the other end: `gd=false` evaluates every
descriptor source and every slot and costs **1.44 us** an event; `gd=true` skips 6.9 of the 8.5
descriptor dwords and costs **1.58 us**; adding the slot skip leaves it at **1.57 us**. The zone is
insensitive to how much of the plan it evaluates. That was read here as fixed per-call work --
plan-shape validation, the scratch vectors, the snapshot the caller swaps out -- and it is not:
measured zone by zone below ("Where the evaluator's 1.5 us goes"), that work is 149 ns of the
1 500, and the rest is the cache misses the flat program takes on its own plan, which the skipped
roots share. That is the same reason stage 1 did not pay, and it is why the projection in "What
stage 1 needs to pay off" was wrong: the flattened reads are two thirds of the *reads*, not two
thirds of the *cost*.

**What this leaves for item 2.** Nothing more to take out of the evaluation itself; the remaining
26% of the render thread is a per-event cost paid 20,739 times a loop, and most of it is cache
misses on the 493 plans the loop cycles (see below), so the lever is calling it less often (one
evaluation a program a frame instead of one a stage a draw, which also keeps the plan warm) or not
at all (stage 3, which removes the image and sampler roots, the last thing the host still needs
from the walk). The setting
is kept default off: it is correct, it is 0.5 ms a loop of `RebindBuffers` and `FindBuffers`, and it
costs nothing while off.

#### Where the evaluator's 1.5 us goes, September 12, 2026

Stage 1b left the zone itself unexplained: it costs 1.44 to 1.58 us an event whatever share of the
plan it evaluates, while `srt_evaluator_bench` runs the whole of a Nexus-shaped plan in 228 ns.
Two candidates, decided with numbers: **A**, fixed per-call overhead around the flat program (the
scratch vectors, the plan-shape checks, the snapshot the caller swaps out); **B**, the memory
latency of the data an evaluation touches, which does not shrink when fewer roots are evaluated.
The answer is **B**, and the cold data is mostly the *plan*, not the guest table.

**The call, split by zone.** `EvaluateRuntimeSourcesImpl` is now five zones: `SrtEval::Setup`
(plan checks and `FlatPlanUsable`), `SrtEval::Prepare` (the register lease, the `FlatMachine`, the
`active` vector and the control-flow walk), `SrtEval::Sources` and `SrtEval::Slots` (the flat
program over the descriptor roots and over the flat-read roots), `SrtEval::Writeback` (the three
swaps into the caller's vectors), plus `SrtEval::Walker` on the fallback path. Replay, nexus-6,
`--gpu-descriptors false`, four captures (two at 30 loops, two at 60; `tracy-zones/`). Tracy's
on-demand connect leaves one in-flight zone per capture holding the whole pre-connect interval, so
each zone's single maximum is dropped before the mean; what is left agrees across the four runs,
which is why these are per-call means and not a 30/60 difference:

| part of the call | ns a call | share | spread over the four captures |
| --- | --- | --- | --- |
| `SrtEval::Setup` | 26.3 | 1.5% | 26.0 - 26.5 |
| `SrtEval::Prepare` | 42.6 | 2.5% | 42.2 - 43.0 |
| `SrtEval::Sources` | **573.5** | 33.1% | 531 - 671 |
| `SrtEval::Slots` | **1 008.2** | 58.3% | 979 - 1 040 |
| `SrtEval::Writeback` | 12.2 | 0.7% | 12.1 - 12.4 |
| `EvaluateRuntimeSourcesImpl` self | 67.5 | 3.9% | 66.6 - 68.9 |
| total | 1 730.3 | | |

**A is 149 ns.** Setup, Prepare, Writeback and the parent's own self time together are 8.6% of the
call; the flat program's execution is 1 582 ns, 91.4%. The instrumented total is 1 730 ns against
1 440 ns uninstrumented because the five zones cost about 50 ns each while a Tracy client is
attached; they cost nothing when none is (`tracy::ProfilerAvailable()` is false), and the replay
bench confirms it: 40 loops x 2 of `gd=false` give 73.41 / 73.49 ms/loop gpu minimum against 73.06
for the build before them, with the image unchanged at R 9.8 G 7.8 B 4.1 (`runs-zones/`). The IR
walker is not involved: 61 of 20 739 calls a loop take it (0.3%), at 686 ns.

**The chase, counted where it happens.** `KYTY_DEBUG_SRT_STATS=1` now also counts, per evaluation,
the guest reads the flat program executes, their *dependent depth* (the longest chain of reads
whose address depends on an earlier read of the same evaluation), and the distinct lines and pages
they touch. Three loops, 33 frames, 62 034 events
(`tracy-zones/srt-stats-20260912-044138.json`):

| | |
| --- | --- |
| events / IR-walker fallbacks | 62 034 / **183** (0.3%) |
| guest reads an event | **21.51** |
| dependent depth an event | **1.14** - 0: 43.0%, 1: 0.4%, 2: 56.5%, 3: 0.1% |
| distinct 64-byte lines an event | **4.03** |
| distinct 4 KiB pages an event | 2.23 |
| distinct pages over the whole run | **474** (1.9 MiB) |

There is no deep pointer chase. A root is user data to one table read, or user data to a table to a
second table: depth 2 at the 56.5% mode and never past 3. The 21.5 reads of an event land in 4
cache lines, and the evaluator's entire guest working set for the frame is 474 pages, which fits in
L2. The events also split in two:

| | shaders | events | flat insts an event | reads an event |
| --- | --- | --- | --- | --- |
| programs that read nothing | 18 | 26 685 (43.0%) | 0.90 | 0 |
| programs that read | 475 | 35 349 (57.0%) | 40.96 | 37.76 |

So 43% of the events run a program of one instruction and must be nearly free, and the zone's
1.5 us mean is carried by the other 57% at about 2.6 us each: 41 instructions, 37.8 reads over
about 7 lines of guest memory, and roughly 50 lines of plan.

**The bench, cold.** `srt_evaluator_bench --cold` builds N copies of a shape's plan, gives each its
own page of one large guest image, and cycles them in a pseudo-random order so consecutive
evaluations touch unrelated lines. `--copies` sets the cycle; the `coldplan`, `coldguest` and
`coldboth` columns instead keep a 64-copy cycle and `clflush` the plan's own arrays, the guest
table, or both before every timed evaluation, minus a baseline loop that performs the same flushes
and no evaluation. `nexus-flat` is the dump's average program shape (21 reads in 44 instructions,
no address arithmetic); `nexus-mix` is the older shape with one arithmetic op a slot. Per call, ns,
from `srt-bench/cold-sweep.txt` and `srt-bench/cold-all-shapes.txt`:

| cycle footprint | copies | `nexus-flat` | `nexus-mix` |
| --- | --- | --- | --- |
| 8 KiB (L1) | 1 | **188** | **249** |
| 0.5 MiB (L2) | 64 | 206 | 273 |
| 4 MiB (L3) | 512 | 246 | 314 |
| 32 MiB (L3) | 4 096 | 323 | 501 |
| 128 MiB (past the 96 MiB L3) | 16 384 | **997** | **1 186** |
| 385 MiB (DRAM) | 49 152 | **1 159** | **1 433** |
| plan lines flushed, guest table warm | 64 | 956 | 1 068 |
| guest table flushed, plan warm | 64 | 296 | 378 |
| both flushed | 64 | 1 038 | 1 147 |
| L1, with the renderer's per-call vectors | 1 | 245 | 318 |

The same evaluation costs 188 ns with everything in L1 and 1 159 ns out of DRAM, a factor of six,
and the renderer's 1 582 ns of flat-program time sits at the cold end of that range. The flush
columns say where the cold bytes are: the plan is 956 of the 1 038 ns penalty and the guest table
296, because a program touches about 4 lines of guest memory against 40 to 60 lines of
`flat.insts`, `schedule`, `flat_reads`, `srt_reads` and the roots. Handing the evaluator fresh
result vectors every call, which is exactly what `MaterializeSnapshot` does, costs about 60 ns.

**Verdict: B, with one correction to how it was stated.** The fixed per-call work is 149 ns, a
twelfth of the call, so A cannot be it and there is nothing here worth pooling. The cost is memory
latency, but not of a three-to-five-deep dependent chase through cold guest pages: the dependent
depth is 1.14 and the guest working set is 1.9 MiB. What is cold is the *plan* -- 493 of them
cycled across a loop with the whole rest of the render thread in between, 40 to 60 lines of it an
event -- and none of that shrinks when roots are skipped. It is also why stage 1 and stage 1b moved
the zone by less than 1%: skipping a root removes an instruction from a contiguous array whose
lines are fetched anyway, and removes a read from a cache line the remaining roots still read. The
lever is not making one evaluation cheaper; it is running it less often (one evaluation a program a
frame instead of one a stage a draw), which reuses the same warm plan across a program's draws, or
removing the host's last consumer of the walk in stage 3.

**Corrected by the next section.** "What is cold is the plan" is what the bench's flush columns
say, and it is wrong for the renderer: the plan is warm there, because the caller walks it
immediately before the evaluation. Packing the plan into a dozen contiguous lines was built and
measured, and it buys 2 ms of a 73 ms loop; splitting the packed run prices the plan at 68 ns an
event against 1 205 ns of first touch on the guest SRT pages. The rest of this section stands: the
fixed per-call work is 149 ns, the chase is shallow, and skipping roots buys nothing -- because it
removes reads, not the cache lines they share.

Artifacts: `_Runtime/_Diagnostics/replay/nexus-6/tracy-zones/` (four CSVs, their logs and the stats
dump), `_Runtime/_Diagnostics/replay/nexus-6/srt-bench/` (bench tables),
`_Runtime/_Diagnostics/replay/nexus-6/runs-zones/` (the 40-loop bench and its image).

#### Packed flat program, September 12, 2026

The section above said the evaluator's 1.5 us is the memory latency of the *plan*, and that the
lever is its layout. A packed execution form was built to take that lever, and it is measured here
both ways. **It works in the bench and buys about 2 ms of a 73 ms replay loop, one fifteenth of what
the plan-cold model predicted -- and the same instrumentation shows why: in the renderer the plan is
not cold. The call waits on the first touch of the guest SRT pages.**

**The form.** `CompileSrtPlan` now also builds, per plan, one deduplicated topological schedule
covering every descriptor source and every non-clean flat read (`SrtPackedProgram`,
SrtFlatProgram.h). It is stored in one allocation, in execution order, **16 bytes an instruction**:
a 4-byte immediate, an opcode byte, a flags byte (argument count, clean read, wide immediate,
sign-extended immediate) and five 2-byte argument positions. 16 rather than 24 because the only
immediate that does not fit 32 bits is a 64-bit literal, which gets an escape into a side table
that no plan in the dump uses; positions are 16-bit because the largest plan in the dump executes
290 instructions. Arguments are positions in the schedule, so the value memo is a dense array
indexed by position, there is no `insts[schedule[step]]` indirection, and nothing has to be cleared
between runs. The source roots carry their `dword_count` and the read roots their `flat_offset`, in
the same blob, so an evaluation never touches the plan's `descriptor_sources` or `srt_reads`
either. The compile-time structures are unchanged and still feed the SPIR-V lowering.

Everything in the schedule runs. A root's validity is a failed bit on its result instead of an
abort: a failure sets its own position's bit, every instruction ORs its arguments' bits, and an
instruction with a failed operand is not executed -- which is what keeps a read off an address a
failed operand would have produced. Any failure hands the whole call back to the compile-time form,
which reproduces it with its diagnostics, so failures stay bit-identical and rare calls pay for
both. Three cases keep the compile-time form: resource control flow (its conditions are clean
context, and its inactive sources must not be evaluated at all), a clean slot inside an otherwise
packed plan, and **any call that skips a flat slot** -- see the regression below.

**The bench.** `srt_evaluator_bench --packed=0|1` measures both forms from one binary; the cold
table's flush columns flush both forms' arrays, so the columns differ only in which one runs. Per
call, ns, 200 000 iterations a row, the flush rows the median of six runs
(`srt-bench/packed-sweep.txt`):

| cycle footprint | copies | `nexus-flat` | packed | `nexus-mix` | packed |
| --- | --- | --- | --- | --- | --- |
| 8 KiB (L1) | 1 | 179.1 | **116.7** | 246.9 | **175.2** |
| 0.5 MiB (L2) | 64 | 201.9 | 127.7 | 275.4 | 184.3 |
| 4 MiB (L3) | 512 | 242.2 | 134.4 | 323.2 | 202.1 |
| 32 MiB (L3) | 4 096 | 327.3 | 170.0 | 484.1 | 247.4 |
| 128 MiB (past the L3) | 16 384 | 951.0 | **355.8** | 1 216.8 | **519.8** |
| 385 MiB (DRAM) | 49 152 | 1 155.4 | 607.9 | 1 419.6 | 762.1 |
| plan lines flushed, guest table warm | 64 | 881.2 | **352.1** | 958.2 | **417.2** |
| guest table flushed, plan warm | 64 | 295.7 | 224.9 | 379.4 | 299.9 |
| both flushed | 64 | 923.2 | 445.1 | 1 074.0 | 499.6 |
| L1, with the renderer's per-call vectors | 1 | 240.2 | 179.7 | 311.0 | 234.8 |

The gate this had to pass before the renderer was 50% off the past-L3 and plan-flushed rows with no
rise in the hot row: **-63% and -60% on `nexus-flat`, -57% and -56% on `nexus-mix`, with the hot row
down 35% and 29%**. The packed blob is 888 B (14 lines) for `nexus-flat`'s 44 instructions and
1 440 B (23 lines) for `nexus-mix`'s 77, against 40 to 60 scattered lines before. Prefetching the
whole blob before the memo is sized is worth 80 ns of the plan-flushed row and nothing in the cycle
rows, where the streamer already follows the contiguous walk. Every shape's checksum is identical
between the two forms, and the hot table moves with them: `typical-ps` 1 202 -> 916 ns,
`heavy-ps` 2 899 -> 2 236, `waterfall-ps` 1 230 -> 958, `control-flow-ps` 1 212 -> 1 200 (not
packed, by design).

**The replay.** nexus-6, 40 loops, two repeats, `-Image`, one binary switched with
`KYTY_SRT_PACKED` (`runs-packed-off3/`, `runs-packed-on4/`):

| `ms/loop gpu`, both runs | packed off | packed on |
| --- | --- | --- |
| `--gpu-descriptors false`, min | 73.31 / 73.93 | **72.02 / 70.45** |
| `--gpu-descriptors false`, median | 75.38 / 75.49 | 73.71 / 72.78 |
| `--gpu-descriptors true`, min | 77.07 / 77.16 | **75.14 / 74.94** |
| `--gpu-descriptors true`, median | 82.20 / 80.44 | 79.12 / 79.36 |
| `--gpu-descriptors true --gpu-srt-reads true`, min | 77.80 / 77.91 | 77.43 / 77.18 |
| image vs `reference.png` | R 9.8 G 7.8 B 4.1 | R 9.8 G 7.8 B 4.1 |

About **2 ms a loop** on the two configurations that evaluate everything, and nothing on the third,
where only the programs with no lowered slot are packed. The first version of the change packed
those too, and it **cost 1.8 ms a loop** (`gd=true --gpu-srt-reads true` at 79.55 / 79.74 min,
`runs-packed-on3/`): a sequential schedule cannot leave a skipped slot out, and what the host saves
by skipping one is its guest line. That is the first direct evidence that the guest lines, not the
plan, are what an evaluation waits for.

**Why it is only 2 ms, measured.** Tracy, nexus-6, per-call means with each zone's single parked
maximum dropped, two captures a configuration (`tracy-packed/`):

| ns a call, `--gpu-descriptors false` | packed off (30 / 60 loops) | packed on (30 / 60) |
| --- | --- | --- |
| `SrtEval::Setup` | 29.5 / 29.6 | 29.6 / 29.3 |
| `SrtEval::Prepare` | 41.5 / 42.8 | 43.6 / 42.4 |
| `SrtEval::Execute` (the packed run) | - | **1 482.8 / 1 438.8** |
| `SrtEval::Sources` | 533.6 / 535.4 | 50.0 / 53.1 |
| `SrtEval::Slots` | 972.6 / 1 033.7 | 82.5 / 83.6 |
| `SrtEval::Writeback` | 11.9 / 11.9 | 12.0 / 12.2 |
| `EvaluateRuntimeSourcesImpl` self | 69.5 / 68.4 | 77.3 / 75.4 |
| all parts | 1 580 / 1 640 | 1 638 / 1 599 |

The instrumented call is the same size either way: the zones cannot resolve a 2 ms a loop change
(30 ns a call) under their own 50 ns a zone. Two experiments inside the packed run do resolve it,
both on `--gpu-descriptors false`, 30 loops:

- **Run the whole packed program twice.** The second pass costs **+68 ns** (1 482.5 -> 1 550.8).
  Executing 24 instructions and 21 guest reads over warm data is 68 ns; the other 1 400 ns of the
  first pass is first touch.
- **Split the pass in two**: one pass that executes every instruction and answers each read from
  its own address without touching memory, then the real one. The first costs **68.0 ns** and the
  second **1 205.3 ns**. The first pass touches the whole packed program, so the plan's cold cost in
  the renderer is 68 ns; the guest pages are the 1 205.

So the model that produced this design is wrong in the renderer, and the bench is why: flushing a
plan's lines prices what the renderer never pays, because the caller walks the same plan
(`ProgramCache::MaterializeResources`, `MaterializeSnapshot`, `info.buffers`,
`materialization_sources`) immediately before the evaluation and leaves it warm. What is cold is the
guest SRT: 4 lines and 2.2 pages an event, spread over 474 pages, with the whole rest of the render
thread between two events. That also explains, at last, why stage 1 and stage 1b moved the zone by
less than 1%: **skipping roots removes reads, not lines**. The 21.5 reads of an event land in 4
lines, so dropping 17.8 of them leaves the same 4 first touches to pay.

What is left for this zone is therefore not its shape but its frequency: one evaluation a program a
frame instead of one a stage a draw (which would also make the guest lines warm across a program's
draws), or a prefetch of the SRT table issued when the draw's user data is known, long before the
evaluation reads it. Both are item 3 work, and both are now sized by the same number: 1.2 us of
first touch an event, 25 ms of a 73 ms loop.

Artifacts: `_Runtime/_Diagnostics/replay/nexus-6/srt-bench/packed-sweep.txt` (the bench sweep),
`.../tracy-packed/` (two captures a form for `--gpu-descriptors false`, one a form for `true`,
plus the twice and split experiments), `.../runs-packed-off3/`,
`.../runs-packed-on3/` (before the skipped-slot fallback) and `.../runs-packed-on4/` (the
40-loop benches and their images).

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

#### Scan breakdown, September 12, 2026

Step 0b of [bda-sync-design.md](bda-sync-design.md): `SynchronizeBuffer` was split into Tracy zones
(`SynchronizeBuffer::Track` for the tracker walk, `::Stage` for the staging reservation, `::Copy`
for the `memcpy` out of guest memory, `::Record` for the barriers and the `copyBuffer`), with
`MemoryTracker::Lock` around the region lock and `PageManager::Protect` around the re-protection
the tracker makes when it clears CPU-dirty state, `PageManager::ProtectCall` around the kernel call
inside it. One warm run of the parked Nexus with `--gpu-descriptors true`, same protocol as the
re-baseline (five-minute warm-up to `0 new pipelines`, 30 s sample, then a 15 s Tracy capture),
**10.03 FPS**, 28.7% GPU, 12.57 CPU cores, Remote Desktop session. Artifacts in
`_Runtime/_Diagnostics/replay/e2e-rebaseline/scan-breakdown/`, script `_Build/scan-breakdown-run.ps1`.
The zones themselves cost about 1.3 µs of the scan (26 of them per scan at the usual tens of
nanoseconds), which is the difference between the 28.55 µs below and the re-baseline's 26.8 µs.

These zones are all called from outside the BDA scan as well (`SynchronizeBuffer` runs for every
bind, the page manager re-protects on every guest write fault), so the aggregate CSV is not the
answer. The numbers below are the events that fall inside a
`GpuResourceManager::SynchronizeBdaBuffers` interval on the render thread, from the unwrapped
export (`gd-true-attribute.py`, output in `gd-true-scan-attribution.txt`); the capture holds 148
`Presenter::Present` and 53 211 scans.

| zone | ms a frame | calls a frame | µs a call | µs a scan |
| --- | --- | --- | --- | --- |
| `GpuResourceManager::SynchronizeBdaBuffers` | 10.27 | 359.5 | 28.55 | 28.55 |
| → `SynchronizeBuffer::Track` | 8.52 | 1342.7 | 6.35 | 23.70 |
| → → `PageManager::Protect` | **7.38** | 1342.5 | 5.50 | **20.53** |
| → → → `PageManager::ProtectCall` | 7.26 | 1342.5 | 5.41 | 20.20 |
| → → `MemoryTracker::Lock` | 0.35 | 1181.5 | 0.30 | 0.97 |
| → → `SynchronizeBuffer::Copy` | 0.43 | 1340.6 | 0.32 | 1.19 |
| → → `SynchronizeBuffer::Stage` | 0.02 | 1334.8 | 0.02 | 0.06 |
| → → `Track` itself (the bitmap walk) | 0.34 | — | — | 0.95 |
| → `SynchronizeBuffer::Record` | 1.08 | 1340.6 | 0.80 | 2.99 |
| → the scan itself (dirty set, intersection, buffer lookup) | 0.67 | — | — | 1.86 |
| `EvaluateRuntimeSourcesImpl` | 30.43 | 20 788.8 | 1.46 | — |
| `CpOpDispatchIndirect::SyncArguments` | 3.83 | 245.0 | 15.62 | — |

Tails: `PageManager::Protect` reaches 162.7 µs inside a scan and 350.1 µs anywhere in the process,
`MemoryTracker::Lock` 89.9 µs, a whole scan 2.42 ms. Across the whole process the re-protection is
5 268 calls and 18.5 ms of CPU a frame, 51% of it on the render thread; three quarters of that
render-thread share is inside these scans and guest write faults are the rest.

**The re-protection carries the scan.** A scan makes 3.7 `NtProtectVirtualMemory` calls at 5.4 µs
each, 20.5 µs of its 28.6 µs, and 98% of that is the syscall rather than the bitmap walk around it.
The `memcpy` the design document's hypothesis pointed at is 1.2 µs of a scan, 4%; the staging
reservation is 0.06 µs, the region lock 1.0 µs (no contention worth naming), the barriers and the
`copyBuffer` 3.0 µs, and the scan's own walk of the dirty set 1.9 µs. So the gap between the game's
26.8 µs and the replay's 4.1 µs is not freshly written guest memory: it is the cost of changing page
protection while the game's guest threads are running, which is a TLB shootdown to every core they
are on, against a replay where nothing else runs and the same call is nearly free. Design E (move the `memcpy` to a
helper thread) would therefore buy about 1.2 µs of the 28.6; what the scan needs is to stop
re-protecting pages it is about to see dirty again, or to batch the re-protection of a whole scan
into one call instead of 3.7.

#### Design P measured, September 12, 2026

Design P of [bda-sync-design.md](bda-sync-design.md) is implemented behind `--bda-async-protect`
(default off, [settings.md](settings.md)): the upload path clears a page's CPU-dirty bit, leaves it
writable (state U) and queues its region; a helper thread protects the batch, marks the pages
landed and moves the BDA generation once; the next scan or bind that covers a landed page uploads
it a second time; and every submission waits for the helper to have drained before its first draw.

**Step 1, what the helper will see** (`_Runtime/_Diagnostics/replay/nexus-6/analyse-protect.py`
over the capture's `dirty-events.bin` and `prepare-events.bin`; one warm frame, 4,039 CPU-dirty
marks, 474 scans):

| | 4 KiB tracker pages | 16 KiB guest pages |
| --- | --- | --- |
| distinct pages dirtied in the frame | **2,816** | 905 |
| marks a page, mean | 1.43 | 4.46 |
| marked once | 92.0% | 13.6% |
| marked twice or three times | 4.8% | 19.2% |
| marked four times | 0.7% | 57.6% (the four tracker pages of one guest page) |
| marked more than twelve times | 0.9%, max 178 | 2.2%, max 180 |

Grouping the marks by the scan that follows them gives 464 batches a frame, which is what one
helper batch would be: **8.55 tracker pages and 3.19 contiguous runs a batch**, so one call per run
covers 2.68 pages. The runs are short -- 81.9% are a single page, 95.5% are four pages or fewer,
2.3% are longer than eight (max 1,025). And pages come back quickly: of 1,223 intervals between a
page's successive marks, 5.8% fall inside the same batch, 30.5% one scan later and 33.7% two, so
**two thirds of a re-protected page is dirty again within two scans**. That is the case for design
P in one number: most of the protection the scan pays for is undone almost immediately.

**Step 2, the replay bench.** nexus-6, 40 loops, two repeats of each configuration interleaved,
`-Image`; reports in `_Runtime/_Diagnostics/replay/nexus-6/runs-p-nobump{,2}/`, the first variant in
`runs-p-final/`, the pre-change build's baseline in `runs-p-baseline/`, the helper-affinity A/B in
`runs-p-affinity/`. Per loop, of the shipped variant:

| | gd=true, off | gd=true, **on** | gd=false, off | gd=false, **on** |
| --- | --- | --- | --- | --- |
| render-thread protect calls, upload path | 926 (1,469 pages) | **0** | 316 (845 pages) | **0** |
| of which in written uploads, not deferred | -- | 0 | -- | 0 |
| helper protect calls (pages) | -- | 700 (3,263) | -- | 275 (2,409) |
| pages a helper call | -- | 4.66 | -- | 8.76 |
| helper batches | -- | 499 | -- | 132 |
| pages landed / uploaded a second time | -- | 2,445 / 2,242 | -- | 1,926 / 1,902 |
| boundary drains / waits / wait µs | -- | 39 / 1 / 15 | -- | 39 / 1 / 24 |
| boundary forced scans / their µs | -- | 39 / 1,270 | -- | 39 / 1,685 |
| draw-driven BDA scans | 462 | 458 | 16 | 12 |
| total scan time, ms a loop | 2.0 | 2.6 + 1.3 | 0.8 | 0.2 + 1.7 |
| `ms/loop gpu`, min of the measured loops | 76.9 | 82.1 | 72.5 | 75.0 |
| image vs reference | R 9.8 G 7.8 B 4.1 | **identical** | identical | identical |

(The `ms/loop gpu` medians of this batch are unusable -- the machine picked up background load
between sessions, jitter went from 0.15-0.50 to 0.55-0.77 and the *off* configurations moved with
it. `gpu min` did not: 76.4 to 77.1 for `gd=true` off and 72.2 to 73.2 for `gd=false` off across
all four sessions, so it is what the row above reports.)

**The generation bump per landing, and why it went.** The first implementation had the helper move
the BDA generation once per batch, as the design says, so that the next `PrepareBda` would scan and
re-upload the landed pages. That raised the scan count from 462 to **545** a loop with `gd=true`
and from 16 to 21 with `gd=false` -- 505 batches a loop, each asking for a scan. It is not needed:
any scan or bind that later covers a landed page uploads it anyway, and a draw of a submission may
only read what the guest wrote before that submission was enqueued, while everything a U page can
be stale by was written *after* its upload, which is during the submission's own processing. So the
bump was removed and the submission boundary, which already drains the helper, forces one scan
instead. The scan count comes back to 458 (and 12 with `gd=false`, below the 16 it does off,
because the boundary scan takes the dirty set the draws would have scanned for).

**It did not buy milliseconds, and the measurement says why.** The total scan time is the same
either way -- 3.84 ms a loop with the bump (545 scans), 3.87 ms without it (458 scans plus 39
boundary scans) -- because the cost is the 2,242 pages uploaded a second time, not the dispatch of
the scans that upload them. By `gpu min`, the no-bump variant is in fact 0.7 ms a loop *worse* on
`gd=true` (82.1 against 81.4) and 0.6 worse on `gd=false`, because a forced boundary scan runs on
every submission and each one sweeps a larger dirty set (32 µs with `gd=true`, 43 with `gd=false`).
It is kept regardless: it makes the correctness argument local and provable -- every page is
protected and clean before a submission's first draw, whether or not that submission ever calls
`PrepareBda` -- and it keeps the replay's scan count comparable to the capture's recorded 474,
which the bump variant's 545 did not.

The render thread makes **no** `NtProtectVirtualMemory` call in the deferred upload path, which is
what the design is for. One or two calls a loop remain in *written* uploads, which are not
deferred: `ForEachUploadRange(is_written = true)` claims the range for the GPU in the same call and
protects it at no-access either way, so deferring the CPU-side protection there would remove no
kernel call and would leave the guest free to write bytes the GPU is about to own.

The helper does **fewer** calls than the render thread did -- 690 against 926 with `gd=true` -- over
more pages, because a batch coalesces across the buffers of several scans, which the render thread
cannot do; with `gd=false` the batches are larger still (8.8 pages a call).

What the design costs, and the replay measures fairly: **2,242 pages a loop uploaded a second
time**, which is 1.9 ms more of scan work with `gd=true` and 1.1 ms with `gd=false`, and about
5.2 ms a loop in all. **In replay the setting is a loss**, exactly as
[bda-sync-design.md](bda-sync-design.md) predicts: a protect call costs about 1 µs there and 5.4 µs
in the game, so the replay pays the design's price without collecting its saving.

**The projection for the game.** The game's render thread makes 1,342 protect calls a frame inside
BDA scans at 5.41 µs, 7.26 ms, and about 3 ms more in the per-bind syncs outside them: about
**10.3 ms a frame of syscall latency** that design P moves to the helper. Against that it adds the
second uploads and the boundary scans the replay measures at **5.2 ms a loop** (`gd=true`), which
the game pays as well and probably pays more, because its copies are of freshly written lines
(5.4 µs a scan in the writer experiment, [frame-replay.md](frame-replay.md)). Net **about 5 ms a
frame off the render thread**, 103.4 ms to roughly 98, against the design document's estimate of
95. For `gd=false` the same arithmetic over a per-bind share that is most of 3.98 ms gives about
1.5 ms. Only the end-to-end A/B settles it, and it is the next step.

Off, the build measures as the one before it: `gd=true` 78.11 against 78.19 ms, `gd=false` 74.07
against 72.76, the second inside its own run-to-run spread of 1.1 ms and not visible in the scan
path (16 scans, 316 protect calls, 845 pages, 0.72 ms of scan time, all identical).

The helper runs on the guest CPU group, chosen by measurement: guest 83.25 ms/loop gpu against
render 85.42, for the reason the replay writer is slower on `render` -- that group holds the render
and presentation threads, and a helper there competes with them for the same logical processors.

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
