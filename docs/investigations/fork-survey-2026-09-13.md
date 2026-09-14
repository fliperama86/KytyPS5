# Other forks and PRs against the drain problem, surveyed 2026-09-13

Question: before continuing our own sync-point work on `des-on-599`, is there a fork or open PR
that already lets the GPU run free of CPU waits and is worth taking?

## What the drain costs on des-on-599

Parked Nexus, local console, `_Build/e2e-compare.ps1`, this branch with three profiler zones
added (`BufferCache::ReadMemoryOnGpu`, the drain inside `DownloadBufferMemory`,
`CommandScheduler::Flush`/`Finish`/`Wait`). 15 s Tracy capture, 302 frames, 20.16 fps, 49.6 ms a
frame. Capture and CSVs: `_Build/compare-20260913/des599-drains.*` in the main checkout.

| per frame | calls | time |
| --- | --- | --- |
| `ReadMemoryOnGpu` (CPU touched a GPU-written page) | 98 | 1.0 ms self |
| drains (`Finish` from the download path) | 92 | 11.0 ms |
| `Flush` (submissions outside the drain) | 128 | 1.5 ms self |
| `Wait` (stream-buffer wraps and the like) | 204 | 0.003 ms |

The drain wait distribution: median 58 us, p90 161 us, p99 3.0 ms, max 4.3 ms. Calls above 200 us
are 9% of the calls and 51% of the time. So half the 11 ms is ~85 round trips a frame where the
GPU has almost nothing left to do (submit the partial command buffer, wait for it, read back), and
half is the few reads that land right behind a big pass. Same ~90 drains a frame as main; PR 599's
native SRT compile made each evaluation cheap, it did not change what the CPU reads.

## Candidates

- **Almo7aya/KytyPS5 `97a895b`** (no PR, 2026-09-04, 171 commits behind): widens a readback to
  the owning buffer (up to 8 MB), marks buffers the CPU reads back as hot, records a host-visible
  shadow of every hot buffer at each flush and writes it back into guest memory when its tick
  completes, so a later poll does not fault. A read that overlaps a write since the last shadow
  still drains. Built for GTA III counters polled long after they were written. Our reads are of
  tables the immediately preceding dispatch filled (70% on main), so the shadow is either not
  recorded yet or its tick is the one we would wait for anyway; the round trip stays. Bound: the
  non-tail half at best, realistically 2 to 4 ms of 49.6. Adds a stale-data path of the kind that
  broke step 2 (tables refilled mid-frame).
- **#506 Techx3**: batches up to eight adjacent RELEASE_MEM labels into one submission, keeps
  textures resident until pressure, execution-only barriers for read-only dispatches, block reads
  of indirect image descriptors. On this branch: `Flush` self is 1.5 ms a frame and the drains
  add 92 submissions the batching does not touch; the texture GC never triggered in the log; the
  compute chains already skip the barrier inside a chain; the descriptor block read is moot with
  native SRT. Under 1 ms expected.
- **#483 Leclowndu93150**: skip a GPU sync when unmapping non-GPU memory. `Wait` totals 0.8 ms in
  the whole capture; nothing to gain here.
- **#562 MehmetCambaz**: finds CPU-dirty uploads from hint bits. Upload side, not the drain.
- **#500 TarkusR, #477 LordixDemon**: shader accuracy and boot fixes for Demon's Souls, not
  performance. #500's content is already in PR 599's base merge `f69e86d`, see
  [the TarkusR port note](tarkusr-port-2026-09-13.md); it is not a lead for the face-colour artifact.

## Decision

Take none of them now. The drain is a round trip per CPU read of GPU-written tables; only not
reading on the CPU removes it. That is our own path: in-shader evaluation of the compute roots
(step 2 of `sync-points-design.md`, faults 89 to 22 on main) or the PR's deferred GPU descriptor
fetch, now to be carried onto this branch. Ceiling on this scene: 11 ms of 49.6, about 26 fps
before anything else moves.
