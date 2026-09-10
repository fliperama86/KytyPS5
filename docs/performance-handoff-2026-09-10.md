# Demon's Souls performance handoff ? September 10, 2026

## Outcome

Two implemented optimizations raised the measured stationary Nexus result from **4.21 FPS to approximately 7.5 FPS (about 78%)** on this machine:

1. Cache completed BDA buffer synchronization until CPU dirtiness, cached-buffer topology, or mapped ranges change.
2. Use a per-call temporary memory arena for shader resource evaluators, preserving distinct normal, clean, and active-mask caches.

Both changes are built and tested. The experimental executable is running. This is a local experimental build, not a new published release. It remains slow, and the results do not establish performance in other areas or broad game stability.

The user requested all project notes in the repository and then requested this handoff. Ten authored documents were moved out of ignored build/old-checkout/runtime folders into `docs/`. Use [the documentation index](README.md) and [the performance report](demons-souls-performance.md). Add future notes here.

## Correct checkout and runtime

| Item | Location / value |
| --- | --- |
| Active fork source | `C:/Users/dudu/Projects/KytyPS5-DeS-1d6e23a` |
| Branch | `demons-souls-workaround` |
| HEAD at handoff | `fffb4cef55c9e537095093bb796ee4509e899c03` |
| Fork remote | `fliperama86` ? `https://github.com/fliperama86/KytyPS5.git` |
| Official remote | `origin` ? `https://github.com/KytyPS5/KytyPS5.git` |
| TarkusR remote | `upstream` ? `https://github.com/TarkusR/KytyPS5.git` |
| Single runtime folder | `E:/Emulation/PS5/KytyPS5` |
| Game | `E:/Emulation/PS5/Games/DemonsSouls-PPSA01342-dump` |
| Title/version | `PPSA01342 / 01.005.000` |
| Runtime save | `_SaveData/PPSA01342/SAVEDATA0PlayerProfile0/USR-DATA` |
| Build tree | active source `_Build/windows` |
| Current run record | active source `_Build/active-profile.json` |

The environment's original working directory `C:/Users/dudu/Projects/KytyPS5` is an older official checkout at `0b4e78c`, with its own pre-existing `runtimeLinker.cpp` modification. **Do the current work in the active fork above.** Source checkouts were not consolidated because their build trees have absolute paths; the user's single-folder preference is honored for the emulator runtime.

The source changes are uncommitted. Documentation is staged in Git; source/test/CMake changes are unstaged. No profiling changes have been pushed. Preserve this work and the unrelated older checkout modification.

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

## Next useful work

1. Re-establish a controlled stationary scene on the combined build and capture after shaders settle. The final trace from this task was contaminated by user movement, so do not use it to rank the new steady-state bottlenecks.
2. If resource evaluation remains dominant, investigate repeated instruction evaluation/hash lookup and nested active-mask evaluators. Preserve clean-reader semantics, per-call mutable guest descriptors, active masks, and transactional failure. Do not cache resource snapshots across draws without proven invalidation.
3. Measure moving gameplay separately from cold shader compilation. The current dirty-build startup cache behavior is a separate issue from steady performance.
4. Before a release, review/commit the local source and staged documentation, build the actual committed release binary, validate it, then package/publish through the existing workflow. No new release was requested or created during this profiling task.
