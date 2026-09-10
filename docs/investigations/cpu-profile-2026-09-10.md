# Whole-process CPU profile — September 10, 2026

First profile of the emulator that sees more than the instrumented render path. It changes where
optimization effort should go, and it corrects two conclusions drawn from the earlier Tracy-only
captures.

## Why the earlier profiles were blind

Tracy only reports threads that call into it. Across a 20-second stationary Nexus capture its zones
accounted for **20.97 s of self time**, while the process consumed roughly **263 core-seconds** —
about **8% of the CPU actually burned**. Every optimization to that point had been chosen from
inside that 8%.

Tracy can also sample call stacks through ETW, but Windows grants ETW stack and context-switch
tracing only to an elevated process. Earlier runs were not elevated, so the traces contained
**0 callstack samples and 0 context switches**, and knew of only two threads.

`_Build/start-des-profile-elevated.ps1` launches the staged profile build with Administrator rights
and otherwise matches `start-des-profile.ps1`. With it, the same capture yielded **369,969 callstack
samples** and **184,886 context switches**.

## What the CPU is doing

Measured on the parked Nexus scene with commit `c3012c9`, Windows Performance Recorder
(`_Build/capture-wpr.ps1`), read through PerfView:

| Module | Exclusive |
| --- | --- |
| `<<?!?>>` — no registered module | **86.8%** |
| `ntoskrnl` | 1.7% |
| `vcruntime140` | 1.7% |
| `ntdll` | 1.6% |
| `ucrtbase` | 1.3% |
| `nvoglv64` | 0.7% |
| `nvlddmkm.sys` | 0.1% |

`<<?!?>>` is **guest game code**. `RuntimeLinker` maps the Demon's Souls ELF by hand, so Windows
never registers it as a module and has no unwind data for it; PerfView reports the stack as `BROKEN`
and the address as unattributable. About **twelve threads each hold 46,000–51,000 ms of CPU over a
51-second trace**, i.e. roughly 100% of a core apiece.

The emulator's own code is around 13% of the process. The descriptor path that the previous task
optimized measures **0.1%** (`EvaluateRuntimeSourcesImpl`) and **0.0%** (`MaterializeResources`) of
whole-process CPU.

## That does not make the render path the wrong target

The guest threads are almost certainly spinning rather than working. Demon's Souls spins its job
system workers; a frame that takes about 125 ms instead of 16 ms makes them spin roughly eight times
longer. The CPU burn is a consequence of the frame rate, not its cause.

Two independent measurements support this.

- CPU falls as frame rate rises. The BDA-cache build measured 6.3575 FPS at **13.110** CPU
  core-equivalents; the evaluation-memo build measured 7.8826 FPS at **12.622**. Fixed work per
  frame would have increased CPU when frames increased.
- Page faults are negligible at **628/sec**, so page-protection dirty tracking is not the cost.
  Context switches run at **66,242/sec** with 13 threads Running and 64 Waiting, which is the
  signature of waiting, not of computation.

So both views are true at once. Whole-process CPU is dominated by guest spin, which is a symptom;
on the render critical path — `Thread_Gpu`, which carries **41%** of all callstack samples and which
Tracy's zone data measures correctly — `MaterializeResources` is **17.0%** inclusive and
`EvaluateRuntimeSourcesImpl` **15.5%**.

**Steer by frame rate, not by whole-process CPU share.** The critical path remains the lever.

## Corrections to earlier readings

- An earlier reading of these captures reported "60% of CPU in the kernel". That came from Tracy's
  client-side symbolisation, which cannot resolve kernel or guest frames and had folded unattributed
  guest addresses into an unresolved export named `Ordinal7` (50% of samples, self ≈ inclusive).
  PerfView with kernel PDBs shows the bulk is guest code, not kernel.
- The `DesTouchTrace` `UD2` probes were suspected of driving that kernel time. Exception machinery
  (`_NLG_Return2`, `RtlVirtualUnwind2`) totals about **3.4%** of samples: real, but not dominant.
  Disabling the probes entirely also prevented the game from reaching gameplay, so the collision
  workaround appears load-bearing and cannot simply be removed to reclaim that cost.

## Symbol names inside the emulator are unreliable

The build uses `-flto=thin` with identical code folding, so the symboliser frequently attributes
samples to a neighbouring or folded name. Two entries that looked like hotspots were artifacts:

| Reported | Actually |
| --- | --- |
| `std::deque<UniqueFunction<void>>::_Tidy`, 2.0% | The GPU thread's entry point. It sits directly under `ucrtbase → kernel32 → ntdll → Thread` and its callee is `GuestGpu::GuestGpu`. |
| `InitDecoder`, 1.7% | A leaf under `BROKEN` on a 46,758 ms guest thread with no callees — the nearest known symbol to an unsymbolised guest address. |

Trust the **shape of a call chain**, not an individual frame's name. Confirm a suspected hotspot in
the source before acting on it.

## Open finding: per-draw binding allocation

`RenderExecutor::PrepareBindings` returns a `PreparedBindings` by value and constructs a fresh one
on every draw, for every stage — `descriptors.cpp` for vertex and pixel, `renderCompute.cpp` for
compute. Each object owns five vectors, and every `TextureBinding` inside it owns a `mip_views`
vector, so a stationary Nexus scene performs on the order of ten million heap operations per
twenty-second window.

Heap traffic corroborates across both captures: `RtlAllocateHeap` plus `RtlFreeHeap` is **2.8%** of
Tracy samples, and `ntdll` plus `ucrtbase` is **2.9%** exclusive in the WPR profile.

Pooling the objects per executor and reusing them across draws would remove this; `clear()` retains
capacity, so the steady state would allocate nothing. Not yet implemented.

## Reproducing

Tooling lives in the ignored `_Build` folder alongside the existing profiling scripts.

| Script | Purpose |
| --- | --- |
| `start-des-profile-elevated.ps1` | Launch the staged profile build elevated so ETW sampling works. `-Restart` stops a running instance first; `-NoTouchSerialize` / `-NoTouchTrace` control the collision probes. |
| `capture-wpr.ps1` | Record a WPR CPU trace of the running emulator. |
| `des-navigate.ps1` | Drive the intro to gameplay with Cross presses. Paces off the window title's frame counter; `-Presses`/`-GapSeconds` paces by wall clock instead, which menu animations need. |
| `des-window.ps1` | Locate, focus, screenshot the game window. |
| `profiling-tools/perfview/PerfView.exe` | Read the WPR trace. Microsoft-signed, downloaded from the project's GitHub releases. |
| `profiling-tools/analyzer-src` | Tracy trace analyser, extended here to aggregate samples per module and per thread. Build it with MSVC, not clang. |

Elevation has two consequences worth knowing before repeating this. A medium-integrity session
cannot send input to, raise, or terminate the elevated emulator, because Windows UIPI blocks it —
`des-navigate.ps1` self-elevates for exactly this reason. And PerfView's headless mode fails here:
symbol matching raises a WPF resource error in a non-interactive session, because the executable is
named `kyty_emulator-profile.exe` while its embedded PDB name is `kyty_emulator.pdb`, which triggers
a trust prompt that cannot be rendered. Use the PerfView GUI to read the trace.

The disk Vulkan pipeline cache only persists across a **clean** exit. Every session recorded here
ended in a forced termination, so the cache on disk is still stale and each run recompiled shaders
from cold.

## Evidence

`_Runtime/_Diagnostics/memo-sampled`

| File | Contents |
| --- | --- |
| `nexus-cpu.etl` | 721 MB WPR CPU trace with kernel stacks |
| `memo-steady.tracy` | Tracy trace, 369,969 callstack samples |
| `sample-profile-v2.txt` | Per-module and per-thread sample breakdown |
| `memo-steady-clean.json` | 7.8826 FPS, 12.622 CPU core-equivalents, 19.9% GPU |
| `perfview-export/` | PerfView stack export used for the call-chain checks |

`_Build/symbols` holds the cached kernel PDBs; PerfView's symbol path is pinned to it so a run does
not stall probing the symbol server for unrelated drivers.
