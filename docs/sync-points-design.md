# Removing the render thread's sync points (proposal)

Status, September 12, 2026: step 1 of the artifact-free variant is landed and measured behind
`--gpu-prologue-table` (below, "Step 1"); its hard stop fired, so the rest of the path awaits a
decision. The host-memory bench removed the need for an artifact policy.
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

Landed and measured September 12, 2026. **The hard stop fired**: the misses go to zero and the
image is unchanged, but `ms/loop gpu` regresses by 5.1 ms against the 2 ms the step was allowed,
and the regression is the GPU reading its descriptors over PCIe, not the bookkeeping. The design
below is what is in the tree; the decision it now needs is in "What the 5 ms is".

### What was built

A second BDA page table, the same shape as the data one (one 64-bit device address per 16 KiB
page, 512 MiB of VRAM), read **only** by the shader prologue. The data table is untouched and
still serves every body load, which is what the host-memory bench requires: a body read from an
import is 300 to 400 times a mirror read.

- **Entries.** The default entry for a mapped guest page is the page itself inside guest memory
  imported with `VK_EXT_external_memory_host`. A page the GPU owns (any byte of it claimed by
  `ObtainBuffer` with `is_written`, until the download that gives it back) points at the VRAM
  mirror instead, because that is where the GPU's writes are. A page that is neither imported nor
  GPU-owned falls back to the mirror when a buffer covers it and to zero, the fault path, when
  none does -- so an un-imported page behaves exactly as it does today.
- **The import is one allocation.** Direct, flexible and pooled guest memory are all views of the
  guest backing store (`GuestBackingStore`, one pagefile-backed section with a writable alias of
  the whole 13.5 GiB), so the alias is imported once and each view's pages point into it at
  `alias + backing_offset`. This is not an optimisation, it is the only thing that works: the
  driver **refuses** an import that covers a part of a guest mapping, refuses most mappings above
  about 16 MiB, and refuses everything past roughly 2.3 GiB of separate views, all with
  `VK_ERROR_OUT_OF_DEVICE_MEMORY`, while it takes the single 13.5 GiB alias without complaint.
  Importing the 4,980 committed ranges one by one covered 2,306 MiB of 9,852 and left 74 prologue
  misses a loop; importing the alias covers all of it in one allocation and leaves none. Private
  commits (stack, code, runtime; 63.7 MiB on nexus-6) are not views of the backing store, are
  imported on their own, and are refused -- the GPU never reads them, and they keep the mirror.
- **When.** `GpuResourceManager::ImportPendingRanges`, at the top of `PrepareBda`, on the GPU
  thread with a command buffer recording. It runs when a guest map set the pending flag and once
  at start-up, and it works from the kernel's committed ranges and backing views rather than the
  GPU's mapped set, because a replay commits gigabytes before the GPU thread exists.
  `GuestResourceManager::UnmapMemory` releases before the guest decommits: it clears the entries,
  defers the Vulkan objects on the scheduler's tick, and the `Finish` plus `WaitPriorityOperations`
  it already did for the unmap runs them before returning.
- **Maintenance.** `ChangeRegister` (both ways), the GPU claim in `ObtainBuffer` and the release in
  `DownloadBufferMemory` queue the pages whose entry changed into one pending map, coalesced by
  page; `PrepareBda` records them as runs of consecutive pages through the staging ring, the same
  mechanism and ordering as the data table's own writes. 656 to 672 entries a loop on nexus-6.
- **The shader.** `EmitBdaPointer` -- the one place `EmitSrtFlatRoot` reads guest memory, and so
  every descriptor root and every flattened SRT read -- resolves through `get_bda_prologue_pointer`
  instead of `get_bda_pointer`. Body loads keep `LoadBdaDword` and the data table. A miss still
  sets a fault bit and reads zero; the bit goes to the prologue table's **own** fault buffer, so
  its misses are counted apart. `ShaderInfo::gpu_prologue_table` carries the decision, so the
  binding layout, the SPIR-V validator and the emitter agree, and a module compiled with the
  setting off is byte-identical to before. The driver's pipeline cache is keyed on the SPIR-V, so
  nothing else needs a key.

### Measured, nexus-6, 40 loops x 2 repeats, `-Image`

`_Runtime/_Diagnostics/replay/nexus-6/runs-prologue-ab/` (the four-way A/B),
`runs-prologue/` (the first set, with the counters), `runs-prologue-diag/` (readback
diagnostics), `runs-prologue-mirrorfirst/` (the variant below).

| `ms/loop gpu` | min run 1 / 2 | median run 1 / 2 |
| --- | --- | --- |
| `--gpu-descriptors false`, table off | 73.08 / 72.32 | 74.89 / 74.00 |
| `--gpu-descriptors false`, table **on** | 74.77 / 72.17 | 77.90 / 72.97 |
| `--gpu-descriptors true --gpu-srt-reads true`, table off | 77.99 / 78.04 | 79.84 / 79.27 |
| `--gpu-descriptors true --gpu-srt-reads true`, table **on** | 83.06 / 83.20 | 85.08 / 84.53 |

| per loop | table off | table **on** |
| --- | --- | --- |
| prologue-table misses | - | **0** (0 in loop 1) |
| data-table fault pages | 12 (652 to 692 in loop 1) | 0 (1,173 to 1,188 in loop 1) |
| prologue entries written | - | 656 to 672 |
| read faults / device wait in them (`--gpu-readback-diagnostics`) | 89.6 / 28.73 ms | 89.6 / 30.83 ms |
| drains / GPU-range syncs | 14 / 10,248 | 14 / 10,248 |
| image against `reference.png` | R 9.8 G 7.8 B 4.1 | R 9.8 G 7.8 B 4.1 |

Imports: **71 allocations, 13.5 GiB, 1.41 s**, of which one is the 13.5 GiB backing alias and 70
are private commits; 4,899 views recorded against the alias; 10 imports refused (63.7 MiB of
stack, code and runtime memory the GPU never reads). Loop 1's presented frame is byte-identical
to loop 40's in every configuration, so nothing renders a frame late.

### What the 5 ms is

Not the bookkeeping. With `--gpu-descriptors false` the table is still imported, filled and
maintained -- 672 entry writes a loop -- and **no shader binds it**, and the loop does not move
(72.17 to 74.77 min against 72.32 to 73.08 off, inside the run-to-run spread). The whole cost
appears exactly when shaders start reading through it.

So it is the prologue's own reads, from imported host memory over PCIe instead of from the VRAM
mirror. The bench (docs/investigations/bda-host-memory-bench-2026-09-12.md) bounded that at
**0.17 ms a frame overlapped and 10.1 ms fully serialized**, and named the risk that decides
between them: a pipeline barrier drops the prologue's cache lines, and an imported host pointer is
the one allocation the driver will not keep in L2 across one. The frame has hundreds of barriers,
and the answer is **5.1 ms**, half way between the two bounds. The readback diagnostics say the
same from the other end: the same 89.6 read faults a loop wait 2.1 ms longer because the device is
slower.

A variant confirms it. Pointing the prologue entry at the **mirror whenever a buffer covers the
page** and at the import only for pages no buffer covers -- still miss-free, still image-identical,
but an upload is again needed for a prologue read of a CPU-dirty page -- costs **2.4 ms** instead
of 5.1 (80.18 / 80.89 min against 78.39 / 77.42 off, `runs-prologue-mirrorfirst/`). So about
2.7 ms is the import reads themselves and about 2.4 ms is the second table's own binding: two more
storage-buffer descriptors written per stage per draw, 20,700 stage events a loop, plus the extra
function in every module.

### Where that leaves the path

The step does what it was for: **a prologue read can never miss**, in loop 1 as well as in the
steady state, with the image unchanged and the 1b counters unchanged. What it does not do is come
for free, and the 5.1 ms has to be paid back by step 2 -- which removes the 26 ms of drains that
item 1b measured -- before the path is ahead. That is still a good trade on paper, and the
measurement above is the first real price for the import rather than a bench projection.

Three things a decision should weigh:

- **The second table's binding is 2.4 ms of the 5.1 and is avoidable.** Folding the prologue table
  and its fault buffer into the existing `BdaPagetable` and `FaultBuffer` bindings as two-element
  arrays, or dropping the separate fault buffer once the counter has done its job, removes
  descriptor writes rather than reads.
- **The import reads are about 2.7 ms and are the design.** They shrink only if the driver starts
  keeping imported host pointers in L2 across a barrier, which the bench already flagged as worth
  re-checking on a driver update.
- **The mirror-first variant is miss-free too**, for 2.4 ms instead of 5.1, and keeps the uploads
  the artifact-free path wanted to delete. It is the cheaper half of step 1 if step 2 turns out to
  need only "a prologue read never misses" and not "a prologue read never needs an upload".

### Commands

```
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '.\_Build\replay-des.ps1' `
    -Capture '_Runtime\_Diagnostics\replay\nexus-6' -Loops 40 -Repeats 2 -Image `
    -Configs '--gpu-descriptors false --gpu-prologue-table false', `
             '--gpu-descriptors false --gpu-prologue-table true', `
             '--gpu-descriptors true --gpu-srt-reads true --gpu-prologue-table false', `
             '--gpu-descriptors true --gpu-srt-reads true --gpu-prologue-table true'"
```

`replay-des.ps1`'s table now carries `bda miss`, `pro miss`, `pro l1` and `pro write`, and the
replay report carries `bda_prologue_fault_pages_per_loop`, `bda_prologue_fault_pages_loop1`,
`bda_prologue_entry_writes_per_loop` and the `guest_import_*` totals.
