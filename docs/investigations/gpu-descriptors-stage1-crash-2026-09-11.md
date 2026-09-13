# Stage 1 materialization failure: a GPU-produced null V#

Date: 2026-09-11. Build `e163754` plus the instrumentation committed alongside this note.
Setting: `--gpu-descriptors true` (docs/gpu-descriptor-fetch.md, stage 1).

## Symptom

```
shader resource materialization failed: stage=4 hash=0xfbcf030a4c3f0725
reason=a descriptor source did not evaluate
```

Fatal, non-deterministic, around the save load or shortly after reaching the Nexus. Three of five
runs in this session died this way; a fourth died with `vkWaitSemaphores: ErrorDeviceLost`, which is
the same corruption showing up on the GPU instead. Never happens with the setting off.

## What was instrumented

All of it is inert unless `--gpu-descriptors` is on, or only reachable on the fatal path.

- `SrtWalker.{h,cpp}`: `SrtFailure` / `LastSrtFailure()` / `FormatSrtFailure()`. `FlatMachine::Read`
  remembers the address of the current read; `FlatMachine::Run` fills the record when `Execute`
  fails: failing slot (descriptor source or flat-SRT read), root extent, failing instruction with
  op, immediate, operand registers and their values, the decoded V# fields of a `ReadBuffer`, the
  address, the user-data registers the root reads, and the root's whole schedule with the value and
  address of every step. Purely a failure-path cost.
- `ResourceMaterialization.{h,cpp}`: `MaterializeReport` carries the formatted record, the failing
  address, every address the root read, and the list of sources the `gpu_fetch` override skipped.
- `pipelineCache.cpp` `ReportMaterialization`: prints all of that and, for every walked address,
  the cache view of its page, then `fflush(stdout)` before the existing `EXIT`.
- `kernel/memory.cpp` `DescribeGpuAddress()`: the dword the CPU would read, the BDA page-table
  mapping, the owning cached buffer, GPU/CPU dirty state, fault history for the page and the
  recent shader writers of the address.
- `faultManager.{h,cpp}`: a 512-entry ring plus a persistent map of every page that has ever
  faulted (pass index, first and last), and a line every 60 fault-processing passes with the pages
  processed. `bufferCache.{h,cpp}`: `ProbeAddress()` and a 4096-entry ring of shader stages that
  could have written guest memory, recorded from `RebindBuffers` and `PrepareBindings`.

## The exact failing read

Same shape in all three failing runs; addresses below are from the last one (`crashdiag6`).

```
gpu-descriptors: materialization failure stage=4 hash=0xfbcf030a4c3f0725 skipped_sources=[]
  slot=flat-read[57] evaluator=flat root=[471..481) root_valid=1
  inst=#92 op=ReadBuffer imm=0x20 clean=0 arg0=r81(0x0) arg1=r82(0x0) arg2=r83(0x0) arg3=r2(0x0)
  arg4=r84(0x0) base=0x0 byte_offset=0x20 stride=0 records=0 bound=0x0 address=0x20
  user_data_size=2 trace(10)
     #5 UserData imm=0x0 clean=0 -> 0xa6423ef8
     #6 UserData imm=0x1 clean=0 -> 0x3
     #2 Imm imm=0x0 clean=0 -> 0x0
     #79 ReadAddress imm=0x10 a0=r5(0xa6423ef8) a1=r6(0x3) a2=r2(0x0) @0x3a6423f08 -> 0xfa7a1c0
     #80 ReadAddress imm=0x14 a0=r5(0xa6423ef8) a1=r6(0x3) a2=r2(0x0) @0x3a6423f0c -> 0x2
     #81 ReadAddress imm=0x10 a0=r79(0xfa7a1c0) a1=r80(0x2) a2=r2(0x0) @0x20fa7a1d0 -> 0x0
     #82 ReadAddress imm=0x14 a0=r79(0xfa7a1c0) a1=r80(0x2) a2=r2(0x0) @0x20fa7a1d4 -> 0x0
     #83 ReadAddress imm=0x18 a0=r79(0xfa7a1c0) a1=r80(0x2) a2=r2(0x0) @0x20fa7a1d8 -> 0x0
     #84 ReadAddress imm=0x1c a0=r79(0xfa7a1c0) a1=r80(0x2) a2=r2(0x0) @0x20fa7a1dc -> 0x0
    *#92 ReadBuffer imm=0x20 a0=r81(0x0) a1=r82(0x0) a2=r83(0x0) a3=r2(0x0) a4=r84(0x0) @0x20
         -> undef user_data=s0=0xa6423ef8,s1=0x3
```

Read it bottom up. The SRT root starts at the user-data pointer `0x3a6423ef8`, reads a pointer
`0x20fa7a1c0` from it, then reads the four V# dwords at `0x20fa7a1d0..0x20fa7a1dc`. **All four are
zero.** A zero V# has `num_records = 0`, so `SrtFlatOp::ReadBuffer` rejects byte offset `0x20`
against a bound of `0`, the root fails, and materialization is fatal.

`skipped_sources=[]`: this materialization had no `gpu_fetch` override at all — it is a plain, full
CPU walk. The flat-program skip (`EvaluateWithFlatProgram` / `skip_sources`) is not involved, which
rules out hypothesis (c).

## Page state of the null V#

```
gpu-descriptors: walked read address=0x20fa7a1d0 backing=0x00000000 page=0x83e9e bda_mapped=1
  buffer=1 range=[0x20fa78000,0x20fa80000) deleted=0 covers_dword=1 gpu_modified=0 cpu_modified=0
  gpu_dirty=0 faulted=0 fault_hits=0 fault_first_pass=0 fault_last_pass=0 fault_ring=512
  fault_passes=35022 fault_pages_total=15489 fault_pages_known=15326
  writers=[ buf:81bb2b1b9c751eca/stage4/gpu_fetch=1@[0x20fa7a1c0,0x20fa7a220)
            dma:9d43ffaa0e273d05/stage4/gpu_fetch=1 dma:94361772e61cd622/stage4/gpu_fetch=1
            dma:d5b8994b4a5343d6/stage4/gpu_fetch=1 dma:2547f624d13368c7/stage4/gpu_fetch=1
            dma:12231ae8f12d4240/stage4/gpu_fetch=1 dma:700997d78e310dfd/stage4/gpu_fetch=1
            dma:b7219a7143bd3606/stage4/gpu_fetch=1 dma:2526bbd6d6de7aae/stage4/gpu_fetch=1 ]
```

- `bda_mapped=1`, `covers_dword=1`: the 16 KiB page **is** in the BDA page table and inside a
  registered cached buffer.
- `faulted=0` against `fault_pages_known=15326`: the persistent fault map holds every page that has
  ever faulted in the session, and this page is not one of them. It has never been touched while
  unmapped.
- `gpu_modified=0`, `gpu_dirty=0`, `cpu_modified=0`: nothing is pending in either direction. The
  guest backing genuinely holds zeros; this is not a missed sync.
- The two levels above it (`0x3a6423f08`, `0x3a6423f0c`, page `0xe9908`) read back correct data, so
  the SRT chain itself is intact — only the leaf descriptor block is null.

## The writer

The only recorded writer of that block is compute shader **`0x81bb2b1b9c751eca`**, bound as a
written storage buffer over exactly `[0x20fa7a1c0, 0x20fa7a220)` — a 0x60-byte descriptor block —
and its stage has **`gpu_fetch=1`** (`ShaderInfo::gpu_descriptors`, so it evaluates some of its own
buffer V#s in-shader through the BDA page table).

It is not a DMA writer: the store goes through a bound Vulkan storage buffer, so it cannot have been
dropped by an unmapped BDA page. Whatever it wrote, the CPU reads back.

Reproduced in every failing run, with the block moving inside the same 32 KiB region:

| run | V# block | failing read | page | writer |
| --- | --- | --- | --- | --- |
| crashdiag2 | `0x20fa7d400` | `0x20fa7d410` | `0x83e9f` | (writer ring not yet instrumented) |
| crashdiag4 | `0x20fa7d1c0` | `0x20fa7d1d0` | `0x83e9f` | `0x81bb2b1b9c751eca` buf `[0x20fa7d1c0,0x20fa7d220)` |
| crashdiag6 | `0x20fa7a1c0` | `0x20fa7a1d0` | `0x83e9e` | `0x81bb2b1b9c751eca` buf `[0x20fa7a1c0,0x20fa7a220)`, `gpu_fetch=1` |

## Verdict

**Hypothesis (a) is refuted.** The page is mapped in the BDA page table, has never faulted, and the
writer is not a BDA store. No write was dropped.

**Hypothesis (b) is refuted.** `gpu_dirty=0` and `gpu_modified=0`: there was nothing for the CPU
read path to sync. The guest backing is genuinely zero.

**Hypothesis (c) is refuted.** `skipped_sources=[]` — the failing materialization is a full CPU
walk with no roots skipped, so the flat-program skip cannot have corrupted it.

**(d), a different mechanism.** The zeros are *produced*, not lost. The corrupt descriptor block is
written by a `gpu_fetch` compute shader. Stage 1 specifies that a shader whose BDA read misses the
page table reads **zero** and sets a fault bit (docs/gpu-descriptor-fetch.md, "Design / Stage 1",
point 4, and the spike's known deviation "a shader cannot fail a root, so invalid roots read as
zero"). For an ordinary consumer shader that costs one wrong frame. For `0x81bb2b1b9c751eca`, which
*produces* descriptors into guest memory, a zero read is stored as a **null V#**, and that null V#
outlives the frame. A later dispatch of `0xfbcf030a4c3f0725` walks the same block on the CPU, where
the walker cannot read zero: `ReadBuffer` rejects `num_records = 0` and materialization is fatal.

The one-frame-late rule is also not a startup effect in this scene. Over the 35,022 fault-processing
passes of `crashdiag6` the emulator processed 15,489 fault pages covering 15,326 distinct pages,
averaging 26 pages per 60 passes and still spiking to 137–473 while parked, so a producer shader has
many chances to observe a miss.

### Consequences for the design

- The error is not in the walker; making the CPU walker tolerant of a null V# would hide a real data
  corruption (the GPU stored a descriptor the guest never wrote).
- A shader that stores to guest memory must not be allowed to read zero on a page-table miss. Either
  such shaders stay on the CPU path (a `gpu_fetch` shader with any `written` buffer resource), or a
  missing page must suppress the store rather than store a zero.
- `ErrorDeviceLost` in the fourth run is very likely the same null descriptor reaching a shader that
  dereferences it, rather than a separate bug.

## Raw excerpt

`_Runtime/_Diagnostics/flat-plan/crashdiag6-console.log`, tail:

```
gpu-descriptors: fault pages 137 in the last 60 processing passes (total 15026 over 34980)
gpu-descriptors: materialization failure stage=4 hash=0xfbcf030a4c3f0725 skipped_sources=[] slot=flat-read[57] evaluator=flat root=[471..481) root_valid=1 inst=#92 op=ReadBuffer imm=0x20 clean=0 arg0=r81(0x0) arg1=r82(0x0) arg2=r83(0x0) arg3=r2(0x0) arg4=r84(0x0) base=0x0 byte_offset=0x20 stride=0 records=0 bound=0x0 address=0x20 user_data_size=2 trace(10)
     #5 UserData imm=0x0 clean=0 -> 0xa6423ef8
     #6 UserData imm=0x1 clean=0 -> 0x3
     #2 Imm imm=0x0 clean=0 -> 0x0
     #79 ReadAddress imm=0x10 clean=0 a0=r5(0xa6423ef8) a1=r6(0x3) a2=r2(0x0) @0x3a6423f08 -> 0xfa7a1c0
     #80 ReadAddress imm=0x14 clean=0 a0=r5(0xa6423ef8) a1=r6(0x3) a2=r2(0x0) @0x3a6423f0c -> 0x2
     #81 ReadAddress imm=0x10 clean=0 a0=r79(0xfa7a1c0) a1=r80(0x2) a2=r2(0x0) @0x20fa7a1d0 -> 0x0
     #82 ReadAddress imm=0x14 clean=0 a0=r79(0xfa7a1c0) a1=r80(0x2) a2=r2(0x0) @0x20fa7a1d4 -> 0x0
     #83 ReadAddress imm=0x18 clean=0 a0=r79(0xfa7a1c0) a1=r80(0x2) a2=r2(0x0) @0x20fa7a1d8 -> 0x0
     #84 ReadAddress imm=0x1c clean=0 a0=r79(0xfa7a1c0) a1=r80(0x2) a2=r2(0x0) @0x20fa7a1dc -> 0x0
    *#92 ReadBuffer imm=0x20 clean=0 a0=r81(0x0) a1=r82(0x0) a2=r83(0x0) a3=r2(0x0) a4=r84(0x0) @0x20 -> undef user_data=s0=0xa6423ef8,s1=0x3
gpu-descriptors: failing read address=0x20 backing=unreadable page=0x0 bda_mapped=0 buffer=0 range=[0x0,0x0) deleted=0 covers_dword=0 gpu_modified=0 cpu_modified=1 gpu_dirty=0 faulted=0 fault_hits=0 fault_first_pass=0 fault_last_pass=0 fault_ring=512 fault_passes=35022 fault_pages_total=15489 fault_pages_known=15326 writers=[ dma:9d43ffaa0e273d05/stage4/gpu_fetch=1 ... ]
gpu-descriptors: walked read address=0x3a6423f08 backing=0x0fa7a1c0 page=0xe9908 bda_mapped=1 buffer=1 range=[0x3a6400000,0x3a6758000) deleted=0 covers_dword=1 gpu_modified=0 cpu_modified=0 gpu_dirty=0 faulted=0 ...
gpu-descriptors: walked read address=0x3a6423f0c backing=0x00000002 page=0xe9908 bda_mapped=1 buffer=1 range=[0x3a6400000,0x3a6758000) deleted=0 covers_dword=1 gpu_modified=0 cpu_modified=0 gpu_dirty=0 faulted=0 ...
gpu-descriptors: walked read address=0x20fa7a1d0 backing=0x00000000 page=0x83e9e bda_mapped=1 buffer=1 range=[0x20fa78000,0x20fa80000) deleted=0 covers_dword=1 gpu_modified=0 cpu_modified=0 gpu_dirty=0 faulted=0 fault_hits=0 fault_first_pass=0 fault_last_pass=0 fault_ring=512 fault_passes=35022 fault_pages_total=15489 fault_pages_known=15326 writers=[ buf:81bb2b1b9c751eca/stage4/gpu_fetch=1@[0x20fa7a1c0,0x20fa7a220) dma:9d43ffaa0e273d05/stage4/gpu_fetch=1 dma:94361772e61cd622/stage4/gpu_fetch=1 dma:d5b8994b4a5343d6/stage4/gpu_fetch=1 dma:2547f624d13368c7/stage4/gpu_fetch=1 dma:12231ae8f12d4240/stage4/gpu_fetch=1 dma:700997d78e310dfd/stage4/gpu_fetch=1 dma:b7219a7143bd3606/stage4/gpu_fetch=1 dma:2526bbd6d6de7aae/stage4/gpu_fetch=1]
gpu-descriptors: walked read address=0x20fa7a1d4 backing=0x00000000 page=0x83e9e bda_mapped=1 ... (same)
gpu-descriptors: walked read address=0x20fa7a1d8 backing=0x00000000 page=0x83e9e bda_mapped=1 ... (same)
gpu-descriptors: walked read address=0x20fa7a1dc backing=0x00000000 page=0x83e9e bda_mapped=1 ... (same)
--- Error ---
shader resource materialization failed: stage=4 hash=0xfbcf030a4c3f0725 reason=a descriptor source did not evaluate
 in C:\Users\dudu\Projects\KytyPS5\src\graphics\host_gpu\renderer\pipeline\pipelineCache.cpp:191
```

Logs: `_Runtime/_Diagnostics/flat-plan/crashdiag{1,2,4,6}-console.log` (materialization failure),
`crashdiag3-console.log` (`ErrorDeviceLost`). `_Runtime` is not in git.
