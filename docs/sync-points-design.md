# Removing the render thread's sync points (proposal)

Status, September 12, 2026: steps 1 and 2 are landed behind `--gpu-prologue-table` and
`--gpu-fetch-side-effects`, both default off (below, "Step 1" and "Step 2"). Step 1 does what it
was for and cost 2.4 ms a loop when it was measured; step 2's shared prologue lowering takes 4 ms
off that configuration, so the 2.4 needs re-measuring against a table-off run on the same binary. Step 2
removes the read faults it was aimed at (89 a loop to 22, the flat reads gone entirely) and its
hard stop fired: the loop is 19 ms slower, its safety net fires 160 to 2,090 times a loop instead
of never, and one run in two died on a descriptor a producer left behind. The host-memory bench
removed the need for an artifact policy; what step 2 shows is that per-wave prologue evaluation is
the wrong shape for compute, not that the CPU has to keep reading those pages.
Builds on [performance-roadmap.md](performance-roadmap.md) item 1b and
[gpu-descriptor-fetch.md](gpu-descriptor-fetch.md).

## What the frame is

Item 1b measured it: about 97 times a replayed loop (110 a game frame) the render thread reads a
guest page that the GPU wrote, the read faults, and the fault handler downloads the page after
draining the queue. 26 distinct pages, read over and over between the compute passes that rewrite
them. In 70% of the faults the producer is the dispatch recorded immediately before the read; the
rest have retired but the download is still queued behind the open command buffer. Total: 26 ms
of a 72 ms replay loop, about the same in the game. The GPU is 72% idle for the same reason: CPU
and GPU take turns, ninety times a frame.

Everything tried against it while keeping the reads on the CPU has hit its floor:

| change | effect on `ms/loop gpu` |
| --- | --- |
| waiting only for the producer (`--gpu-readback-producer-wait`) | 0 (the producer is the whole open buffer) |
| periodic submits (`--gpu-submit-interval 16`) | -3.2 ms; the floor of ~17 ms of waits is the producers' own execution plus a round trip each |
| stage 1 + 1b (`--gpu-descriptors`, `--gpu-srt-reads`) | 0; the faults moved to the roots still on the CPU |

With stage 1 and 1b on, the remaining faults by root kind and program (`readback-faults.json` in
`_Runtime/_Diagnostics/replay/nexus-6/readback-gd/`, 19 loops, diagnostics on):

| what stays on the CPU | faults a loop | wait ms a loop |
| --- | --- | --- |
| flat reads and descriptor roots of compute programs with side effects (excluded from stage 1 by the side-effect rule; one shader, `0x74e4490fe7af0970`, is 48 of them) | ~60 | ~13 |
| image and sampler roots of pixel and vertex shaders (stage 3 territory; four faults of `0xb035b18a7b371e8f` cost 8.8 ms because each drains a long open buffer) | ~12 | ~17 |
| outside any evaluation | 5 | 1 |

The console never reads these pages on its CPU; its command processor and shader scalar unit
read them on the GPU, in order. The emulator has to do the same for the reads that matter, or
keep paying the round trips.

## Two pieces

### A. Compute programs with side effects evaluate their roots in-shader

Stage 1 excludes any program that writes a buffer or an image, because a fetched read that
misses (a page not yet in the BDA page table) yields zero, which for a producer is permanent
corruption (docs/investigations/gpu-descriptors-stage1-crash-2026-09-11.md). The fix is a miss
policy, not the exclusion:

- The prologue evaluates every root and flat read as stage 1 and 1b do. Roots are uniform, so a
  miss is uniform across the dispatch.
- On any miss the dispatch returns before its first side effect and sets its program's feedback
  bit and the page's fault bit. Nothing is written; the consumers of this pass read whatever the
  page held before, for one frame. The fault buffer registers the page and the next frame runs
  the pass normally.
- The render thread does not read the pages at all for these programs: no fault, no drain.

Cost: the same one-frame-late semantics stage 1 already has for consumers, now also for
producers at first touch of a page. In the parked scene no page is new after warm-up. In motion,
every newly streamed allocation the tables point into is one frame of stale output for the
passes that read it. Whether that is visible is the question the user has to decide on, and a
replay of a capture taken during movement (not yet recorded) would show the first-loop image
against the steady state.

Removes about 60 of the 97 faults a loop, about 13 ms.

### B. Image and sampler roots speculated, verified in-shader

Stage 3 (bindless images and samplers) is the full answer and is weeks of work. The short form:

- The CPU keeps, per program and per image or sampler root, the T# or S# it decoded the last time
  it evaluated that root (the same memo stage 1 keeps for the specialization tuple).
- On the GPU path the CPU binds from the memo without reading guest memory. The shader's prologue
  reads the real descriptor through BDA (the same lowering as the roots it already evaluates),
  compares it with the memoed one passed in shader data (eight dwords, or a hash), and on
  mismatch sets the program's feedback bit and renders with the memoed binding.
- The next draw of that program after a feedback bit goes through the CPU path, which drains
  once and refreshes the memo; then GPU path again. Same policy and plumbing as stage 1's
  specialization feedback.

Cost: one frame with the previous texture or sampler whenever a root's descriptor changes.
Textures change per object and per material, so in motion this is one frame of a wrong texture
at each transition of each program's binding; a program that changes every frame pins itself to
the CPU path (stage 1's rule) and keeps its drains.

Removes the remaining ~12 faults a loop, about 17 ms in the parked scene (they are the expensive
ones because each drains a long open buffer).

## What it buys

Replay: 71 ms a loop to about 45 with both pieces, plus the 3 ms of periodic submits once drains
are rare (the submits then only cost). Game: 86 ms to about 60, 16 to 17 FPS, before any of the
per-draw CPU savings the roadmap's items 3 to 5 were meant to bring, and it is the precondition
for those to matter: with the sync points gone the GPU can run ahead, and the render thread's
own time becomes the frame.

## Measurement

Both pieces are pure CPU-side removals of waits, which the replay prices fairly (the faults and
their waits reproduce exactly). Gate each behind a setting; judge by faults a loop and wait ms a
loop from `--gpu-readback-diagnostics`, `ms/loop gpu`, the steady-state image, and loop 1's image
against loop 40's for the first-touch behaviour. One end-to-end run at the end. A capture taken
while moving is needed before shipping either as a default.

## Decision needed

The artifact policy: one frame of stale or previous data at first touch (A) and at binding
transitions (B). Both are settings, default off, until a moving capture shows what they look
like.

## After the bench: the artifact-free variant

`bda_load_bench` with the fixture imported from host memory
(docs/investigations/bda-host-memory-bench-2026-09-12.md, `6274b72`): a dependent uniform load from
imported guest memory costs about 700 ns against 280 ns from VRAM and is never cached across a
barrier, but chains overlap almost perfectly across waves, so the prologue's reads for a whole
frame (20,800 events, 1.14 dependent reads, 4 lines each) add about 0.2 ms of GPU time. Body
reads (a shader streaming data) from the import are 300 to 400 times slower than from the mirror,
so the import can serve the prologue only, never the data.

That gives every mapped guest page a home the GPU can always read, and the miss policy above
becomes unnecessary:

- **The BDA page table follows the tracker.** Every mapped guest page has an entry by default
  pointing at its imported host memory. A page the GPU owns (written through a registered
  buffer, GPU-dirty) points at the VRAM mirror, which is where the GPU's writes are. A page the
  CPU owns points at the import, which is where the CPU's writes are, with no upload at all. The
  entry changes when the tracker's state changes, which is a page-table write, not a copy. The
  400 scans a frame, the second uploads of design P and the staging traffic for BDA reads
  disappear with the copies; buffers bound as data keep their mirror and its upload path.
- **A shader read can never miss**, so compute programs with side effects evaluate their roots
  in-shader with no skip policy and no one-frame-late output: a page nobody registered is simply
  read from host memory, as the console would.
- **The CPU stops reading GPU-written pages** for every root the shader evaluates, which is the
  sync points of item 1b. What remains on the CPU is the image and sampler roots (piece B above,
  now also miss-free: the shader verifies the memoed T# from memory it can always read), until
  stage 3 makes those bindless.

Needs, in order: imports at commit granularity with release before decommit
(`Common::VirtualMemory::Commit` / `Decommit`), the default page-table entries and their
maintenance from the tracker's state changes, the side-effect rule of stage 1 lifted, then piece
B. Each step is measured in replay by faults a loop, wait ms a loop and the image; the first
step alone can be judged by scans a loop going to zero with the image unchanged.

Expected at the end: the 26 ms of drains gone and the BDA sync gone, replay 71 ms a loop to
about 45, the game 86 to about 60. Effort: about a week of agent work in four measured steps.

## Step 1: the prologue table (landed, `--gpu-prologue-table`, default off)

Landed and measured September 12, 2026. **The hard stop fired**: both page tables miss nothing,
in loop 1 as well as in the steady state, the image is unchanged and the bookkeeping is free, but
`ms/loop gpu` on `--gpu-descriptors true --gpu-srt-reads true` is 80.8 to 81.5 against 78.4 to
78.8 with the table off. Three shapes of the design were built and measured to find out why; the
last is what the tree holds.

### What was built

A second BDA page table read **only** by the shader prologue -- the descriptor roots and
flattened SRT reads `--gpu-descriptors` and `--gpu-srt-reads` lower, which all go through
`EmitSrtFlatRoot`. The data table is untouched and still serves every body load, which is what the
host-memory bench requires: a body read from an import is 300 to 400 times a mirror read.

- **Entries.** A page a registered buffer covers holds that buffer's mirror -- the same address
  the data table holds, kept up to date before every draw that reads it by the BDA scan, and a
  VRAM read. Every other mapped page holds the page itself inside guest memory imported with
  `VK_EXT_external_memory_host`, so the read cannot miss. A page that is neither keeps today's
  answer, zero, which faults and reads zero. The tracker never moves an entry: only a buffer
  registering or retiring, and an import arriving or going away, change one. **4 entries a loop**
  on nexus-6 with `--gpu-descriptors true`, 41 with it false.
- **The import is one allocation.** Direct, flexible and pooled guest memory are all views of the
  guest backing store (`GuestBackingStore`, one pagefile-backed section with a writable alias of
  the whole 13.5 GiB), so the alias is imported once and each view's pages point into it at
  `alias + backing_offset`. This is not an optimisation, it is the only thing that works: the
  driver **refuses** an import that covers part of a guest mapping, refuses most mappings above
  about 16 MiB, and refuses everything past roughly 2.3 GiB of separate views, all with
  `VK_ERROR_OUT_OF_DEVICE_MEMORY`, while it takes the single 13.5 GiB alias without complaint.
  Importing the 4,980 committed ranges one by one covered 2,306 MiB of 9,852 and left 74 prologue
  misses a loop; the alias covers all of it in one allocation and leaves none. Private commits
  (stack, code, runtime; 63.7 MiB on nexus-6) are not views of the backing store, are imported on
  their own, are refused, and keep the mirror -- the GPU never reads them.
- **One binding, not three.** The prologue page table is the **upper half of the `BdaPagetable`
  buffer** (which is twice as long with the setting on) and a prologue miss counts itself in a
  dword past the `FaultBuffer` bitmap, which the host reads back with a four-byte copy; the bit it
  sets is the bit a data miss sets, so the page is registered exactly as it always was and the
  compaction shader still scans one bitmap. A stage therefore writes exactly the descriptors it
  wrote before, and the emitter has one lookup function taking the half as an argument rather than
  two functions.
- **When.** `GpuResourceManager::ImportPendingRanges`, at the top of `PrepareBda`, on the GPU
  thread with a command buffer recording. It runs when a guest map set the pending flag and once
  at start-up, and it works from the kernel's committed ranges and backing views rather than the
  GPU's mapped set, because a replay commits gigabytes before the GPU thread exists.
  `GpuResourceManager::UnmapMemory` releases before the guest decommits: it clears the entries,
  defers the Vulkan objects on the scheduler's tick, and the `Finish` plus `WaitPriorityOperations`
  it already did for the unmap runs them before returning.
- **Maintenance.** `ChangeRegister`, both ways, and the import queue the pages whose entry changed
  into one pending map, coalesced by page; `PrepareBda` records them as runs of consecutive pages
  through the staging ring, the same mechanism and ordering as the data table's own writes.
- **The setting** carries into `ShaderInfo::gpu_prologue_table`, so the binding layout, the SPIR-V
  validator and the emitter agree, and a module compiled with it off is what it always was. The
  driver's pipeline cache is keyed on the SPIR-V, so nothing else needs a key.

### Measured, nexus-6, 40 loops x 2 repeats, `-Image`

`_Runtime/_Diagnostics/replay/nexus-6/runs-prologue-final/` is the build in the tree.

| `ms/loop gpu` | min run 1 / 2 | median run 1 / 2 |
| --- | --- | --- |
| `--gpu-descriptors false`, table off | 71.87 / 71.86 | 72.54 / 72.72 |
| `--gpu-descriptors false`, table **on** | 72.36 / 71.41 | 73.37 / 72.59 |
| `--gpu-descriptors true --gpu-srt-reads true`, table off | 78.44 / 78.84 | 79.78 / 80.44 |
| `--gpu-descriptors true --gpu-srt-reads true`, table **on** | 81.47 / 80.83 | 82.49 / 82.07 |

| per loop, `--gpu-descriptors true --gpu-srt-reads true` | table off | table **on** |
| --- | --- | --- |
| prologue-table misses | - | **0** (0 in loop 1) |
| data-table fault pages | 12 (702 to 729 in loop 1) | **2** (1,110 to 1,113 in loop 1) |
| prologue entries written | - | 4 |
| read faults / device wait in them (`--gpu-readback-diagnostics`) | 89.6 / 28.7 ms | 89.6 / 30.8 ms |
| drains / GPU-range syncs | 14 / 10,248 | 14 / 10,248 |
| image against `reference.png` | R 9.8 G 7.8 B 4.1 | R 9.8 G 7.8 B 4.1 |

Imports: **71 allocations, 13.5 GiB, 1.37 s**, one of them the backing alias and 70 private
commits; 4,899 views recorded against the alias; 10 refused (63.7 MiB). Loop 1's presented frame
is byte-identical to loop 40's in every configuration, so nothing renders a frame late. The SPIR-V
validates (`--shader-validation true`).

### What the 2.4 ms is, and what it is not

**Not the bookkeeping.** With `--gpu-descriptors false` the table is still imported, filled and
maintained and **no shader binds it**, and the loop does not move (71.41 to 72.36 min against
71.86 to 71.87 off). The cost appears only when shaders read through it.

**Not the descriptor writes.** The first shape gave the prologue table and its fault buffer
bindings of their own, two more storage-buffer descriptors per stage per draw over 20,700 stage
events a loop. Folding both into the halves of the two bindings that already exist, so a stage
writes exactly what it wrote before, changed the cost by nothing measurable.

**Not the second fault-buffer scan.** The second shape gave the prologue half a bitmap of its own,
compacted by a second 32,768-workgroup dispatch on the fault-processing schedule, about 39 times a
loop. Replacing it with a counter dword and a four-byte copy also changed nothing measurable.

**Not the import reads, mostly.** With mirror-first entries the prologue reads a mirror for every
page a buffer covers, which is nearly all of them: only 2 pages a loop are still resolved through
the import against 12 that faulted with the table off. A diagnostic build whose prologue lookup
reads the **data** half -- the same addresses as the table-off build, with everything else of step
1 in place -- still cost 2.0 ms (`runs-prologue-diag2/`). So at most a few tenths of the 2.4 is
PCIe.

**What is left is the lookup itself.** Every guest read of the SRT flat program goes through it,
hundreds of thousands of times a loop, and step 1 makes it read a table selected at run time
instead of a fixed one. Two functions cost 2.6 ms, one function with a half argument costs 2.4,
and the same function reading the same half as before costs 2.0: the shape of the module is worth
about 2 ms whatever is done to it, and the remaining 0.4 is the two integer operations the half
adds. The rest of the 26 ms this path is meant to remove has not been touched yet.

**Two variants that were tried and are not in the tree.** Entries following the tracker -- the
import by default and the mirror only for a page the GPU owns, which is what the design called
for -- costs **5.1 ms**, the extra 2.7 being the prologue reading imported memory for every
CPU-owned page; it is the shape the later steps want, because it needs no upload, and this is its
price. A soft fault, where a page resolved through the import also records itself so the host
registers it and the next frame reads a mirror, costs **4.0 ms**: the recording is paid on every
read of an unregistered page and the pages churn.

### Where that leaves the path

Step 1 does what it was for: **neither page table misses**, in loop 1 as well as in the steady
state, with the image unchanged and item 1b's counters unchanged. It costs 2.4 ms a loop, against
the 1 ms allowed, and that cost is in the one place nothing here can remove: the prologue's
page-table lookup, which every root and every flattened read makes. Step 2 removes the 26 ms of
drains item 1b measured, so the trade is still heavily in favour on paper -- but the 2.4 ms is
real and the decision is whether to spend it now or to build step 2 far enough to see the return.

### Commands

```
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '.\_Build\replay-des.ps1' `
    -Capture '_Runtime\_Diagnostics\replay\nexus-6' -Loops 40 -Repeats 2 -Image `
    -Configs '--gpu-descriptors false --gpu-prologue-table false', `
             '--gpu-descriptors false --gpu-prologue-table true', `
             '--gpu-descriptors true --gpu-srt-reads true --gpu-prologue-table false', `
             '--gpu-descriptors true --gpu-srt-reads true --gpu-prologue-table true'"
```

`replay-des.ps1`'s table carries `bda miss`, `pro miss`, `pro l1` and `pro write`, and the replay
report carries `bda_prologue_fault_pages_per_loop`, `bda_prologue_fault_pages_loop1`,
`bda_prologue_entry_writes_per_loop` and the `guest_import_*` totals.

Earlier shapes, for the numbers above: `runs-prologue/` and `runs-prologue-ab/` (tracker-following
entries, separate bindings), `runs-prologue-mirrorfirst/` (mirror-first, separate bindings),
`runs-prologue-v3/` (mirror-first, folded bindings, two functions), `runs-prologue-v4/` (soft
fault), `runs-prologue-diag2/` (the prologue lookup reading the data half).

## Step 2: compute roots in-shader (landed, `--gpu-fetch-side-effects`, default off)

Landed and measured September 12, 2026. **The hard stop fired.** The faults go where they were
meant to -- 89 a loop to **22**, with the flat reads gone entirely -- but the loop is **19 ms
slower**, the safety net fires **160 to 2,090 times a loop** instead of never, and one of the two
40-loop runs died with a texture descriptor the host could not decode. The setting stays off and
the path needs the design change below before it is worth another run.

### What was built

`--gpu-fetch-side-effects` (effective only with `--gpu-descriptors true` and
`--gpu-prologue-table true`) lifts the side-effect rule of stage 1 for **compute** programs.

- **Marking.** `MarkGpuFetchBuffers` no longer returns early for a compute program with a written
  or atomic buffer or image, or with `uses_dma`. Its read-only buffer roots and its flattened SRT
  slots are marked exactly as any other program's; the written and atomic resources keep their host
  binding, because stage 1 covers read-only resources only. On nexus-6 that is **286 compute
  programs of 358**, carrying **29,308 lowered read slots** on top of stage 1b's 6,926.
- **The safety net.** `EmitGpuFetchDescriptors` collects the validity of every root and slot it
  lowers, votes it across the subgroup (`OpGroupNonUniformAny`; the roots read user data and memory
  at addresses derived from it, so the value is uniform by construction and the vote only keeps a
  divergence, if one were possible, on the conservative side), and on any invalid root returns from
  the entry point **before the body** -- before the program's first side effect -- after setting the
  program's `DescriptorFeedback` bit and counting itself in the dword behind the prologue table's
  miss counter. The renderer's existing policy does the rest: `ProgramCache::Get` sees `cpu_next`
  and materializes the next dispatch on the CPU, and eight mismatches pin the program to the CPU
  path for the session. A skipped dispatch is the documented one-frame-late policy applied to a
  producer: nothing is written, and its consumers read what the page held before.
- **One emitter for a whole prologue.** Necessary, not an optimisation. Every root was lowered on
  an emitter of its own, so a 250-root prologue re-emitted the SRT chain 250 times: **a quarter of
  a million SPIR-V words**, and the driver took 1 to 13 seconds over each such module. Sharing one
  `RootEmitter` across a prologue's roots (`EmitSrtFlatRoots`) reuses a step another root already
  produced, exactly as `FlatMachine` does within one run. It takes **36%** off the modules, and it
  is worth about **4 ms a loop** to step 1's own configuration: `--gpu-descriptors true
  --gpu-srt-reads true --gpu-prologue-table true` measures 76.6 to 78.3 ms here against the 80.8 to
  82.5 step 1 measured, and the replay's warm-up loop falls from 133 s to 23 s. The table-off side
  was not re-run on this binary, so step 1's 2.4 ms is not yet re-priced.
- **Compute clear recognizers.** `TryConsumeComputeMetaClear`, `ResolveComputeBufferFill` and
  `ResolveComputePatternFill` rejected every `gpu_descriptors` shader. They now reject a shader that
  fetches a descriptor **of its own**, which is the condition that matters: a program that lowered
  only its flattened reads still has every descriptor in the snapshot. The four clears a loop
  nexus-6 consumes survive the setting either way (title-2's two likewise); 1,244 dispatches a loop
  are refused at the gate that were never consumed before.
- **Counters.** `gpu_fetch_side_effect_skips_per_loop` and `_loop1`, `compute_clears_per_loop` and
  `compute_clears_refused_per_loop` in the replay report and in `replay-des.ps1`'s table; one
  console line per marked program with its fetched buffers, lowered slots and module size, and one
  per compute pipeline the driver took over 250 ms to compile.

### Measured, nexus-6, 40 loops x 2 repeats, `-Image`

`_Runtime/_Diagnostics/replay/nexus-6/runs-step2/`. The driver pipeline cache was warm (it is new,
see [frame-replay.md](frame-replay.md)); every configuration ran on the same binary.

| `ms/loop gpu` | min run 1 / 2 | median run 1 / 2 |
| --- | --- | --- |
| `--gpu-descriptors false` (setting on, inert) | 72.37 / 71.53 | 73.68 / 73.06 |
| `--gpu-descriptors true --gpu-srt-reads true --gpu-prologue-table true`, setting **off** | 76.58 / 75.50 | 78.34 / 78.27 |
| the same, setting **on** | **EXIT** / 94.55 | **EXIT** / 97.33 |

| per loop, `--gpu-descriptors true --gpu-srt-reads true --gpu-prologue-table true` | setting off | setting **on** |
| --- | --- | --- |
| read faults / device wait in them | 89 / 36.3 to 37.6 ms | **22** / **62.8 ms** |
| safety-net skips | 0 | **160** (2,090 a loop over 4 loops, before the programs pin themselves) |
| prologue-table misses | 0 (0 in loop 1) | 0 (0 in loop 1) |
| data-table fault pages | 0 (1,162 to 1,176 in loop 1) | 95 (2,393 in loop 1) |
| prologue entries written | 2 to 3 | 98 |
| submissions | 190 | 123 |
| compute clears consumed / refused | 4 / 0 | 4 / 1,244 |
| image against `reference.png` | R 9.8 G 7.8 B 4.1 | R 9.8 G 7.8 B 4.1 |

The image number is the mean absolute difference of the presented frame; it does not move, which is
worth knowing but is not a clean bill of health -- 160 skipped dispatches a loop and the EXIT below
say otherwise. `--gpu-descriptors false` is unchanged by the setting in every number, as it must be.

`KYTY_DEBUG_SRT_STATS=1`, 6 loops, setting on: **80 feedback bits**, **68 dispatches** routed to the
CPU path by one, and **0 programs pinned** -- the readback coalesces a program's bits into one, so
eight mismatches are never reached and a program that skips goes CPU, GPU, skip, CPU, for as long
as the capture runs. 466 programs lower reads, 43,862 slots in the shader against 380 left on the
host. (That run's `ms/loop` is not comparable: the stats collector costs render-thread time.)

**The EXIT.** Run 1 of the setting-on configuration died in `descriptors.cpp:618`:

```
unsupported texture mip view: base=3 last=11 levels=1 max=0 type=9 tile=9 class=1 numeric=1
dimension=3 mip_mode=0 read=1 written=0 dwords=11af1000,cb500000,81ffc1ff,909b3fac,0,0,0,0
```

A T# walked out of guest memory that no consistent texture describes. Run 2 of the same
configuration, and the 20-loop diagnostics run, did not hit it. It is the failure mode the
side-effect rule existed to prevent, one level removed: a producer that skipped, or that read a
page the data table does not map, leaves a descriptor block that a later draw walks on the CPU.

### The remaining faults, and where they are

`--gpu-readback-diagnostics true`, 20 loops, setting on
(`runs-step2/readback-faults-se-true.json`):

| root kind | faults a loop | ms a loop |
| --- | --- | --- |
| descriptor source | 16.1 | 12.1 |
| outside any evaluation | 6.0 | 6.7 |
| **flat read (`srt_reads`)** | **0** | **0** |

| shader | stage | faults a loop | ms a loop |
| --- | --- | --- | --- |
| (no evaluation) | - | 6.0 | 6.7 |
| `0xb035b18a7b371e8f` | ps | 4.0 | 3.1 |
| `0x0bc3409287149ce2` | cs | 1.0 | 2.8 |
| `0x560c7a298b6a497b` | ps | 1.0 | 2.3 |
| `0xd669aa446e6b6137` | vs | 2.0 | 2.0 |
| `0x9feda52abefba1d5` | ps | 2.0 | 1.0 |

Item 1b's 79.9 flat-read faults a loop are **gone**, and `0x74e4490fe7af0970`, which was 48 of
them, no longer appears at all. What is left is what the design said would be left: the image and
sampler roots of pixel and vertex shaders (piece B, stage 3) and the reads outside any evaluation.
This is the number step 3 should be sized from.

### Why it is not worth switching on, and what has to change

- **The safety net fires, and nothing converges.** It should have been 0 in this capture and it is
  160 to 2,090 a loop, with 0 programs pinned: the feedback readback coalesces a program's bits, so
  the eight-mismatch pin never trips and the program alternates between the two paths forever.
  The prologue page table misses **nothing** (0 a loop), so these are not unmapped pages: they are
  the flat program's own validity conditions -- a `ReadBuffer` whose byte offset is outside its V#'s
  `num_records`, or an address past the page table's 40-bit space. The host does not fail on those,
  because `MaterializeSnapshot` evaluates only the sources the plan's resource control flow marks
  **active** for that dispatch, while the prologue evaluates every marked root unconditionally. One
  garbage root the dispatch would never have used aborts the whole dispatch. **That is the design
  error to fix first**: the validity that guards a side effect has to be the validity of the roots
  the dispatch actually uses, not of every root the prologue evaluated.
- **The loop is 19 ms slower** (78.3 to 97.3 median), and the device wait inside the remaining
  faults *rises* from 36 to 63 ms: four times fewer drains, each waiting on a queue of much heavier
  dispatches. The prologue of a marked program evaluates up to 287 SRT chains, per wave, where the
  host evaluated them once per dispatch. The reads themselves are cheap (the host-memory bench) but
  the multiplication by wave count is not, and that is what stage 1b's per-slot lowering becomes
  when the programs are compute.
- **The modules are large and the driver is slow over them.** 286 pipelines, a median of 61k SPIR-V
  words after sharing, 300 to 400 s of cold driver compile for one capture. Replay never had a
  driver cache (no title); it has one now, keyed on the capture, so a bench pays this once. The game
  would pay it once per shader per driver-cache generation, on the render thread, as first-encounter
  stutter.
- **Data-table faults are up** from 0 to 95 a loop (2,393 in loop 1): a `gpu_fetch` resource is
  never `ObtainBuffer`ed, so its pages are registered by the fault path rather than by the bind, and
  a body read of an unregistered page still reads zero. For a consumer that is one frame late; for a
  producer it is a wrong value stored, and it is a second candidate for the EXIT above.

The shape that would answer all four is not this one: evaluate a dispatch's SRT **once**, on the
GPU, into the buffer the shader already reads (`FlattenedSrt`), instead of once per wave in every
prologue -- a small compute pass per submission that fills the flattened reads for the events in
it. The host stops reading the pages either way, the work stays O(1) per dispatch instead of
O(waves), and a root's validity can be decided where the host decides it, against the plan's
active-source control flow.

### Commands

```
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '.\_Build\replay-des.ps1' `
    -Capture '_Runtime\_Diagnostics\replay\nexus-6' -Loops 40 -Repeats 2 -Image `
    -ExtraArgs '--replay-timeout 900000' `
    -Configs '--gpu-descriptors true --gpu-srt-reads true --gpu-prologue-table true --gpu-fetch-side-effects false', `
             '--gpu-descriptors true --gpu-srt-reads true --gpu-prologue-table true --gpu-fetch-side-effects true', `
             '--gpu-descriptors false --gpu-fetch-side-effects true'"
```

`--replay-timeout` is needed: a measured loop is allowed 2 s by default and the first loops of the
setting-on configuration are far past that. Artifacts:
`_Runtime/_Diagnostics/replay/nexus-6/runs-step2/` (the bench, the images, the diagnostics run and
`readback-faults-se-true.json`) and `_Runtime/_Diagnostics/replay/title-2/runs-step2/`, where the
setting is neutral (median 28.5 / 28.2 off against 27.8 / 28.1 on, 6 faults a loop either way, no
skips, no crash).
