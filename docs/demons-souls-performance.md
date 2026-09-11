# Demon's Souls performance investigation ? September 10, 2026

## Test conditions

- Game: own dump of PPSA01342, version 01.005.000, stationary character in the Nexus, same camera and save for each run.
- Source baseline: `fffb4cef55c9e537095093bb796ee4509e899c03`, Release, clang-cl 22.1.6, Windows.
- Hardware: Ryzen 9 9950X3D and GeForce RTX 5090.
- Collision serialization remains enabled throughout. Full and light capture were compared separately.
- One runtime directory, `_Runtime`, and one binary, `_Build/windows/kyty_emulator.exe`, launched in place by the root `Play Demon's Souls.cmd`. No staged or preserved copies; rebuild an earlier commit for a comparison.
- Cold startup/shader compilation is excluded. No builds or tests ran during the clean measurement windows.
- FPS comes from the change in presented frame count divided by wall time, not the rounded window-title FPS. CPU is process CPU time divided by wall time, expressed as fully occupied logical cores. GPU readings are device-wide `nvidia-smi` samples.

Local evidence is under `_Diagnostics/perf-20260910-141256` in the runtime directory. Traces use vendored Tracy 0.13.1 / protocol 76.

## Baseline results

| Run | Duration | Presented frames | FPS | CPU core equivalents | Mean GPU utilization |
| --- | ---: | ---: | ---: | ---: | ---: |
| Release, full collision capture | 29.45 s | 124 | 4.2104 | 13.284 | 15.87% |
| Same release, light collision capture | 29.45 s | 124 | 4.2106 | 13.291 | 15.87% |
| Initial profiling scopes, light | 29.43 s | 124 | 4.2132 | 13.349 | 14.47% |
| Shader resource scopes, light | 29.41 s | 123 | 4.1825 | 13.329 | 14.00% |
| BDA scan cache | 29.41 s | 187 | 6.3575 | 13.110 | 17.17% |
| BDA cache + capped evaluator reserves | 29.40 s | 180 | 6.1218 | 13.034 | 18.10% |
| BDA cache + capped reserves + temporary arena | 29.42 s | 222 | 7.5462 | 12.941 | 20.63% |
| Combined optimizations, confirmation | 44.62 s | 334 | 7.4853 | 12.982 | 19.89% |

The release binary SHA-256 is `eab650d0064b916dde8aa95523fed6ce7bfffe04c62a31ab773100165ed53fc5`. The last baseline with shader resource scopes is `c6545b66ef7ad80da8a0f8bd9d6494cbe83d57bebac62df57bed03c73dcf4bb3`; its precise source diff is `materialize-source.patch` beside the trace.

Removing per-node collision diagnostics did not produce a measurable gain in this scene. The lighter mode also loaded the save and completed save/exit successfully. This does not establish that the serialization workaround itself is free.

`materialize-steady.tracy` contains 20.12 seconds and 87 frame markers. Excluding synthetic/preconnection intervals and incomplete edge intervals leaves 83 complete intervals: median 233.58 ms, mean 239.33 ms, p95 267.60 ms, maximum 279.45 ms. These are short, stationary-scene measurements, not a whole-game benchmark.

## Measured CPU hot paths

Self times below exclude nested zones; percentages divide by the 20.12-second capture window. Do not use the CSV export's `total_perc`, which includes a preconnection span in its denominator. Template instantiations with the same zone name are aggregated where indicated.

| Scope | Self time | Share of capture | Calls |
| --- | ---: | ---: | ---: |
| `RenderCompute::PrepareBda` | 6.130 s | 30.5% | 29,573 |
| `ProgramCache::MaterializeResources` (all stages) | 8.168 s | 40.6% | 1,701,465 |
| `CpOpDispatchIndirect::SyncArguments` | 0.379 s | 1.9% | 20,586 |

The BDA preparation walks every mapped guest range and synchronizes overlapping cached buffers before each applicable compute dispatch. Its cost is repeated synchronization preparation, not necessarily actual data uploads.

The shader cache hits rebuild resource snapshots and specialization state. Shader source lookup and permutation matching are small after separating this work from the original broad shader-preparation scope. Further zones split resource evaluation, indirect image probing, and specialization construction.

The initial broad trace attributed substantial time to indirect dispatch. The refined trace showed that argument synchronization itself accounts for less than 2%; a native-indirect draft was archived and removed without running it. That route is not the current optimization target.

## Instrumentation and limits

Frame markers are emitted after successful presentation. Graphics/compute preparation, cache lookup, materialization, and BDA synchronization have named CPU zones. Profiler initialization now follows guest address-space reservation: initializing Tracy earlier caused two profiled starts to fail while reserving guest memory; later starts with the new order succeeded.

Tracy captured no CPU callstack samples or context switches in this non-administrator session. The zones identify instrumented rendering paths, not all guest-worker CPU activity. There are no Vulkan GPU timestamp zones yet. Low device utilization and expensive CPU preparation justify investigating the CPU paths first, but do not by themselves prove the absence of GPU stalls.

All current gameplay comparisons use the Nexus scene. Other levels, extended gameplay, other drivers, and other machines remain unmeasured. A prior mixed loading measurement and a window overlapping an analyzer build are excluded from the baseline table.

## Optimization validation

The first experiment caches completed BDA synchronization using a buffer generation and a mapped-range generation. CPU invalidation, CPU-dirty readback, buffer registration/removal, and mapping changes invalidate the snapshot. The buffer generation is acquired before scanning; only that captured value is published afterward, so a concurrent invalidation still requires a later scan. Fault processing remains pending even when the scan is skipped.

The Release emulator and production test executable build successfully. The focused `bda_generation_cache` test passes with actual mapped guest memory and Vulkan buffers: initial materialization, unchanged repeated preparation, explicit CPU invalidation/write followed by updated native-buffer readback, new buffer registration, mapped-range changes, and preservation/download of GPU-owned data. Existing `buffer_cache_dirty_gc` and Windows `buffer_cache_ranges` regressions also pass. This is production cache-boundary coverage, not an end-to-end emulated game dispatch test.

The first optimized runtime binary SHA-256 is `366602383081b23ae709e65fcf3c51fc37126cba7071c15807795b10491203dd`; its source snapshot is `bda-cache-source.patch`. In the same stationary Nexus scene, the clean 29.41-second measurement produced 187 presented frames: **6.3575 FPS**, versus 4.2106 FPS for the release baseline (**51.0% higher**). CPU use was 13.110 core equivalents, average GPU utilization 17.17%, and average GPU power 100.54 W. This is one short optimized window; performance in other scenes has not been compared. Saving/exiting completed normally.

`bda-cache-steady.tracy` captured 20.09 seconds. Only 1,204 BDA scans ran across graphics and compute, taking 0.710 seconds total. Compute alone called preparation 42,423 times; its inclusive preparation time fell to 0.381 seconds despite processing more frames. `EvaluateRuntimeSourcesImpl` became the largest remaining self-time scope: 10.951 seconds (54.5%) over 2,523,172 calls. This motivates the next shader-evaluation allocation experiment.

Capping the evaluator's initial cache/recursion-stack reservations at 64/16 entries passed the resource tracking and materialization suites, but its first 29.40-second window measured only 6.1218 FPS. This did not beat the 6.3575 FPS BDA-only result. A follow-up experiment retains the cap while using a 4 KiB per-call temporary arena (with heap fallback) for separate normal/clean/active-mask evaluator containers; the two resource suites pass. Its first clean 29.42-second game window produced 222 presented frames: **7.5462 FPS**, with 12.941 CPU core equivalents and 20.63% mean GPU utilization. That is **79.2% above** the release baseline and **18.7% above** the BDA-only result. The longer 44.62-second confirmation produced 334 frames at **7.4853 FPS** (77.8% above baseline). The two clean windows support approximately **7.5 FPS / 78% improvement** in this scene. No shader resource values are shared between evaluations.

The final `srt-pmr-steady.tracy` capture is **excluded from the stationary comparison**: the user moved the player, new shaders compiled, and a subsequent screenshot showed a different camera/position. It contains 6.282 seconds in compute pipeline creation and cannot provide a steady-state percentage for the final optimized build. The subsequent `srt-pmr-post-trace-clean` sample is also excluded despite its filename (3.9423 FPS across movement/compilation). The two earlier clean windows above are the reported comparison. Re-establish the same position/camera and allow compilation to settle before another comparison.

The combined runtime binary SHA-256 is `8a7c7dd79b01b8d22ff8952d61e6e6d821d054b4ecaebd7a9bbb9c384bae0881`, with source snapshot `srt-pmr-source.patch`. Launch it using `Play Demon's Souls - performance.cmd` in the existing runtime directory. The prior working `kyty_emulator.exe` is preserved.

## Render-thread per-draw work ? September 10, 2026

The parked Nexus scene issues about 9,300 draws and 1,900 dispatches per frame, and `Thread_Gpu`
is the bottleneck. Four changes were tried against the `7e37a84` baseline, each measured with the
procedure in [Reaching the Nexus](reaching-the-nexus.md): 30-second clean windows in the same
parked scene, plus a 20-second Tracy capture taken afterwards with the measurement already
finished. Scene noise is roughly +/-4% per window, so read the trend and the zone self times, not
any single window.

| Build | FPS | CPU core equivalents | Mean GPU |
| --- | ---: | ---: | ---: |
| `7e37a84` baseline | 10.000 | 12.665 | 24.3% |
| `0183bc8` incremental BDA ranges | 10.122 | 12.648 | 25.7% |
| `bc74f63` context dirty mask, pipeline and dynamic-state memo | 10.398 | 12.573 | 26.0% |
| `61d5917` per-stage allocation pooling | 10.703 | 12.535 | 27.2% |
| rebind memo (not kept) | 10.729 | 12.557 | 27.2% |
| page verdict cache | 10.887 | 12.551 | 26.1% |

Zone self times divide by the 20.1-second capture window; every zone below runs on the render
thread, so the share is that thread's share.

### Incremental BDA synchronization

`GpuResourceManager::PrepareBda` early-outs on a buffer generation and a mapped-range generation.
When either moved it rescanned every mapped range, walking every registered buffer's whole extent
through the memory tracker. The buffer cache now records the guest pages each invalidation touches
in a `RangeSet` beside the generation; preparation takes that set, intersects it with the mapped
ranges and synchronizes only those intervals. Every place that advanced the generation already had
a range in hand: buffer registration, `InvalidateMemory`, CPU-dirty readback, mapping and
unmapping. Buffer retirement still advances the generation without recording a range, because a
deleted buffer has nothing to upload and a later lookup registers and dirties its replacement.
The recorded range is widened to whole tracker pages, since the tracker marks dirty pages, not
bytes.

In the capture after the change a scan cost 146 microseconds on average against 0.70 milliseconds
before. The scan count varies strongly with the scene (2,078 to 6,370 scans across three
20-second captures of the same parked view), so the total is not a stable comparison; the
per-scan cost is.

### Context dirty mask, pipeline lookup and dynamic state

`HW::Context` now carries one bit per top-level register block that the draw path re-reads every
draw, set by the setters themselves rather than by a register-offset table. Doing it in the
setters covers every writer, including register loads and context save/restore, without having to
map the whole context register space; a restored context is marked entirely dirty. The draw path
clears the bits it consumed.

Two consumers use it. `SetGraphicsDynamicParams` re-recorded fifteen Vulkan dynamic-state commands
per draw; it now compares the register bits plus a small memo of what it reads that is not a
register (the attachment identity, the derived depth and stencil state, the vertex program) and
returns early when nothing moved: 195 nanoseconds per draw down to 36, 0.359 s to 0.062 s.
`PipelineCache::CreateGraphicsPipeline` hashed the 166-byte static-parameter block one byte at a
time on every draw; it now compares the freshly built key against the last one and returns the
remembered pipeline on a match: 238 nanoseconds down to 99, 0.438 s to 0.197 s. The bound pipeline
is only re-bound when it actually changes.

Both memos are keyed on a command-buffer epoch, because the bound pipeline and the dynamic state
die with the command buffer. `CommandBuffer::Begin`, rebinding a different register set, and the
blit helper (which records its own graphics pipeline, viewport and scissor) all advance it.

### Per-stage allocations

Every stage of every draw copied its user-SGPR registers into a fresh vector and built the
resource specialization into two more. The shader parameters now carry a span over the guest
registers; only the NGG vertex path rewrites them, and it uses scratch that survives the call. The
specialization is handed to its destination by swap, so the destination's previous allocation
comes back for the next draw. `PrepareProgram` fell from 94 to 70 nanoseconds for vertex, 518 to
385 for compute, and `BuildResourceSpecialization` from 59 to 56.

The resource snapshot was left alone. It is moved into the per-draw stage runtime and outlives its
producer, so pooling it means changing who owns it, not adding a pool.

### Rebind memo, rejected

`RenderExecutor::RebindBuffers` is the largest remaining instrumented zone at 1.36 s of 20.1 s
(6.8%) over 4.3 million calls. An attempt to skip the buffer-cache walk for a slot whose guest
range, host buffer and resource flags were unchanged, gated on an unchanged buffer-cache
generation, moved the zone from 318 to 301 nanoseconds per call and the frame rate by less than
the scene's noise. It was reverted.

The reason is the gate, not the memo. After the incremental-BDA change the buffer-cache generation
advances on every transition that marks a tracked page CPU-modified, which is what the memo needs
it to do, but it is a single global counter and the guest faults on tracked pages continuously.
Between two consecutive draws it has almost always moved, so the memo almost never hits. Skipping
this work needs a per-buffer or per-page write generation, which is a larger change. Written,
formatted and small-buffer streamed slots must be excluded in any case: they copy or take
ownership on every call, and an image-backed texel buffer can change without the buffer cache
knowing.

### Page verdict cache, measured and reverted

`EvaluateRuntimeSourcesImpl` dominates the render thread: 5.88 s of the 20.1-second capture (29%)
over 4.3 million calls, about 1.37 microseconds per stage per draw, seven times the size of
anything else measured here. The evaluation itself is a compiled flat program, so the suspicion was
its guest reads: a clean read goes through `TryReadGpuCleanBacking`, which per four-byte word runs
`IsGpuAddressRange`, `IsGpuThread`, a buffer-cache `RangeSet` lookup and a locked texture-cache
region query before it reaches the backing store.

The reader now takes one verdict per 4 KiB guest page instead of one per word. A page is `Clean`
when its whole extent passes the same three checks; every word in such a page is then read with
plain `TryReadBacking`, because a range with no GPU-dirty bytes and no GPU-modified image contains
no word that has either. Any other verdict falls back to the unchanged per-word call, so behaviour
is identical, and no per-word result is ever remembered. The cache is eight entries, direct-mapped
by page number, on the stack of `ProgramCache::Get`; it is created beside the `SrtRuntime` and
handed over in `SrtRuntime::userdata`, which was unused. Exactly one materialization runs per
`Get`, so the verdicts cannot outlive the call that took them, and only the GPU thread reaches the
clean path at all (`IsGpuCleanBackingRange` requires `IsGpuThread`), which is the same thread that
runs the materialization. Nothing can therefore record a GPU write between a verdict and the reads
it covers. `SyncGpuCleanBacking` only ever clears GPU-dirty state, so a sync during the call can
turn a `NotClean` page clean, never the reverse. `SyncShaderGuestMemory` was left alone: it works
on whole ranges already and it mutates state, so its result must not be cached.

It works and it changes nothing measurable. The 30-second clean window produced **10.887 FPS**
against 10.703 and 10.786 for the same code without it, inside the scene's own noise, and
`EvaluateRuntimeSourcesImpl` in `page-verdict.tracy` is 5.876 s over 4,299,561 calls, 1,366 ns
each, against 5.876 s over 4,291,183 calls at 1,369 ns in `phaseA-3.tracy`. Taken under Remote
Desktop, like every sample from the flat-program build onwards.

Counters compiled into a throwaway build explain why. Per million `EvaluateRuntimeSourcesImpl`
calls in the parked scene the flat program executes 23.7 million instructions and performs **21.5
million guest word reads — but only 45,000 of them are clean reads**. Demon's Souls marks almost
none of its SRT reads clean, and a read that is not clean has no reader installed at all: it is a
direct `memcpy` from the guest address, which never consults the GPU caches. The page verdict
covers 0.2% of the reads this zone performs, and 99.9% of those now hit a verdict already taken,
which is why the hit rate is excellent and the frame rate is unmoved. The change is kept because it
is cheap, tested and correct, not because it paid.

### What is left

The same counters point at the real shape of the cost. Each `EvaluateRuntimeSourcesImpl` call
reads about **21 guest dwords scattered across the SRT**, one per flattened slot, and re-reads
every one of them for the same shader on every draw. At 1.37 microseconds for 23.7 instructions
plus 21.4 scattered reads, the zone is paying roughly 60 nanoseconds per step, which is memory
latency, not arithmetic. The next experiment is therefore to cache the flattened SRT contents per
shader and user-data pointer and invalidate it from the page writes that can change it, rather than
to make the individual read cheaper. `RenderExecutor::RebindBuffers` remains the second-largest
zone at 1.38 s over 4.3 million calls; the note above on why its memo was rejected still applies.

### CCD affinity

If the render thread is memory-latency bound, where Windows puts it matters. This host is a Ryzen 9
9950X3D: two eight-core CCDs, and `GetLogicalProcessorInformationEx` reports **96 MB of L3 behind
logical CPUs 0-15 and 32 MB behind 16-31**, so the 3D V-cache die is the low half. Twelve guest
worker threads spin at 100% each and, left alone, the scheduler spreads everything over both dies.

`KYTY_GPU_THREAD_AFFINITY`, `KYTY_PRESENT_THREAD_AFFINITY` and `KYTY_GUEST_THREAD_AFFINITY`
(see [settings.md](settings.md)) pin those threads to a hexadecimal mask at startup. Five 30-second
clean windows in the parked Nexus, same binary, same session (Remote Desktop):

| Run | GPU + present | Guest threads | FPS | CPU cores | GPU % |
| --- | --- | --- | --- | --- | --- |
| `none` | unset | unset | 10.938 | 12.57 | 28.1 |
| `gpu-lo` | `0xFFFF` (V-cache) | `0xFFFF0000` | **11.532** | 12.59 | 28.2 |
| `gpu-lo` repeat | `0xFFFF` (V-cache) | `0xFFFF0000` | **11.387** | 12.57 | 28.1 |
| `gpu-hi` | `0xFFFF0000` | `0xFFFF` | 10.611 | 12.51 | 25.3 |
| `gpu-only-lo` | `0xFFFF` (V-cache) | unset | 10.982 | 12.63 | 27.5 |

Splitting the render and presentation threads onto the V-cache die and the guest threads onto the
other one is worth **4 to 5%**, twice the scene's run-to-run spread (10.70-10.98 across every
unpinned sample of this build). The mirror image loses 3%, so the effect is the cache and not the
split.

Neither half of the arrangement pays on its own. `gpu-only-lo` pins the render thread to the 96 MB
die and lands on the unpinned number, because the twelve spinning guest workers are still free to
land there and evict its working set; `gpu-hi` separates the two groups just as cleanly as `gpu-lo`
and is the worst run of the five, because the thread that needs the cache is not on it. Both
conditions are needed: the render thread on the large L3, and nothing else allowed onto it.

Total CPU is flat at 12.5 to 12.6 core-equivalents everywhere, so this is not a throughput change;
the frame rate moves because the render thread's scattered SRT reads hit in cache more often.
`gpu-lo` also shifts about 0.3 core-equivalents from user to kernel time, which is the guest workers
contending harder on sixteen cores instead of thirty-two, and costs nothing here because they spin.

The variables stay off by default: the masks are specific to this CPU, and a fixed mask on a host
with a different CCD layout, or with one CCD, would be a pessimization. On this machine set

```powershell
$env:KYTY_GPU_THREAD_AFFINITY = '0xFFFF'; $env:KYTY_PRESENT_THREAD_AFFINITY = '0xFFFF'
$env:KYTY_GUEST_THREAD_AFFINITY = '0xFFFF0000'
```

before launching. Deriving the masks from the cache topology at runtime instead of hard-coding them
is the obvious way to make this a default, and has not been done.

## Reproducing a capture

Build the vendored tools in a Visual Studio developer shell (matching Tracy versions is required):

```powershell
cmake -S 3rdparty/tracy/capture -B _Build/profiling-tools/capture -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build _Build/profiling-tools/capture --parallel 12
cmake -S 3rdparty/tracy/csvexport -B _Build/profiling-tools/csvexport -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build _Build/profiling-tools/csvexport --parallel 12
```

Launch the experimental executable with the collision variables from the workaround document and `--profiler-direction Network`. Advance to the save, leave the camera unchanged, and wait for loading/compilation to finish. Capture from the source directory, choosing a new output path:

```powershell
& .\_Build\profiling-tools\capture\tracy-capture.exe -a 127.0.0.1 -p 8086 -s 20 -o capture.tracy
& .\_Build\profiling-tools\csvexport\tracy-csvexport.exe -e capture.tracy > self.csv
& .\_Build\profiling-tools\csvexport\tracy-csvexport.exe capture.tracy > total.csv
```

Disconnect the capture client for the independent FPS measurement. This investigation's local helper is `_Build/measure-des-performance.ps1 -ProcessId <pid> -OutputPrefix <new-prefix> -Seconds 30`; it saves raw samples, per-thread CPU totals, and a JSON summary. Keep builds and tests stopped throughout the measurement. The original save is backed up as `save-before` beside the trace files.
