# Removing the render thread's sync points (proposal)

Status, September 12, 2026: proposal, not started. The host-memory bench (below) removes the need for
an artifact policy; awaiting a go.
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

