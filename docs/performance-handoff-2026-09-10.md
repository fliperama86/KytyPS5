# Demon's Souls performance handoff ? September 10, 2026

## Outcome

Two implemented optimizations raised the measured stationary Nexus result from **4.21 FPS to approximately 7.5 FPS (about 78%)** on this machine:

1. Cache completed BDA buffer synchronization until CPU dirtiness, cached-buffer topology, or mapped ranges change.
2. Use a per-call temporary memory arena for shader resource evaluators, preserving distinct normal, clean, and active-mask caches.

Three follow-ups documented at the end of this file (evaluation memo, pooled bindings, flat SRT
program) took the same scene to **10.20 FPS**, about 2.4x the starting point. Steady state at each
step: 4.21, 6.36 (BDA cache), 7.55 (arena), 7.88 (memo), 8.20 (pooling), 10.20 (flat program).

Both changes are built and tested. The experimental executable is running. This is a local experimental build, not a new published release. It remains slow, and the results do not establish performance in other areas or broad game stability.

The user requested all project notes in the repository and then requested this handoff. Ten authored documents were moved out of ignored build/old-checkout/runtime folders into `docs/`. Use [the documentation index](README.md) and [the performance report](demons-souls-performance.md). Add future notes here.

## Correct checkout and runtime

| Item | Location / value |
| --- | --- |
| Project folder | `C:/Users/dudu/Projects/KytyPS5` (source, `_Build`, `_Runtime`) |
| Branch | `demons-souls-workaround` |
| HEAD at handoff | `fffb4cef55c9e537095093bb796ee4509e899c03` |
| Fork remote | `fliperama86` ? `https://github.com/fliperama86/KytyPS5.git` |
| Official remote | `origin` ? `https://github.com/KytyPS5/KytyPS5.git` |
| TarkusR remote | `upstream` ? `https://github.com/TarkusR/KytyPS5.git` |
| Runtime folder | `C:/Users/dudu/Projects/KytyPS5/_Runtime` |
| Game | `E:/Emulation/PS5/Games/DemonsSouls-PPSA01342-dump` |
| Title/version | `PPSA01342 / 01.005.000` |
| Runtime save | `_SaveData/PPSA01342/SAVEDATA0PlayerProfile0/USR-DATA` |
| Build tree | `_Build/windows`; dependencies in `_Build/deps` |
| Current run record | `_Build/active-profile.json` |

Everything except the game dump now lives in `C:/Users/dudu/Projects/KytyPS5`. It previously spanned three source checkouts plus a runtime folder on `E:`; the other two checkouts were linked git worktrees sharing this repository's object store, and are retired under `C:/Users/dudu/Projects/_retired` together with patches of the uncommitted work found in them. The emulation drive keeps only `E:/Emulation/PS5/Games/DemonsSouls-PPSA01342-dump`. Sections below and the notes in `docs/investigations/` were written before the consolidation and still quote the older paths.

The BDA cache, evaluator arena, profiler zones and these documents are commit `dfd97b5`; the descriptor evaluation memo described below is `c3012c9` on top. Both are pushed to the `fliperama86` fork only, never to `origin` or `upstream`.

## Running build and user control

- At handoff, PID **40148** runs `E:/Emulation/PS5/KytyPS5/kyty_emulator-profile.exe`. Verify the process identity before acting; PIDs are transient.
- Executable SHA-256: `8a7c7dd79b01b8d22ff8952d61e6e6d821d054b4ecaebd7a9bbb9c384bae0881`.
- Source snapshot: `srt-pmr-source.patch` in the evidence directory below. It records the uncommitted source used for the binary.
- Launcher: `E:/Emulation/PS5/KytyPS5/Play Demon's Souls - performance.cmd`.
- The previous working `kyty_emulator.exe` remains untouched, SHA-256 `6594aa1866fae3a4ef5cb831687ce346006157db2f61fa432f2a6188b248721f`.
- Last published release remains `des-experimental-2026-09-10-fffb4ce`; the new optimizations are not in that release.

The user has taken the controls and moved the character around the Nexus. The final screenshot `pmr-after-trace.png` shows a different position/camera from the benchmark. Leave the current session available to the user; do not reset the character, restore a save, replace a running binary, or send menu input just to tidy the handoff. No capture client, compiler, or test process was left running by this task.

The current environment enables `KYTY_DEBUG_GUEST_FAULT=1`, `KYTY_DEBUG_DES_TOUCH_LIST=1`, `KYTY_DEBUG_DES_TOUCH_TRACE=1`, `KYTY_DEBUG_DES_TOUCH_SERIALIZE=1`, and `KYTY_DEBUG_DES_TOUCH_FULL_CAPTURE=0`. Arguments are `--game E:/Emulation/PS5/Games/DemonsSouls-PPSA01342-dump --profiler-direction Network`.

The original console export and a pre-profiling save backup remain preserved. The backup is `save-before` in the evidence directory. Normal game saves may have changed during the session; do not overwrite them with the backup without a concrete testing need and respecting the user's current play session.

## Measurements and evidence

Benchmark metrics, traces, game logs, screenshots, and staged source snapshots live under the directory below. Build/test logs remain in the source build tree and are listed under validation.

`E:/Emulation/PS5/KytyPS5/_Diagnostics/perf-20260910-141256`

| Prefix | Result | Interpretation |
| --- | --- | --- |
| `full-steady-clean` | 4.2104 FPS, 29.45 s | Clean Release baseline, full collision capture |
| `light-steady-clean` | 4.2106 FPS, 29.45 s | Same binary, lighter capture; no gain |
| `materialize-steady-clean` | 4.1825 FPS, 29.41 s | Profiling scopes, no behavioral optimization |
| `bda-cache-steady-clean` | 6.3575 FPS, 29.41 s | BDA scan cache |
| `srt-reserve-steady-clean` | 6.1218 FPS, 29.40 s | Reservation caps alone did not improve the BDA result |
| `srt-pmr-steady-clean` | 7.5462 FPS, 29.42 s | Combined BDA cache + capped reserves + arena |
| `srt-pmr-confirm-clean` | 7.4853 FPS, 44.62 s | Longer pre-trace confirmation |

FPS uses presented-frame deltas and elapsed wall time, not rounded title FPS. CPU usage stayed near 13 logical-core equivalents; GPU device utilization increased from roughly 14?16% to roughly 20%. Hardware is Ryzen 9 9950X3D / RTX 5090.

**Exclude** `srt-pmr-steady.tracy` and `srt-pmr-post-trace-clean.*` from stationary comparison. The user moved during this later period and new shaders compiled. The trace includes 6.282 seconds of compute pipeline creation; the later metrics report 3.9423 FPS across movement/compilation. Neither demonstrates regression in the controlled scene. The `clean` filename on the latter does not make it a valid stationary benchmark. A prior window overlapping an analyzer build and all loading captures are likewise excluded.

Traces establishing the original bottlenecks:

- `materialize-steady.tracy`: 20.12 s; compute `PrepareBda` self 6.130 s (30.5%), resource materialization across shader stages 8.168 s (40.6%), indirect-argument synchronization 0.379 s (1.9%).
- `bda-cache-steady.tracy`: 20.09 s; 1,204 actual BDA scans across graphics/compute, 0.710 s total. Compute preparation alone was called 42,423 times. `EvaluateRuntimeSourcesImpl` then dominated at 10.951 s (54.5%), motivating the arena change.

There are no CPU callstack/context-switch samples in this non-administrator session and no Vulkan GPU timestamp zones. CPU scope percentages describe instrumented paths, not all guest-worker CPU activity. Device-wide `nvidia-smi` readings are not precise per-process GPU timing. Dense Tracy scopes add overhead; use disconnected captures for FPS measurements.

## Implementation details that matter

- `src/graphics/host_gpu/renderer/cache/bufferCache.h/.cpp`: atomic BDA generation advances with release ordering **after** CPU dirty state or buffer registration/removal changes. Includes write-oriented GPU readback that marks CPU data dirty.
- `src/graphics/host_gpu/renderer/cache/gpuResourceManager.h/.cpp`: mapping generation is protected by the mapped-range mutex. `PrepareBda()` acquires the buffer generation before scanning and publishes only those captured generations afterward. A concurrent invalidation therefore forces a subsequent scan. Skipped scans still set pending fault processing. Actual scans have their own profiler zone.
- `src/graphics/shader/recompiler/ir/passes/SrtWalker.cpp`: cache reserve starts at at most 64 entries; recursion-stack reserve at at most 16. A 4 KiB top-level `std::pmr::monotonic_buffer_resource` uses ordinary heap fallback. Normal, clean, and nested active-mask evaluators share allocation storage, **not cached values**. Their containers remain distinct and die before the arena. Immediate-only paths still avoid container allocation.
- `src/emulator.cpp`: profiler initialization follows guest address-space reservation. Starting Tracy earlier caused two profiled startups to fail reservation; the reordered startups succeeded.
- Named profiler zones were added through compute/draw preparation, shader lookup/materialization/evaluation, and indirect synchronization. `WindowContext::UpdateTitle()` emits a guarded `FrameMark` after successful presentation.
- `tests/ShaderRecompilerComputeTests.cpp` and `CMakeLists.txt`: new production cache test/CTest entry `bda_generation_cache`.

A native indirect-dispatch optimization draft was archived as `_Build/native-indirect-experiment.patch` and removed from source. It was never enabled in a game run. Indirect synchronization was under 2% and is not the current priority. `KYTY_DEBUG_GPU_INDIRECT_ARGS` and `DispatchIndirectAddress` are absent from production source.

## Validation completed

- Release emulator built successfully with clang-cl 22.1.6 and existing LTO configuration.
- `shader_recompiler_compute_tests --bda-generation-only`: passed.
- `shader_recompiler_compute_tests --buffer-cache-gc-only`: passed.
- Windows `shader_recompiler_compute_tests --buffer-cache-range-only`: passed (`UnifiedTextureCacheFlow`).
- `resource_tracking_tests`: passed after both reservation and PMR changes.
- `resource_materialization_tests`: passed after both reservation and PMR changes.
- Documentation moves verified against pre-edit hashes; local Markdown links checked; staged and unstaged `git diff --check` passed.
- BDA-only and reservation-cap builds saved/exited normally; combined arena build loaded that save and remained running for the user's movement. No full save/reload cycle was requested after the user took control of the final build.

The BDA test uses real guest direct memory, production caches/scheduler, native Vulkan readback, explicit CPU invalidation/write, a second registered buffer, map changes, and preserved/downloaded GPU data. Early fixture failures were corrected: a tiny read-only request returned a temporary stream buffer, and `FillBuffer` initially took the CPU path on protected memory. Final tests force registered backing and GPU ownership and pass. These were test-setup failures, not accepted production failures.

Logs in the active source build tree include `_Build/windows/bda-generation-final.log`, `buffer-cache-gc-regression.log`, `buffer-cache-range-regression.log`, `srt-pmr-build.log`, `srt-pmr-resource-tracking.log`, and `srt-pmr-resource-materialization.log`. Earlier failed fixture logs are retained separately.

The prior upstream merge had 36/37 CTest passes, with the known unchanged Windows socket `PEEK|WAITALL` failure in `kernel_file_system`. That unrelated full suite was not rerun for these changes; do not claim a new all-suite pass.

## Resuming profiling or building

Use the existing Visual Studio developer-shell setup, LLVM on PATH, then:

```powershell
cmake --build _Build/windows --target kyty_emulator resource_tracking_tests resource_materialization_tests --parallel 12
```

`KYTY_RELEASE_TAG` is empty for these unpublished experiments. Dirty builds disable the disk Vulkan pipeline cache, so expect cold compilation on each fresh game run. Do not erase the existing runtime pipeline cache. Do not build or run tests during a measurement.

The ignored helper `_Build/start-des-profile.ps1 -Phase <unique-name> -StageBuild` stages only the profile executable/PDB, refuses an already running profile PID, creates unique logs/source snapshots, and updates `active-profile.json`. It does not replace the preserved main executable. `_Build/measure-des-performance.ps1 -ProcessId <pid> -OutputPrefix <new-prefix> -Seconds 30` writes raw samples and a JSON summary. Use the phase/run JSON to identify exact binaries and flags.

The vendored tools are `_Build/profiling-tools/capture/tracy-capture.exe`, `csvexport/tracy-csvexport.exe`, and `analyzer/tracy-sample-analyzer.exe`, all built against Tracy 0.13.1 / protocol 76. Capture with `-a 127.0.0.1 -p 8086 -s 20 -o <new.tracy>`; CSV export `-e` gives self times, no `-e` gives total times, and the local analyzer `--frames` emits intervals. PowerShell 5 redirected CSV is UTF-16. Ignore the CSV `total_perc` column's preconnection denominator; calculate against the actual capture window.

For a fresh run, the user previously authorized automated navigation: Cross is J/VK74, hold about 4.5 seconds during the opening cinematic, then advance Press Any Button ? Continue ? Continue Offline with short Cross presses. The existing helper is `C:/Users/dudu/Projects/KytyPS5-DeS/_Build/des-window.ps1`; use `-Show` for reliable focused input and inspect screenshots. This does not authorize disrupting the user's current active play session.

## Follow-up: descriptor evaluation memo

`EvaluateRuntimeSourcesImpl` remained the largest zone after the arena change, at
2,011,523 calls with a 3,135 ns self-time mean. The plan is walked once per shader stage
per draw, so shader translation caching does not help it: only the per-call cost or the
call count can move. Three changes reduce the per-call cost.

- `Inst` carries a dense `PlanIndex()`, assigned to every clone in `ExtractResourcePlan`,
  and `ResourcePlan::value_count` records the total.
- The evaluator memo is a per-thread flat array addressed by that index and stamped with a
  64-bit per-evaluator epoch, replacing `std::pmr::unordered_map`. No hashing, no per-node
  allocation, no clearing between draws. The stamp's high bit marks an evaluation in
  progress, which also makes cycle detection constant time. Programs never extracted into a
  plan carry no index and keep the pointer-keyed map; only unit fixtures reach that path.
- Descriptor, flat-SRT and active-source result buffers are reused per thread and swapped
  into the caller's vectors on success, so the steady state allocates nothing while failure
  still leaves every destination unchanged.

`tests/SrtEvaluatorBench.cpp` builds target `srt_evaluator_bench`, deliberately not a CTest
entry because it reports timings rather than asserting behaviour. Run
`srt_evaluator_bench [--iterations=N] [--only=NAME] [--sweep]`. Its checksum is
deterministic and is the regression signal for further evaluator work.

| Shape | Flat slots | Before ns/call | After ns/call |
| --- | --- | --- | --- |
| `small-vs` | 8 | 841 | 531 |
| `typical-ps` | 80 | 12045 | 7218 |
| `heavy-ps` | 160 | 34739 | 21114 |
| `chained-cs` | 64 | 9879 | 6200 |
| `control-flow-ps` | 80 | 12138 | 7290 |
| `waterfall-ps` | 80 | 12726 | 8062 |

Those are synthetic-plan numbers. The change has since been measured in the emulator on a parked
Nexus scene: **7.8826 FPS against the 7.5462 FPS combined-arena baseline, about 4.5%**, at 12.622
CPU core-equivalents and 19.9% GPU (`memo-steady-clean.json`). The gain is smaller than the
benchmark's 37-40% because evaluation is only about 15% of the render critical path. The two runs
come from different sessions rather than a back-to-back A/B; `kyty_emulator-profile-srt-pmr.exe`
is preserved in the runtime folder if a controlled comparison is wanted.

### Corrections to the earlier analysis

- Evaluation cost is linear, not superlinear: roughly 36 ns per SRT slot plus 21 ns per IR
  instruction, flat across 40, 80 and 160 slots. An earlier reading of rising per-slot cost
  came from comparing shapes whose per-slot instruction counts also differed.
- `clean_flat_slots` is populated only for shaders with indirect images, so the clean reader
  is hot only for bindless-heap shaders and resource control-flow conditions. Every other
  raw read is a direct `memcpy` from guest memory.
- `ReadShaderGuestMemory` reaches `TextureCache::IsRegionGpuModified`, which takes the
  texture-cache mutex and runs a page-table region search for every four-byte word. That
  cost is invisible in the traces because it is charged to `EvaluateRuntimeSourcesImpl` self
  time.

## Follow-up: pooled bindings

Commit `956b062`. `RenderExecutor::PrepareBindings` used to return a fresh `PreparedBindings` by
value for every draw and every stage; each owned five vectors plus a `mip_views` vector per texture
binding, which the whole-process profile had put at about 3% of samples in heap traffic. The
executor now keeps one object per stage (`m_vertex_bindings`, `m_pixel_bindings`,
`m_compute_bindings`) and fills it in place through `PreparedBindings::Reset`, which clears every
container while retaining capacity, so the steady state allocates nothing. `GraphicsBindings` hands
out pointers to the pooled objects.

Measured on the parked Nexus scene: **8.1998 FPS against the 7.8826 FPS memo build, about 4%**
(`pooled-check/pooled-steady-clean.json`). The runs are from different sessions, not a back-to-back
A/B.

`tests/ShaderRecompilerComputeTests.cpp` was left calling the old by-value API by that commit and
did not build until the flat-program work below adapted it; all its cases pass again.

## Follow-up: flat SRT program

The evaluator memo had removed hashing and allocation, but each value still cost a `Resolve`, a
memo probe, a `std::vector` operand fetch and a recursive call, about 21 ns per IR instruction. That
work depends only on the plan, and extracted plans are immutable, so it now happens once.

- `graphics/shader/recompiler/ir/SrtFlatProgram.h` defines `SrtFlatProgram`: one contiguous
  instruction array (`SrtFlatInst`, 40 bytes, up to five register operands plus an immediate), a
  register per instruction, and a root per descriptor source, flat SRT slot, resource control-flow
  condition and uniform-fill word. Each root lists its whole dependency closure in operand-first
  order, so callers may evaluate any subset of roots in any order.
- `CompileSrtPlan` (in `SrtWalker.cpp`) lowers a plan into `ResourcePlan::flat`. `ExtractResourcePlan`
  calls it as its last step. The lowering mirrors `Evaluator` step for step and is specialised per
  evaluation context: normal, clean (specialization reader, no clean-table routing) and one context
  per `ReadFirstLane` mask, because the active mask changes which operand a select takes. Whatever
  the walker rejects structurally (unsupported opcode, cycle, malformed operand, non-invariant phi)
  becomes a root that fails before running; whatever it rejects at run time (memory read, address
  overflow, user-data bounds, float conversion range) fails in the same op.
- At run time `FlatMachine` replays a root as a linear loop over a per-thread register file whose
  entries are stamped with the epoch of the call that wrote them. Shared work between roots is done
  once per call and nothing is cleared between draws. The control-flow activity walk, the source
  loop and the flat-slot loop are the same as the walker's, so transactional failure is unchanged.
- The walker stays as the fallback and is still the reference: it runs for programs that were never
  extracted (unit fixtures), for a caller whose clean-slot set differs from the one compiled into
  the plan, and for a plan whose shape changed after compilation.

`srt_evaluator_bench` now measures both evaluators on every shape and aborts if their checksums
differ, which is the equivalence check for the lowering. Idle machine, 100,000 iterations:

| Shape | Flat slots | Walker ns/call | Flat ns/call | Speed-up |
| --- | --- | --- | --- | --- |
| `small-vs` | 8 | 558 | 116 | 4.8x |
| `typical-ps` | 80 | 7780 | 1013 | 7.7x |
| `heavy-ps` | 160 | 22896 | 2423 | 9.5x |
| `chained-cs` | 64 | 6480 | 926 | 7.0x |
| `control-flow-ps` | 80 | 7906 | 1050 | 7.5x |
| `waterfall-ps` | 80 | 8595 | 1044 | 8.2x |

The `--sweep` puts the marginal cost at about **1.2 ns per IR instruction** (was 22.7) and about
**9 ns per flat slot** (was 37); the slot cost is now dominated by the guest read itself.

Measured in the emulator on the parked Nexus scene, same protocol as the earlier rows (intro skipped
with a 2 s Cross hold at frame 700, twelve Cross presses, 100 s warm-up, 30 s sample):
**10.1974 FPS against the 8.1998 FPS pooled build, about 24%**, at 12.612 CPU core-equivalents and
25.1% GPU (`flat-plan/flat-steady-clean.json`, screenshot `flat-plan/after-nav.png`). Different
sessions, not a back-to-back A/B. The gain matches the zone data below: evaluation was about 39%
of the GPU thread, and the compiled program cuts its per-call cost by roughly half in the shapes
that dominate in-game, where the guest reads that remain are now the larger part of each call.

The memo section above says evaluation was "about 15% of the render critical path". That figure
came from callstack samples, which this LTO build mis-attributes. Zone timing from the same trace
(`memo-steady.tracy`, exact self time) puts `EvaluateRuntimeSourcesImpl` at **38.8%** of the GPU
thread's instrumented time, over 3,033,624 calls; it was the largest zone by a factor of six. That
trace also fixes the frame anatomy: 149 frames, 1,389,903 draws and 240,463 direct plus 41,847
indirect dispatches, i.e. about **9,300 draws and 1,900 dispatches per frame** at a 133 ms median.

## Next useful work

Read [the whole-process CPU profile](investigations/cpu-profile-2026-09-10.md) first. It establishes
that whole-process CPU share is the wrong metric to steer by, that the render critical path is still
the lever, and that individual symbol names inside this LTO build are not trustworthy. Steer by
zone self time, not by sample attribution.

1. Skip descriptor evaluation for draws whose inputs did not change. Consecutive draws mostly share
   shaders and most of their SRT; the compiled program makes the input set explicit (user-data
   registers, shader base, the guest words each `ReadAddress`/`ReadBuffer` touched). A cache keyed on
   those inputs could remove most of the remaining evaluation cost, but it needs proven
   invalidation of the guest words between draws. Do not cache without it.
2. Exit the emulator cleanly at least once so the disk Vulkan pipeline cache persists. It is enabled
   only for a committed, clean tree, and every profiling session so far ended in a forced
   termination, so the cache on disk is stale and each run still recompiles the compute shaders cold.
3. The long tail on the GPU thread from the memo trace: `FlipQueue::Flip` 6.6%, `RebindBuffers`
   6.4%, `SynchronizeBdaBuffers` 4.9%, `ExecutePreparedDraw` 4.8%, `CommandProcessor::Process`
   3.8%, `SyncArguments` 2.7%. None is large alone; the per-draw ones add up across 9,300 draws.
4. If bindless shaders prove hot, memoize the clean reader's GPU-dirty verdict per page for the
   duration of one materialization instead of per four-byte word. Keep it conservative: only a
   whole-page clean verdict may skip the per-word check, because a partially dirty page must still be
   tested word by word.
5. Measure moving gameplay separately from cold shader compilation.
6. Before a release, build the actual committed release binary, validate it, then package and publish
   through the existing workflow.

Two lines of enquiry are closed. Page-fault-based memory tracking is not a cost at 628 faults per
second. The `DesTouchTrace` probes cost about 3.4% in exception machinery, but disabling them stopped
the game reaching gameplay, so the collision workaround is load-bearing and that cost cannot simply
be reclaimed.
