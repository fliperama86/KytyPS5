# Render thread (`Thread_Gpu`, tid 40300) kernel time — 2026-09-10

**Question.** Tracy's sampler reported 26% "kernel" for `Thread_Gpu`; PerfView's process-wide
module table shows `ntoskrnl` at 1.7%. Which is right, and is a kernel hotspot hiding there?

**Answer: no hidden kernel hotspot.** Both numbers are right about different things. True kernel
time on the render thread is **10.4%** (4,002 ms of 38,537 ms), and it is dominated by the two
calls you would expect a GPU thread to make — command submit and fence wait.

## The numbers

Source: `_Runtime/_Diagnostics/memo-sampled/perfview-export/nexus-cpu.View1.perfView.xml` — WPR CPU trace, 5,347,790 samples, 668,474 ms, 0.125 ms/sample.

| Scope | Total | ntoskrnl+`*.sys` | OS user DLLs | Combined |
|---|---|---|---|---|
| Process 5856 (whole) | 668,474 ms | 1.90% | 3.4% | 5.3% |
| Thread 40300 `Thread_Gpu` | 38,537 ms | **10.38%** (4,002 ms) | **14.22%** (5,482 ms) | **24.61%** |

"OS user DLLs" = `ntdll` / `win32u` / `kernelbase` / `kernel32` leaves. These are user mode, but a
sampler that classifies frames as "my binary vs not" counts them as kernel — which is how **Tracy
gets 26%: our 24.61% combined figure is the same measurement.**

**PerfView's 1.7% is a process-wide average, diluted to meaninglessness.** The trace has 96.2%
broken stacks overall: process-wide `?!?` (unwalkable) is 86.80% exclusive and `ntoskrnl` 1.72% —
both reproduced exactly by the script, which validates the parser. That 86.8% comes from the 13
emulator worker threads (~47,000 ms each) whose stacks never walk. The render thread is the
healthy one: only 0.07% `?!?` exclusive, so its per-thread numbers are trustworthy.

**Top 5 kernel leaves (thread 40300):** `ntoskrnl!?` 3,230.9 ms (8.38% of thread), `nvlddmkm.sys!?`
320.6 ms (0.83%), `dxgkrnl.sys!?` 247.5 ms (0.64%), `dxgmms2.sys!?` 147.2 ms (0.38%),
`watchdog.sys!?` 38.2 ms (0.10%). No kernel symbols resolved anywhere in this trace — every OS
frame is `module!?`, so "which kernel function" is unanswerable; only module and caller are.

**Top 5 user-mode entry points into the kernel** (% of the thread's 4,002 ms kernel time):

| ms | % kernel | entry | nearest app call site |
|---|---|---|---|
| 1,818.1 | 45.4% | `win32u!?` | `CommandScheduler::Submit`, `MasterSemaphore::Wait` |
| 1,418.5 | 35.4% | `<BROKEN>` | unattributable — stack walk failed |
| 579.4 | 14.5% | `ntdll!?` | `GuestAddressSpace::ProtectMappedUnlocked` |
| 88.5 | 2.2% | `kyty!monotonic_buffer_resource::'vector deleting dtor'` | (LTO-folded symbol) |
| 42.9 | 1.1% | `vcruntime140!?` | — |

Resolved to our own code, kernel time splits: `CommandScheduler::Submit` 836.8 ms (20.9%),
`MasterSemaphore::Wait` 822.6 ms (20.6%), `GuestAddressSpace::ProtectMappedUnlocked` 401.2 ms
(10.0%), `VmaAllocator_T::GetHeapBudgets` 96.4 ms, `VmaAllocator_T::FreeDedicatedMemory` 64.5 ms.

## Reading

- **Submit + fence wait = 41% of kernel time (1,659 ms, 4.3% of the thread).** Normal
  `win32u` -> `dxgkrnl` -> `nvlddmkm` submission path. Expected cost, not a hotspot.
- **`ProtectMappedUnlocked` = 401 ms (1.0% of the thread)** is the one avoidable item:
  `NtProtectVirtualMemory` churn from guest page-protection tracking, issued on the render thread.
  Small, but pure overhead and ours. `ProtectGuestHostMemory` adds 33 ms.
- **The bigger user-mode finding is allocator traffic**, not kernel: `operator new` is 1,047.5 ms
  (19.1% of OS-user-DLL time, ~2.7% of the thread) in the `ntdll` heap, then `PrepareProgram`
  249.5 ms and `PreparedBindings::~PreparedBindings` 227.6 ms.
- **35% of the thread's kernel time (1,418 ms) has no recoverable caller** — 40.8% of this thread's
  samples carry a `BROKEN` marker, so call-site attribution is a lower bound. Leaves are still
  sound (only 28 ms of `?!?` exclusive).

## Caveat: symbol names are unreliable in this build

This is an LTO build with identical-COMDAT folding, so PerfView attributes samples to whichever
symbol owns the folded code. The evidence is blatant: process-wide the #1 app leaf is
`kyty_emulator-profile!InitDecoder` at 11,253 ms, and the render thread's #1 leaf is
`std::pmr::monotonic_buffer_resource::'vector deleting destructor'` at 5,838 ms (15.2%). Neither is
real. **Trust the call-chain shape and module boundaries, not individual leaf names.** Conclusions
above are unaffected: module attribution comes from the address range, not the symbol.

## Re-running

`python _Build/profiling-tools/perfview_thread_breakdown.py <export.xml> --thread 40300` — ~4 min
(streaming parse, progress every 1M samples). `--list-threads` lists threads by CPU, `--thread TID`
selects one, `--process PID` picks the process, `--top N` sets rows per table. The process-wide
table always prints; confirm it still reads `?!?` 86.80% / `ntoskrnl` 1.72% before trusting the
per-thread numbers.
