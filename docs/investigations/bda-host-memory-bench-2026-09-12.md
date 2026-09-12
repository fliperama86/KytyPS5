# BDA reads from host memory: the bench for design A (2026-09-12)

Design A in [bda-sync-design.md](../bda-sync-design.md) ("Parked: A, no mirror for BDA-read pages")
points the BDA page table at guest memory imported with `VK_EXT_external_memory_host` instead of at
a device-local mirror. For the pages it covers there is then no scan, no fault, no protection and
no copy. The open question was the GPU-side price: the prologue's SRT walk is one to three
dependent uniform loads per root, several roots per wave, about 9,300 draws a frame, and under A
every one of those loads comes from system memory over PCIe instead of from VRAM.

[bda-load-bench-2026-09-11.md](bda-load-bench-2026-09-11.md) measured that on a device-local
fixture only and concluded "the GPU is not the wall". This is the same bench with the fixture moved
to host memory.

**Answer in one line.** Imported host memory is 2.5x the latency of a warm VRAM mirror per
dependent read and 42x its cost per cache line, but the GPU hides dependent-read latency almost
perfectly across waves, so the game's descriptor mix costs **0.17 ms a frame** added GPU time at
the overlapped bound and 10.1 ms at the fully serialized one, and the evidence says the overlapped
bound is the right one. A is viable for the prologue's descriptor reads and is not viable as a
blanket policy for pages a shader streams data from.

## What was added

`tests/BdaLoadBench.cpp` and `tests/bda_load_bench.comp`, target `bda_load_bench`
(`EXCLUDE_FROM_ALL`).

**Fixture kinds**, `--fixture <device-local|host-import|host-visible>`:

- `device-local` is the original bench: the 257 MB fixture in VRAM. Baseline.
- `host-import` commits the fixture with `VirtualAlloc` (reserve, then commit at an address rounded
  up to `VkPhysicalDeviceExternalMemoryHostPropertiesEXT::minImportedHostPointerAlignment`, size
  rounded up the same way), imports it with `VkImportMemoryHostPointerInfoEXT` and handle type
  `VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT`, picks the memory type from
  `vkGetMemoryHostPointerPropertiesEXT`, and binds it to a `VkBuffer` created with
  `VkExternalMemoryBufferCreateInfo` for that handle type plus `SHADER_DEVICE_ADDRESS` and the
  storage and transfer usage the bench needs. `VK_EXT_external_memory_host` is enabled at device
  creation when the device reports it. This is design A's shape exactly.
- `host-visible` is the control: a plain `HOST_VISIBLE | HOST_COHERENT` allocation that is not
  `DEVICE_LOCAL`. Same system memory, no import.

All three are filled by the same `vkCmdFillBuffer` and `vkCmdCopyBuffer` commands, so the fixture
is bit-identical and the existing checksum check compares the same constant in every kind. It
passed everywhere: 0 fault words, 0 of 256 sink values wrong, `sink[0] = 0x82928080` in all three
runs.

On this machine `VK_EXT_external_memory_host` is present, `minImportedHostPointerAlignment` is
**4096 bytes**, and both host kinds land on the same memory type: **type 2, heap 1 (31561 MB),
`HOST_VISIBLE | HOST_COHERENT`**. The import and the plain allocation are therefore the same kind
of memory by every property Vulkan exposes, which makes the difference between them (below) a
property of the import itself.

**New pattern, `chain-latency`.** One workgroup of 64 threads walks a dependent chain of N loads:
the address of step i+1 is derived from the value step i loaded, so nothing overlaps and the
dispatch time divided by N is the latency of one dependent guest read. The chain is lane uniform,
which is the shape of a prologue's scalar SRT walk. The addresses are scattered over the whole
256 MB region: for N = 1024 that is 1024 distinct offsets, 1024 distinct 64-byte lines, 1001
distinct 16 KB pages and no two consecutive steps in the same page. N = 64 and N = 1024 are both
run so the fixed dispatch overhead separates; the `slope` column is
`(T(1024) - T(64)) / (1024 - 64)`, which removes it.

The chain is built from the loaded value rather than from stored pointers - `off = hash(value, i)` -
so the fixture keeps the single constant everywhere and the bit-exact check still applies. The
dependency is real in the machine code (the next address cannot be formed until the load retires)
even though every load returns the same word; what it does not model is a chain whose *values*
differ, which no hardware on this path cares about.

**Two passes.** Every cell now runs twice back to back in the same command buffer over the same
addresses, separated only by an execution barrier, and both are timed. For `chain-latency` an
untimed scattered dispatch (8192 groups, 8.4 M random loads over the region) runs first and evicts
the caches, so pass 1 is a genuinely cold read and pass 2 a warm one. The throughput patterns
stream far more than any cache holds and need no eviction.

**Two overlap probes**, to decide between the serialized and overlapped projections:

- the same chain in 1, 16, 128 and 1024 workgroups (independent chains, one per workgroup);
- one dispatch of 16384 groups against the same work as 64 dispatches of 256 groups, with and
  without barriers between them.

`--runs N` folds the repetitions into the process and reports the minimum over all of them, so the
15 samples of the original method (3 runs x 5) come out of one invocation per fixture.

Hardware, unchanged: NVIDIA GeForce RTX 5090, driver 616.64.0, Vulkan 1.4.351, `timestampPeriod`
1.0 ns, Ryzen 9 9950X3D, Windows 11.

## Throughput: ps per load, minimum over 15 samples

`ps / load` is GPU time divided by the total data loads issued across the device: a throughput
figure, not a latency. Pass 1 and pass 2 are the two passes of the same command buffer.

| pattern | variant | device-local p1 | p2 | host-import p1 | p2 | host-visible p1 | p2 | import / VRAM |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| uniform | A ssbo | 0.13 | 0.13 | 3.94 | 3.94 | 3.92 | 3.92 | 30x |
| uniform | B bda | 0.41 | 0.41 | 3.96 | 3.99 | 3.99 | 3.98 | 9.7x |
| uniform | C bda+srt | 0.59 | 0.60 | 4.05 | 4.03 | 4.01 | 4.02 | 6.9x |
| strided | A ssbo | 1.04 | 1.06 | 457.02 | 461.95 | 472.78 | 460.82 | 439x |
| strided | B bda | 1.26 | 1.26 | 474.18 | 459.74 | 467.89 | 462.81 | 376x |
| strided | C bda+srt | 1.32 | 1.32 | 424.02 | 434.76 | 459.75 | 467.34 | 321x |
| scattered | A ssbo | 47.57 | 47.90 | 2038.96 | 2051.20 | 2038.39 | 2050.09 | 43x |
| scattered | B bda | 48.13 | 47.79 | 2039.82 | 2050.90 | 2050.34 | 2050.78 | 42x |
| scattered | C bda+srt | 47.74 | 48.07 | 2040.46 | 2052.96 | 2040.20 | 2050.30 | 43x |

The device-local column reproduces the September 11 run (0.41 / 0.59 / 1.26 / 48.1 against 0.41 /
0.60 / 1.23 / 46.9), so the two-pass command buffer did not change the baseline.

Reading it:

1. **Both host kinds are the same for throughput.** Every cell is within a few percent of its
   opposite number. The import costs nothing in bandwidth, and nothing in bandwidth is gained by
   avoiding it.
2. **The page table stops mattering.** On host memory the BDA variant is 1.00x to 1.02x the bound
   SSBO on every pattern and the SRT prologue adds 0.4 to 1.5 ps per invocation, against 2.85 ps on
   VRAM. The page-table entry is a device-local L2 hit hidden behind a read that now takes ten to
   four hundred times longer.
3. **Pass 2 never helps.** For the streaming patterns that is expected: they walk 256 MB, which no
   cache holds. It is the chain that answers the caching question.
4. **The scale of the penalty depends entirely on the access shape.** Lane-uniform reads, where a
   wave's 16 loads fall in one 64-byte line, cost 30x. Reads that fetch a fresh line per lane cost
   321x to 439x, because they are bandwidth bound and the fixture is now behind PCIe: the strided
   pattern moves 67 MB per iteration at **8.8 GB/s** on the import, where the same pattern on the
   device-local fixture sustains 3.8 TB/s (most of it out of L2).

Per-invocation cost of the SRT prologue alone, as C minus B (pass 1):

| pattern | device-local | host-import | host-visible |
| --- | ---: | ---: | ---: |
| uniform | 2.85 ps | 1.49 ps | 0.37 ps |
| strided | 0.89 ps | -802 ps | -130 ps |
| scattered | -6.2 ps | 10.3 ps | -163 ps |

On a host fixture this subtraction is noise: the prologue's 40 lines of chain are shared by every wave
and cost nothing measurable next to a body that is 400 to 2000 ps per load. The negative cells are
calibration differences between two cells that each take 30 ms, not a real speed-up.

## chain-latency: ns per dependent load, minimum over 15 samples

One workgroup, one dependent chain, caches evicted before pass 1. `cold` is pass 1, `warm` is
pass 2 over the same 1024 addresses.

| fixture | variant | N=64 cold | N=1024 cold | slope cold | N=64 warm | N=1024 warm | slope warm |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| device-local | A ssbo | 312.5 | 294.8 | **293.7** | 153.5 | 137.9 | **136.9** |
| device-local | B bda | 460.5 | 439.4 | **438.0** | 304.5 | 280.9 | **279.3** |
| device-local | C bda+srt | 1102.5 | 813.6 | **794.3** | 725.0 | 636.7 | **630.8** |
| host-import | A ssbo | 595.0 | 581.2 | **580.3** | 575.5 | 564.3 | **563.5** |
| host-import | B bda | 744.5 | 722.4 | **720.9** | 724.5 | 704.8 | **703.4** |
| host-import | C bda+srt | 1703.0 | 1114.0 | **1074.8** | 1672.0 | 1094.0 | **1055.5** |
| host-visible | A ssbo | 603.5 | 585.8 | **584.7** | 168.0 | 153.2 | **152.2** |
| host-visible | B bda | 752.0 | 729.3 | **727.8** | 318.5 | 295.8 | **294.3** |
| host-visible | C bda+srt | 1712.0 | 1119.2 | **1079.7** | 756.0 | 653.3 | **646.5** |

The N = 64 and N = 1024 columns differ by 2 to 11% for A and B, and the implied fixed overhead
(`T(64) - 64 x slope`) is **0.8 to 1.6 us** in every one of those cells - a dispatch launch and the
timestamp pair - so the slope and the N = 1024 figure agree. For C the two columns differ by 14 to
53% and the intercept is 6 to 40 us, because the SRT chain's own 12 KB is cold on the first
iterations and warm after; the slope is the steady-state number there.

The rows in order:

1. **A dependent read from imported host memory costs 721 ns cold and 703 ns warm.** The warm
   number is the one that matters, and the import barely improves on repetition.
2. **A dependent read from the VRAM mirror costs 438 ns cold and 279 ns warm.** So the import is
   1.65x the mirror when both are cold and **2.52x** when both are warm.
3. **The import is the reason, not system memory.** `host-visible` is the same memory type, the
   same heap, the same PCIe link, and its warm chain costs **294 ns** - within 5% of the VRAM
   mirror's 279 ns, and 2.4x faster than the import's 703 ns. Its cold chain (728 ns) is identical
   to the import's (721 ns). Both are equally slow the first time; only the driver's own allocation
   keeps the lines in L2 across a pipeline barrier. An imported host pointer can be written by the
   CPU at any moment with nothing for the driver to hook, so it does not get to cache it across a
   barrier; that is the price of the import and it is not tunable from here.
4. **Within one dispatch the import is cached normally.** C minus B is 354 ns on `host-import` and
   356 ns on `device-local` - identical. The SRT walk re-reads the same 40 lines of chain on every
   iteration of the same dispatch, and if those were uncached at 570 ns each the difference would
   be about 1700 ns, not 354. So what the barrier drops is the cache, not the cacheability: waves
   of the same draw, and draws between two barriers, share the prologue's lines.
5. **The page table is 140 to 160 ns of every BDA read** (B minus A: 144 cold / 142 warm on VRAM,
   141 / 140 on the import), the same on both sides, because the page table itself stays in VRAM
   under design A.

## How much of that latency does the GPU hide?

Independent chains, N = 1024, `B bda`, one chain per workgroup:

| fixture | 1 group | 16 | 128 | 1024 |
| --- | ---: | ---: | ---: | ---: |
| device-local cold (us) | 450.9 | 452.6 | 430.6 | 422.1 |
| host-import cold (us) | 740.2 | 774.9 | 763.2 | 771.9 |
| host-visible cold (us) | 745.3 | 736.7 | 717.8 | 711.7 |
| host-visible warm (us) | 302.9 | 331.5 | 335.0 | 487.0 |

**1024 independent dependent chains finish in the same wall time as one.** On the import it is
771.9 us against 740.2 us, 4% for 1024x the work. Dependent-read latency is not a throughput cost
on this GPU as long as there is other work in flight.

The same work as one dispatch or as 64 back-to-back dispatches, uniform `C bda+srt`:

| fixture | 1 x 16384 groups | 64 x 256 groups, no barrier | 64 x 256 groups, barriers |
| --- | ---: | ---: | ---: |
| device-local | 7.842 ms | 8.143 ms | 52.814 ms |
| host-import | 6.908 ms | 6.373 ms | 19.242 ms |
| host-visible | 6.611 ms | 1.509 ms | 15.984 ms |

Splitting the work across 64 dispatches costs 4% on the device-local fixture and putting a barrier
between them costs **6.5x**. The host rows are not comparable across the first two columns - the
uniform pattern's stride is derived from `gl_NumWorkGroups`, so the 256-group dispatches walk a
1.6 MB window instead of 256 MB and cache it - but the barrier column is against the same pattern
as the column next to it, and it is 3.0x and 10.6x. Back-to-back work overlaps; barriers are what
serializes.

## What it costs the frame

The mix, from [gpu-descriptor-fetch.md](../gpu-descriptor-fetch.md) ("The chase, counted where it
happens"): about **20,800 stage events a frame**, **1.14 dependent guest reads** an event (depth 0
for 43.0%, depth 2 for 56.5%), **4.03 distinct 64-byte lines** an event, 21.51 guest reads and 8.46
descriptor dwords an event - so the roots' own decode is inside those 4 lines.

**Upper bound, fully serialized across draws.** Every event's dependent chain is paid end to end
and nothing else is in flight. 20,800 x 1.14 = 23,712 dependent reads a frame, each 424 ns more
expensive (703 warm on the import against 279 warm on the mirror):

> **23,712 x 424 ns = 10.1 ms a frame.**

Cold against cold (721 against 438) it is 283 ns each and **6.7 ms a frame**. If the import were
cached the way the driver's own host allocation is (294 against 279), it would be 15 ns each and
**0.36 ms**.

**Lower bound, overlapped across waves in flight.** The latency is hidden and only the line fetches
remain. 20,800 x 4.03 = 83,824 lines a frame, at 2.04 ns a line from the import against 0.048 ns
from VRAM (the scattered `B bda` figures, which are one line per load):

> **83,824 x 1.99 ns = 0.17 ms a frame**, which is 5.4 MB a frame of extra PCIe traffic.

**Which bound is closer: the lower one, by a wide margin.** The chain-parallel probe is direct
evidence: 1024 independent dependent chains take 4% longer than one, so the GPU keeps of the order
of a thousand such chains in flight, and 10.1 ms divided by that is 0.01 ms - below the line-fetch
term, which is therefore the whole cost. The dispatch-split probe says the same from the other end:
64 back-to-back dispatches cost 4% more than one, and only a barrier between them turns that into
6.5x. Draws of a frame overlap the way those dispatches do.

So the honest projection is **0.2 ms a frame, with 10 ms as the number the frame would pay only if
every draw's prologue were the only work on the GPU**. For scale, the parked Nexus frame is 86 to
103 ms with the GPU 72% idle ([sync-points-design.md](../sync-points-design.md)), and what design A
removes is the scan and protect work that
[bda-sync-design.md](../bda-sync-design.md) prices at 20.2 us of syscall per scan, 399 scans a
frame.

**The one risk this projection carries** is the barrier behaviour in point 4 above. The per-event
model is right only because a draw's waves share the prologue's lines in cache; every pipeline
barrier throws them away and the next draw re-fetches at 703 ns instead of 279. The frame has
hundreds of barriers (399 BDA syncs a frame alone), so the real number is somewhere above the
0.17 ms line-fetch bound in proportion to how often the prologue's 474 pages are re-fetched. It
does not reach 10 ms, because reaching that needs every event serialized, not merely cold.

## Verdict on A

**Viable for what it was parked for, and only for that.**

- For the prologue's descriptor and SRT reads - lane-uniform, 4 lines an event, 1.14 dependent
  levels - the added GPU time is a few tenths of a millisecond a frame against the render-thread
  scans, faults and drains it removes. The GPU is not the wall here either.
- It is **not** viable as a blanket policy for every page that is only read through BDA. A page a
  shader streams data from costs 321x to 439x more in the strided shape and 43x in the scattered
  one: 200 million strided reads are 0.25 ms from the mirror and 92 ms from the import. Any program
  that does bulk `uses_dma` reads must keep its mirror. Design A needs a per-page or per-program
  rule that separates "read by the prologue" from "read by the body", and the bench says that rule
  is the whole design, not a detail of it.
- The `host-visible` control is the interesting negative result: system memory itself is not the
  problem. The same memory the driver allocates is within 5% of VRAM once its lines are in L2, and
  the import gives that up. If a later driver caches imported host pointers across barriers, A's
  cost drops from 424 ns a dependent read to 15 ns and the question stops being a question. Worth
  re-running this bench on a driver update before building on the 424.

## Caveats

- The bench imports one 257 MB block. The emulator would import guest pages as the guest commits
  and decommits them (`Common::VirtualMemory::Commit` / `Decommit`), which means many imports,
  re-imports on decommit, and a page-table rewrite each time. None of that bookkeeping is measured
  here; `bda-sync-design.md` already names it as A's main risk and this bench does not reduce it.
- The projection uses the CPU-side mix as a per-event count. It is the right count only because
  point 4 above holds: waves of the same draw share the prologue's lines in cache. Between
  barriers they do not, and the true number sits above the 0.17 ms floor by however often the
  frame's barriers drop them. Measuring that needs the real thing, not this bench.
- `host-import` and `host-visible` differ in one measurement only, the warm chain, and that
  difference is driver policy, not an architectural limit. It reproduced identically in all three
  runs (705 / 706 / 705 against 296 / 296 / 296 ns), but it is the sort of thing a driver release
  can change in either direction.
- The dispatch-split probe's first two columns are not comparable on the host fixtures, because the
  uniform pattern derives its stride from `gl_NumWorkGroups` and the 256-group dispatches therefore
  walk a 1.6 MB window instead of 256 MB. Only its third column against its second is a fair
  comparison there. The chain-parallel probe has no such flaw and is the one the verdict leans on.

## Commands

```
_Build\build-targets.ps1 -Targets bda_load_bench

_Build\windows\bda_load_bench.exe --fixture device-local --runs 3
_Build\windows\bda_load_bench.exe --fixture host-import  --runs 3
_Build\windows\bda_load_bench.exe --fixture host-visible --runs 3
```

`--runs 3` with the built-in 5 samples a cell is the 15 samples the September 11 bench took over
three invocations; the program reports the minimum over all of them in the combined tables at the
end. `--device <n>` picks a GPU.

## Raw output

### --fixture device-local

```
device 0: NVIDIA GeForce RTX 5090
device 1: AMD Radeon(TM) Graphics

selected: NVIDIA GeForce RTX 5090
driver version: 616.64.0, api 1.4.351
timestampPeriod: 1.0000 ns
fixture: device-local
  VK_EXT_external_memory_host: present
  minImportedHostPointerAlignment: 4096 bytes
  guest memory type 1, heap 0, flags: DEVICE_LOCAL
  heap size: 32187 MB
  guest buffer device address: 0x8000000
dispatch: 16384 groups x 64 threads = 1048576 invocations, 16 loads per iteration
guest memory: 257 MB mapped in 16448 pages of 16 KB; page table 512 MB

=== run 1 (device-local) ===

uniform   A ssbo    iterations=13097
  pass 1 (ms): 28.984 29.024 29.893 29.398 29.485
    min 28.984 ms, 0.13 ps/load
  pass 2 (ms): 30.034 29.014 29.233 29.404 29.969
    min 29.014 ms, 0.13 ps/load

uniform   B bda     iterations=3914
  pass 1 (ms): 27.149 27.416 29.122 27.128 27.180
    min 27.128 ms, 0.41 ps/load
  pass 2 (ms): 27.161 27.168 27.188 27.156 27.295
    min 27.156 ms, 0.41 ps/load

uniform   C bda+srt iterations=2754
  pass 1 (ms): 27.882 28.304 28.095 29.702 29.971
    min 27.882 ms, 0.60 ps/load
  pass 2 (ms): 27.997 28.082 28.023 27.970 27.901
    min 27.901 ms, 0.60 ps/load

strided   A ssbo    iterations=1688
  pass 1 (ms): 29.409 29.623 31.090 31.152 31.650
    min 29.409 ms, 1.04 ps/load
  pass 2 (ms): 29.975 32.429 31.122 31.042 31.141
    min 29.975 ms, 1.06 ps/load

strided   B bda     iterations=1216
  pass 1 (ms): 27.297 28.292 27.815 27.844 27.620
    min 27.297 ms, 1.34 ps/load
  pass 2 (ms): 28.087 27.503 27.273 27.751 28.041
    min 27.273 ms, 1.34 ps/load

strided   C bda+srt iterations=1328
  pass 1 (ms): 31.094 31.450 30.680 29.390 29.693
    min 29.390 ms, 1.32 ps/load
  pass 2 (ms): 30.687 30.035 32.254 30.105 30.329
    min 30.035 ms, 1.35 ps/load

scattered A ssbo    iterations=31
  pass 1 (ms): 25.067 25.486 25.071 25.281 25.029
    min 25.029 ms, 48.12 ps/load
  pass 2 (ms): 25.011 25.219 24.991 24.975 25.116
    min 24.975 ms, 48.02 ps/load

scattered B bda     iterations=34
  pass 1 (ms): 29.211 28.926 28.953 29.082 29.223
    min 28.926 ms, 50.71 ps/load
  pass 2 (ms): 29.504 29.302 29.290 29.249 28.399
    min 28.399 ms, 49.79 ps/load

scattered C bda+srt iterations=30
  pass 1 (ms): 24.113 24.592 24.029 24.567 26.334
    min 24.029 ms, 47.74 ps/load
  pass 2 (ms): 24.253 24.285 25.251 24.266 26.109
    min 24.253 ms, 48.19 ps/load

chain     A ssbo    N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 20.93 20.93 20.29 20.90 20.42 21.09 20.32 20.70 21.06
    min 20.29 us, 317.0 ns per dependent load
  pass 2 warm (us): 9.82 9.86 9.86 9.92 9.82 9.86 9.86 9.86 9.89
    min 9.82 us, 153.5 ns per dependent load

chain     B bda     N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 30.69 30.78 31.42 30.05 30.62 30.78 29.92 30.43 30.40
    min 29.92 us, 467.5 ns per dependent load
  pass 2 warm (us): 19.49 19.52 19.97 19.52 19.52 19.49 19.52 19.49 19.52
    min 19.49 us, 304.5 ns per dependent load

chain     C bda+srt N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 72.29 73.18 72.35 72.70 72.03 72.67 72.16 72.67 73.38
    min 72.03 us, 1125.5 ns per dependent load
  pass 2 warm (us): 46.43 46.50 46.43 46.40 46.40 46.50 46.40 46.50 46.43
    min 46.40 us, 725.0 ns per dependent load

chain     A ssbo    N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 309.79 312.54 313.25 311.58 314.02 314.27 313.89 301.92 312.64
    min 301.92 us, 294.8 ns per dependent load
  pass 2 warm (us): 141.41 141.25 141.28 141.76 141.28 141.25 141.25 141.28 141.25
    min 141.25 us, 137.9 ns per dependent load

chain     B bda     N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 456.74 460.93 454.98 449.95 465.15 458.59 456.51 461.50 458.24
    min 449.95 us, 439.4 ns per dependent load
  pass 2 warm (us): 288.06 287.62 287.94 288.19 288.06 288.10 287.74 287.78 287.90
    min 287.62 us, 280.9 ns per dependent load

chain     C bda+srt N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 838.62 840.99 835.52 839.94 836.54 842.66 835.23 1062.18 834.02
    min 834.02 us, 814.5 ns per dependent load
  pass 2 warm (us): 652.61 652.54 652.38 652.42 652.13 652.42 652.35 653.15 652.16
    min 652.13 us, 636.8 ns per dependent load

chain-parallel B bda N=1024
     1 groups: pass1 453.86 us (443.2 ns/dep load), pass2 287.78 us (281.0 ns/dep load)
    16 groups: pass1 456.26 us (445.6 ns/dep load), pass2 308.48 us (301.2 ns/dep load)
   128 groups: pass1 430.66 us (420.6 ns/dep load), pass2 308.35 us (301.1 ns/dep load)
  1024 groups: pass1 428.48 us (418.4 ns/dep load), pass2 309.34 us (302.1 ns/dep load)

dispatch split, uniform C bda+srt, iterations=688
  1 x 16384 groups            : 7.842 ms
  64 x 256 groups, no barrier: 9.109 ms
  64 x 256 groups, barriers  : 57.713 ms

=== run 2 (device-local) ===

uniform   A ssbo    iterations=13123
  pass 1 (ms): 30.554 30.517 29.675 30.878 31.215
    min 29.675 ms, 0.13 ps/load
  pass 2 (ms): 29.881 30.450 31.435 30.844 30.529
    min 29.881 ms, 0.14 ps/load

uniform   B bda     iterations=4127
  pass 1 (ms): 34.009 37.447 31.781 29.497 35.559
    min 29.497 ms, 0.43 ps/load
  pass 2 (ms): 35.292 31.692 35.417 35.172 31.895
    min 31.692 ms, 0.46 ps/load

uniform   C bda+srt iterations=2671
  pass 1 (ms): 32.267 32.571 34.228 33.492 33.417
    min 32.267 ms, 0.72 ps/load
  pass 2 (ms): 30.592 32.579 33.792 33.501 33.390
    min 30.592 ms, 0.68 ps/load

strided   A ssbo    iterations=1614
  pass 1 (ms): 29.146 29.151 29.088 28.518 29.271
    min 28.518 ms, 1.05 ps/load
  pass 2 (ms): 30.719 29.489 29.665 31.510 29.842
    min 29.489 ms, 1.09 ps/load

strided   B bda     iterations=1396
  pass 1 (ms): 29.784 30.051 30.077 29.837 30.285
    min 29.784 ms, 1.27 ps/load
  pass 2 (ms): 30.490 30.322 29.523 29.730 30.703
    min 29.523 ms, 1.26 ps/load

strided   C bda+srt iterations=1384
  pass 1 (ms): 32.286 32.895 31.501 31.338 32.464
    min 31.338 ms, 1.35 ps/load
  pass 2 (ms): 32.698 31.485 31.363 31.575 33.504
    min 31.363 ms, 1.35 ps/load

scattered A ssbo    iterations=36
  pass 1 (ms): 28.948 29.239 29.189 29.020 28.733
    min 28.733 ms, 47.57 ps/load
  pass 2 (ms): 29.227 29.219 29.457 29.448 29.222
    min 29.219 ms, 48.38 ps/load

scattered B bda     iterations=34
  pass 1 (ms): 27.507 28.096 27.729 27.769 27.949
    min 27.507 ms, 48.22 ps/load
  pass 2 (ms): 27.407 27.772 27.415 27.405 27.714
    min 27.405 ms, 48.04 ps/load

scattered C bda+srt iterations=33
  pass 1 (ms): 26.436 26.910 28.037 28.777 28.353
    min 26.436 ms, 47.75 ps/load
  pass 2 (ms): 27.301 26.612 28.593 28.664 28.189
    min 26.612 ms, 48.07 ps/load

chain     A ssbo    N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 20.00 20.86 20.13 20.86 20.77 20.90 20.42 20.90 20.51
    min 20.00 us, 312.5 ns per dependent load
  pass 2 warm (us): 9.86 9.86 9.82 9.82 9.92 9.82 9.86 9.86 9.86
    min 9.82 us, 153.5 ns per dependent load

chain     B bda     N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 30.46 30.24 30.24 29.57 30.56 30.72 30.53 30.53 30.85
    min 29.57 us, 462.0 ns per dependent load
  pass 2 warm (us): 19.52 19.52 19.49 19.52 19.49 19.52 19.49 19.52 19.49
    min 19.49 us, 304.5 ns per dependent load

chain     C bda+srt N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 72.42 72.19 73.76 73.09 72.06 72.83 73.38 72.99 73.25
    min 72.06 us, 1126.0 ns per dependent load
  pass 2 warm (us): 46.53 46.43 46.43 46.50 46.43 46.50 46.56 46.43 46.50
    min 46.43 us, 725.5 ns per dependent load

chain     A ssbo    N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 303.58 310.75 312.42 312.32 304.80 311.62 311.74 311.97 307.74
    min 303.58 us, 296.5 ns per dependent load
  pass 2 warm (us): 141.25 141.34 141.25 141.25 141.31 141.50 141.28 141.41 141.50
    min 141.25 us, 137.9 ns per dependent load

chain     B bda     N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 460.61 454.46 452.22 459.68 453.31 452.45 459.26 456.58 454.75
    min 452.22 us, 441.6 ns per dependent load
  pass 2 warm (us): 287.65 287.94 288.03 287.68 287.84 288.35 287.97 287.97 288.35
    min 287.65 us, 280.9 ns per dependent load

chain     C bda+srt N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 835.30 840.54 833.09 839.23 1109.60 839.58 840.19 834.05 840.58
    min 833.09 us, 813.6 ns per dependent load
  pass 2 warm (us): 652.10 652.00 652.19 652.22 652.51 652.32 651.97 652.32 651.97
    min 651.97 us, 636.7 ns per dependent load

chain-parallel B bda N=1024
     1 groups: pass1 455.74 us (445.1 ns/dep load), pass2 287.68 us (280.9 ns/dep load)
    16 groups: pass1 454.62 us (444.0 ns/dep load), pass2 308.51 us (301.3 ns/dep load)
   128 groups: pass1 433.95 us (423.8 ns/dep load), pass2 308.42 us (301.2 ns/dep load)
  1024 groups: pass1 422.05 us (412.2 ns/dep load), pass2 309.28 us (302.0 ns/dep load)

dispatch split, uniform C bda+srt, iterations=667
  1 x 16384 groups            : 8.527 ms
  64 x 256 groups, no barrier: 8.582 ms
  64 x 256 groups, barriers  : 55.910 ms

=== run 3 (device-local) ===

uniform   A ssbo    iterations=13163
  pass 1 (ms): 32.843 32.732 33.293 33.123 31.689
    min 31.689 ms, 0.14 ps/load
  pass 2 (ms): 33.470 34.190 33.869 32.955 31.373
    min 31.373 ms, 0.14 ps/load

uniform   B bda     iterations=4228
  pass 1 (ms): 38.485 38.136 35.922 32.021 30.643
    min 30.643 ms, 0.43 ps/load
  pass 2 (ms): 37.919 34.426 38.291 36.103 38.454
    min 34.426 ms, 0.49 ps/load

uniform   C bda+srt iterations=2434
  pass 1 (ms): 31.916 29.563 26.873 24.145 29.484
    min 24.145 ms, 0.59 ps/load
  pass 2 (ms): 25.384 29.127 28.409 28.963 28.129
    min 25.384 ms, 0.62 ps/load

strided   A ssbo    iterations=1696
  pass 1 (ms): 30.959 30.397 30.848 30.628 31.015
    min 30.397 ms, 1.07 ps/load
  pass 2 (ms): 30.774 30.583 31.112 30.782 30.866
    min 30.583 ms, 1.07 ps/load

strided   B bda     iterations=1271
  pass 1 (ms): 27.200 27.553 27.369 27.243 26.939
    min 26.939 ms, 1.26 ps/load
  pass 2 (ms): 27.589 27.582 28.159 28.249 27.608
    min 27.582 ms, 1.29 ps/load

strided   C bda+srt iterations=1274
  pass 1 (ms): 29.674 30.034 29.041 29.024 28.333
    min 28.333 ms, 1.33 ps/load
  pass 2 (ms): 30.911 30.182 28.290 28.625 30.024
    min 28.290 ms, 1.32 ps/load

scattered A ssbo    iterations=36
  pass 1 (ms): 28.948 29.012 29.223 29.332 29.080
    min 28.948 ms, 47.93 ps/load
  pass 2 (ms): 29.246 28.930 29.221 29.710 29.218
    min 28.930 ms, 47.90 ps/load

scattered B bda     iterations=30
  pass 1 (ms): 24.225 24.686 24.940 24.573 24.315
    min 24.225 ms, 48.13 ps/load
  pass 2 (ms): 24.052 24.480 24.243 24.558 24.242
    min 24.052 ms, 47.79 ps/load

scattered C bda+srt iterations=33
  pass 1 (ms): 26.740 26.943 26.630 26.670 28.484
    min 26.630 ms, 48.10 ps/load
  pass 2 (ms): 27.361 27.038 26.761 28.428 28.146
    min 26.761 ms, 48.34 ps/load

chain     A ssbo    N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 20.74 20.45 20.67 20.93 20.70 21.12 20.70 20.67 20.86
    min 20.45 us, 319.5 ns per dependent load
  pass 2 warm (us): 9.86 9.92 9.86 9.89 9.86 9.92 9.89 9.86 9.89
    min 9.86 us, 154.0 ns per dependent load

chain     B bda     N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 29.82 29.47 30.18 30.75 30.30 30.40 30.21 30.50 30.14
    min 29.47 us, 460.5 ns per dependent load
  pass 2 warm (us): 19.62 19.52 19.62 19.87 19.55 19.58 19.52 19.58 19.52
    min 19.52 us, 305.0 ns per dependent load

chain     C bda+srt N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 70.56 73.15 73.06 72.99 72.67 73.76 71.68 72.51 73.12
    min 70.56 us, 1102.5 ns per dependent load
  pass 2 warm (us): 46.59 46.50 46.72 46.85 46.66 46.75 46.62 46.50 46.69
    min 46.50 us, 726.5 ns per dependent load

chain     A ssbo    N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 301.95 312.06 313.57 312.29 310.69 311.49 314.11 512.45 310.30
    min 301.95 us, 294.9 ns per dependent load
  pass 2 warm (us): 141.50 141.57 141.89 141.95 141.54 141.47 141.34 141.70 141.50
    min 141.34 us, 138.0 ns per dependent load

chain     B bda     N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 462.40 459.52 459.65 459.39 459.55 461.34 460.93 459.10 460.45
    min 459.10 us, 448.3 ns per dependent load
  pass 2 warm (us): 288.70 288.90 288.16 288.35 289.09 288.45 288.26 288.22 288.22
    min 288.16 us, 281.4 ns per dependent load

chain     C bda+srt N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 837.09 837.50 841.09 838.56 841.18 840.42 839.52 836.00 839.58
    min 836.00 us, 816.4 ns per dependent load
  pass 2 warm (us): 653.44 653.41 846.53 652.42 652.61 652.19 652.48 653.12 652.48
    min 652.19 us, 636.9 ns per dependent load

chain-parallel B bda N=1024
     1 groups: pass1 450.94 us (440.4 ns/dep load), pass2 287.74 us (281.0 ns/dep load)
    16 groups: pass1 452.58 us (442.0 ns/dep load), pass2 308.61 us (301.4 ns/dep load)
   128 groups: pass1 430.62 us (420.5 ns/dep load), pass2 308.48 us (301.2 ns/dep load)
  1024 groups: pass1 428.16 us (418.1 ns/dep load), pass2 309.28 us (302.0 ns/dep load)

dispatch split, uniform C bda+srt, iterations=608
  1 x 16384 groups            : 7.939 ms
  64 x 256 groups, no barrier: 8.143 ms
  64 x 256 groups, barriers  : 52.814 ms

fault words set near the mapped range: 0 (expected 0)
sink values differing from the expected 0x82928080: 0 of 256 (expected 0), sink[0] = 0x82928080

=== combined: device-local, minimum over 3 runs x 5 samples ===

| fixture | pattern | variant | ps/load pass 1 | ps/load pass 2 | ratio vs A p1 | pass2 / pass1 | 200M reads p1 (ms) |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| device-local | uniform | A ssbo | 0.13 | 0.13 | 1.00x | 1.00x | 0.03 |
| device-local | uniform | B bda | 0.41 | 0.41 | 3.13x | 1.00x | 0.08 |
| device-local | uniform | C bda+srt | 0.59 | 0.60 | 4.48x | 1.02x | 0.12 |
| device-local | strided | A ssbo | 1.04 | 1.06 | 1.00x | 1.02x | 0.21 |
| device-local | strided | B bda | 1.26 | 1.26 | 1.22x | 1.00x | 0.25 |
| device-local | strided | C bda+srt | 1.32 | 1.32 | 1.27x | 1.00x | 0.26 |
| device-local | scattered | A ssbo | 47.57 | 47.90 | 1.00x | 1.01x | 9.51 |
| device-local | scattered | B bda | 48.13 | 47.79 | 1.01x | 0.99x | 9.63 |
| device-local | scattered | C bda+srt | 47.74 | 48.07 | 1.00x | 1.01x | 9.55 |

| fixture | pattern | descriptor prologue p1 (ps per invocation) | p2 (ps per invocation) |
| --- | --- | ---: | ---: |
| device-local | uniform | 2.85 | 3.04 |
| device-local | strided | 0.89 | 1.01 |
| device-local | scattered | -6.24 | 4.45 |

| fixture | variant | N | ns per dependent load, cold | warm | slope cold (ns) | slope warm (ns) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| device-local | A ssbo | 64 | 312.5 | 153.5 | 293.7 | 136.9 |
| device-local | A ssbo | 1024 | 294.8 | 137.9 | 293.7 | 136.9 |
| device-local | B bda | 64 | 460.5 | 304.5 | 438.0 | 279.3 |
| device-local | B bda | 1024 | 439.4 | 280.9 | 438.0 | 279.3 |
| device-local | C bda+srt | 64 | 1102.5 | 725.0 | 794.3 | 630.8 |
| device-local | C bda+srt | 1024 | 813.6 | 636.7 | 794.3 | 630.8 |

| fixture | chains in flight (groups) | total cold (us) | total warm (us) | ns per dependent load, cold |
| --- | ---: | ---: | ---: | ---: |
| device-local | 1 | 450.94 | 287.68 | 440.4 |
| device-local | 16 | 452.58 | 308.48 | 442.0 |
| device-local | 128 | 430.62 | 308.35 | 420.5 |
| device-local | 1024 | 422.05 | 309.28 | 412.2 |

| fixture | dispatch shape | ms |
| --- | --- | ---: |
| device-local | 1 x 16384 groups | 7.842 |
| device-local | 64 x 256 groups, no barrier | 8.143 |
| device-local | 64 x 256 groups, barriers | 52.814 |
```

### --fixture host-import

```
device 0: NVIDIA GeForce RTX 5090
device 1: AMD Radeon(TM) Graphics

selected: NVIDIA GeForce RTX 5090
driver version: 616.64.0, api 1.4.351
timestampPeriod: 1.0000 ns
fixture: host-import
  VK_EXT_external_memory_host: present
  minImportedHostPointerAlignment: 4096 bytes
  host block: 000001CE36C00000, 269484032 bytes committed (requested 269484032)
  guest memory type 2, heap 1, flags: HOST_VISIBLE | HOST_COHERENT
  heap size: 31561 MB
  guest buffer device address: 0x7e10000
dispatch: 16384 groups x 64 threads = 1048576 invocations, 16 loads per iteration
guest memory: 257 MB mapped in 16448 pages of 16 KB; page table 512 MB

=== run 1 (host-import) ===

uniform   A ssbo    iterations=271
  pass 1 (ms): 18.230 18.410 18.382 18.627 18.610
    min 18.230 ms, 4.01 ps/load
  pass 2 (ms): 18.360 18.176 18.168 17.908 18.206
    min 17.908 ms, 3.94 ps/load

uniform   B bda     iterations=447
  pass 1 (ms): 30.209 29.701 30.319 30.696 30.212
    min 29.701 ms, 3.96 ps/load
  pass 2 (ms): 30.005 30.334 30.246 30.210 29.946
    min 29.946 ms, 3.99 ps/load

uniform   C bda+srt iterations=447
  pass 1 (ms): 31.922 30.880 30.658 31.255 31.276
    min 30.658 ms, 4.09 ps/load
  pass 2 (ms): 30.905 30.919 30.592 31.283 31.212
    min 30.592 ms, 4.08 ps/load

strided   A ssbo    iterations=3
  pass 1 (ms): 27.433 23.003 28.126 25.181 25.210
    min 23.003 ms, 457.02 ps/load
  pass 2 (ms): 26.556 25.337 26.209 25.354 26.474
    min 25.337 ms, 503.41 ps/load

strided   B bda     iterations=3
  pass 1 (ms): 24.305 24.297 24.061 25.453 23.866
    min 23.866 ms, 474.18 ps/load
  pass 2 (ms): 23.140 25.332 24.695 25.311 24.743
    min 23.140 ms, 459.74 ps/load

strided   C bda+srt iterations=6
  pass 1 (ms): 49.165 48.505 47.519 45.757 49.227
    min 45.757 ms, 454.55 ps/load
  pass 2 (ms): 49.227 49.229 48.945 45.054 49.150
    min 45.054 ms, 447.57 ps/load

scattered A ssbo    iterations=1
  pass 1 (ms): 34.217 34.208 34.698 34.414 34.423
    min 34.208 ms, 2038.96 ps/load
  pass 2 (ms): 34.423 34.460 34.526 34.422 34.422
    min 34.422 ms, 2051.71 ps/load

scattered B bda     iterations=1
  pass 1 (ms): 34.418 34.424 34.418 34.422 34.429
    min 34.418 ms, 2051.48 ps/load
  pass 2 (ms): 34.428 34.425 34.423 34.681 34.456
    min 34.423 ms, 2051.75 ps/load

scattered C bda+srt iterations=1
  pass 1 (ms): 34.241 34.458 34.445 34.443 34.455
    min 34.241 ms, 2040.91 ps/load
  pass 2 (ms): 34.471 34.457 34.469 34.579 34.752
    min 34.457 ms, 2053.80 ps/load

chain     A ssbo    N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 38.43 39.30 38.85 39.20 38.50 38.72 38.21 39.90 38.46
    min 38.21 us, 597.0 ns per dependent load
  pass 2 warm (us): 37.18 37.31 37.06 36.96 37.54 37.66 37.25 37.12 37.18
    min 36.96 us, 577.5 ns per dependent load

chain     B bda     N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 48.74 49.63 48.10 47.81 47.68 48.26 48.16 47.65 48.38
    min 47.65 us, 744.5 ns per dependent load
  pass 2 warm (us): 46.98 46.78 46.75 46.78 47.07 47.01 46.46 46.56 46.78
    min 46.46 us, 726.0 ns per dependent load

chain     C bda+srt N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 113.28 109.22 110.69 109.79 109.60 108.99 111.36 109.34 111.33
    min 108.99 us, 1703.0 ns per dependent load
  pass 2 warm (us): 107.30 107.20 107.71 107.36 107.36 107.94 107.39 108.13 108.00
    min 107.20 us, 1675.0 ns per dependent load

chain     A ssbo    N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 596.61 596.77 596.86 600.54 595.97 597.98 595.58 600.06 607.39
    min 595.58 us, 581.6 ns per dependent load
  pass 2 warm (us): 580.22 579.81 579.94 580.00 579.94 581.28 580.74 579.07 581.06
    min 579.07 us, 565.5 ns per dependent load

chain     B bda     N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 744.16 739.71 746.11 739.90 746.66 741.38 742.53 744.13 741.89
    min 739.71 us, 722.4 ns per dependent load
  pass 2 warm (us): 1013.66 723.36 725.89 725.41 724.29 723.68 727.39 723.81 725.02
    min 723.36 us, 706.4 ns per dependent load

chain     C bda+srt N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 1142.50 1150.56 1142.05 1142.30 1147.90 1145.28 1152.00 1142.50 1143.87
    min 1142.05 us, 1115.3 ns per dependent load
  pass 2 warm (us): 1122.08 1417.41 1122.91 1411.20 1316.51 1123.20 1408.06 1120.86 1121.34
    min 1120.86 us, 1094.6 ns per dependent load

chain-parallel B bda N=1024
     1 groups: pass1 740.86 us (723.5 ns/dep load), pass2 722.88 us (705.9 ns/dep load)
    16 groups: pass1 774.85 us (756.7 ns/dep load), pass2 738.66 us (721.3 ns/dep load)
   128 groups: pass1 765.09 us (747.2 ns/dep load), pass2 744.06 us (726.6 ns/dep load)
  1024 groups: pass1 775.87 us (757.7 ns/dep load), pass2 768.61 us (750.6 ns/dep load)

dispatch split, uniform C bda+srt, iterations=111
  1 x 16384 groups            : 7.426 ms
  64 x 256 groups, no barrier: 6.794 ms
  64 x 256 groups, barriers  : 20.724 ms

=== run 2 (host-import) ===

uniform   A ssbo    iterations=277
  pass 1 (ms): 18.594 18.304 18.291 18.596 18.795
    min 18.291 ms, 3.94 ps/load
  pass 2 (ms): 18.741 19.057 19.000 18.754 18.582
    min 18.582 ms, 4.00 ps/load

uniform   B bda     iterations=434
  pass 1 (ms): 29.473 29.843 29.099 29.123 29.484
    min 29.099 ms, 4.00 ps/load
  pass 2 (ms): 29.519 29.419 29.375 29.077 29.444
    min 29.077 ms, 3.99 ps/load

uniform   C bda+srt iterations=448
  pass 1 (ms): 31.042 31.633 30.710 30.638 30.466
    min 30.466 ms, 4.05 ps/load
  pass 2 (ms): 31.568 30.959 30.291 31.106 30.609
    min 30.291 ms, 4.03 ps/load

strided   A ssbo    iterations=3
  pass 1 (ms): 25.322 24.735 25.347 24.257 24.979
    min 24.257 ms, 481.95 ps/load
  pass 2 (ms): 26.315 23.581 26.671 23.251 25.945
    min 23.251 ms, 461.95 ps/load

strided   B bda     iterations=3
  pass 1 (ms): 24.207 24.399 24.464 24.467 24.015
    min 24.015 ms, 477.14 ps/load
  pass 2 (ms): 24.579 25.128 24.807 24.728 24.345
    min 24.345 ms, 483.68 ps/load

strided   C bda+srt iterations=3
  pass 1 (ms): 22.802 21.342 24.330 21.627 23.548
    min 21.342 ms, 424.02 ps/load
  pass 2 (ms): 24.253 25.057 23.046 25.447 21.882
    min 21.882 ms, 434.76 ps/load

scattered A ssbo    iterations=1
  pass 1 (ms): 34.418 34.421 34.431 34.424 34.412
    min 34.412 ms, 2051.11 ps/load
  pass 2 (ms): 34.423 34.414 34.414 34.460 34.423
    min 34.414 ms, 2051.20 ps/load

scattered B bda     iterations=1
  pass 1 (ms): 34.445 34.427 34.425 34.222 34.449
    min 34.222 ms, 2039.82 ps/load
  pass 2 (ms): 34.426 34.433 34.730 34.417 34.408
    min 34.408 ms, 2050.90 ps/load

scattered C bda+srt iterations=1
  pass 1 (ms): 34.444 34.445 34.748 34.438 34.445
    min 34.438 ms, 2052.69 ps/load
  pass 2 (ms): 34.446 34.447 34.457 34.451 34.444
    min 34.444 ms, 2053.04 ps/load

chain     A ssbo    N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 38.78 38.88 38.24 39.01 38.69 39.26 38.53 38.75 38.43
    min 38.24 us, 597.5 ns per dependent load
  pass 2 warm (us): 36.86 37.09 36.83 37.76 37.15 37.12 37.12 36.86 37.12
    min 36.83 us, 575.5 ns per dependent load

chain     B bda     N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 48.22 48.00 48.77 48.00 49.02 47.94 48.67 47.81 48.10
    min 47.81 us, 747.0 ns per dependent load
  pass 2 warm (us): 47.07 46.53 46.75 46.91 46.43 46.66 46.50 46.59 46.69
    min 46.43 us, 725.5 ns per dependent load

chain     C bda+srt N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 113.76 109.09 111.62 109.31 113.76 109.76 109.41 109.12 108.99
    min 108.99 us, 1703.0 ns per dependent load
  pass 2 warm (us): 107.94 107.78 107.36 107.01 107.58 107.52 107.39 107.14 107.74
    min 107.01 us, 1672.0 ns per dependent load

chain     A ssbo    N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 598.56 600.35 597.09 800.86 599.23 611.42 597.82 598.30 600.26
    min 597.09 us, 583.1 ns per dependent load
  pass 2 warm (us): 579.97 581.41 577.82 580.42 581.41 581.06 580.13 583.07 580.86
    min 577.82 us, 564.3 ns per dependent load

chain     B bda     N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 741.54 741.89 743.04 742.37 741.60 743.65 740.83 746.59 752.45
    min 740.83 us, 723.5 ns per dependent load
  pass 2 warm (us): 724.38 723.17 725.02 728.64 724.35 723.62 727.04 727.58 724.99
    min 723.17 us, 706.2 ns per dependent load

chain     C bda+srt N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 1144.13 1140.77 1141.25 1155.10 1141.12 1147.97 1141.44 1150.78 1142.98
    min 1140.77 us, 1114.0 ns per dependent load
  pass 2 warm (us): 1337.70 1122.72 1422.02 1317.44 1121.79 1316.00 1120.29 1416.22 1412.38
    min 1120.29 us, 1094.0 ns per dependent load

chain-parallel B bda N=1024
     1 groups: pass1 741.76 us (724.4 ns/dep load), pass2 723.49 us (706.5 ns/dep load)
    16 groups: pass1 775.87 us (757.7 ns/dep load), pass2 740.13 us (722.8 ns/dep load)
   128 groups: pass1 771.42 us (753.3 ns/dep load), pass2 758.02 us (740.2 ns/dep load)
  1024 groups: pass1 780.51 us (762.2 ns/dep load), pass2 770.27 us (752.2 ns/dep load)

dispatch split, uniform C bda+srt, iterations=112
  1 x 16384 groups            : 7.515 ms
  64 x 256 groups, no barrier: 6.675 ms
  64 x 256 groups, barriers  : 20.476 ms

=== run 3 (host-import) ===

uniform   A ssbo    iterations=447
  pass 1 (ms): 29.966 30.174 29.932 30.250 30.256
    min 29.932 ms, 3.99 ps/load
  pass 2 (ms): 30.234 30.137 30.567 30.261 30.248
    min 30.137 ms, 4.02 ps/load

uniform   B bda     iterations=447
  pass 1 (ms): 30.848 30.229 29.913 29.985 29.876
    min 29.876 ms, 3.98 ps/load
  pass 2 (ms): 31.011 30.226 30.232 30.189 31.125
    min 30.189 ms, 4.03 ps/load

uniform   C bda+srt iterations=413
  pass 1 (ms): 28.586 28.509 28.112 28.978 28.958
    min 28.112 ms, 4.06 ps/load
  pass 2 (ms): 28.647 28.381 29.737 29.263 28.209
    min 28.209 ms, 4.07 ps/load

strided   A ssbo    iterations=3
  pass 1 (ms): 28.380 25.251 24.818 24.629 24.276
    min 24.276 ms, 482.33 ps/load
  pass 2 (ms): 25.406 25.725 25.349 25.842 25.301
    min 25.301 ms, 502.69 ps/load

strided   B bda     iterations=3
  pass 1 (ms): 26.775 24.283 24.319 24.041 24.386
    min 24.041 ms, 477.66 ps/load
  pass 2 (ms): 25.203 24.683 24.585 24.572 25.929
    min 24.572 ms, 488.20 ps/load

strided   C bda+srt iterations=6
  pass 1 (ms): 49.579 49.176 49.044 49.308 49.587
    min 49.044 ms, 487.20 ps/load
  pass 2 (ms): 48.750 49.072 49.206 49.034 49.091
    min 48.750 ms, 484.29 ps/load

scattered A ssbo    iterations=1
  pass 1 (ms): 34.413 34.703 34.224 34.404 34.450
    min 34.224 ms, 2039.93 ps/load
  pass 2 (ms): 34.429 34.414 34.556 34.717 34.436
    min 34.414 ms, 2051.25 ps/load

scattered B bda     iterations=1
  pass 1 (ms): 34.452 34.427 34.429 34.444 34.807
    min 34.427 ms, 2051.99 ps/load
  pass 2 (ms): 34.843 34.418 34.422 34.421 34.742
    min 34.418 ms, 2051.46 ps/load

scattered C bda+srt iterations=1
  pass 1 (ms): 34.233 34.663 34.248 34.453 34.460
    min 34.233 ms, 2040.46 ps/load
  pass 2 (ms): 34.458 34.462 34.640 34.443 34.454
    min 34.443 ms, 2052.96 ps/load

chain     A ssbo    N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 38.30 38.21 39.23 39.01 38.18 39.04 38.08 39.52 38.53
    min 38.08 us, 595.0 ns per dependent load
  pass 2 warm (us): 36.96 37.41 38.40 37.06 37.18 37.60 37.12 37.66 37.38
    min 36.96 us, 577.5 ns per dependent load

chain     B bda     N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 47.87 47.87 48.10 48.80 48.13 47.81 48.06 52.45 48.26
    min 47.81 us, 747.0 ns per dependent load
  pass 2 warm (us): 46.62 46.62 46.82 46.78 47.01 46.66 46.69 46.75 46.37
    min 46.37 us, 724.5 ns per dependent load

chain     C bda+srt N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 109.06 109.09 109.89 110.53 112.54 109.22 110.34 109.70 111.14
    min 109.06 us, 1704.0 ns per dependent load
  pass 2 warm (us): 107.65 107.36 408.58 107.65 108.13 107.68 107.26 108.42 107.42
    min 107.26 us, 1676.0 ns per dependent load

chain     A ssbo    N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 597.02 596.45 595.17 597.06 597.41 602.40 596.32 601.02 596.03
    min 595.17 us, 581.2 ns per dependent load
  pass 2 warm (us): 579.52 580.38 578.50 579.01 580.16 579.65 580.35 579.97 579.68
    min 578.50 us, 564.9 ns per dependent load

chain     B bda     N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 742.82 741.70 740.45 741.92 750.53 741.22 748.19 741.22 746.43
    min 740.45 us, 723.1 ns per dependent load
  pass 2 warm (us): 723.26 721.66 1024.26 725.54 724.93 724.13 727.55 724.67 723.58
    min 721.66 us, 704.8 ns per dependent load

chain     C bda+srt N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 1141.28 1148.19 1141.28 1150.08 1141.41 1141.60 1146.98 1144.99 1172.70
    min 1141.28 us, 1114.5 ns per dependent load
  pass 2 warm (us): 1419.26 1316.70 1121.09 1410.85 1121.66 1418.75 1316.51 1124.58 1318.18
    min 1121.09 us, 1094.8 ns per dependent load

chain-parallel B bda N=1024
     1 groups: pass1 740.16 us (722.8 ns/dep load), pass2 723.42 us (706.5 ns/dep load)
    16 groups: pass1 776.86 us (758.7 ns/dep load), pass2 741.70 us (724.3 ns/dep load)
   128 groups: pass1 763.23 us (745.3 ns/dep load), pass2 743.30 us (725.9 ns/dep load)
  1024 groups: pass1 771.94 us (753.8 ns/dep load), pass2 764.48 us (746.6 ns/dep load)

dispatch split, uniform C bda+srt, iterations=103
  1 x 16384 groups            : 6.908 ms
  64 x 256 groups, no barrier: 6.373 ms
  64 x 256 groups, barriers  : 19.242 ms

fault words set near the mapped range: 0 (expected 0)
sink values differing from the expected 0x82928080: 0 of 256 (expected 0), sink[0] = 0x82928080

=== combined: host-import, minimum over 3 runs x 5 samples ===

| fixture | pattern | variant | ps/load pass 1 | ps/load pass 2 | ratio vs A p1 | pass2 / pass1 | 200M reads p1 (ms) |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| host-import | uniform | A ssbo | 3.94 | 3.94 | 1.00x | 1.00x | 0.79 |
| host-import | uniform | B bda | 3.96 | 3.99 | 1.01x | 1.01x | 0.79 |
| host-import | uniform | C bda+srt | 4.05 | 4.03 | 1.03x | 0.99x | 0.81 |
| host-import | strided | A ssbo | 457.02 | 461.95 | 1.00x | 1.01x | 91.40 |
| host-import | strided | B bda | 474.18 | 459.74 | 1.04x | 0.97x | 94.84 |
| host-import | strided | C bda+srt | 424.02 | 434.76 | 0.93x | 1.03x | 84.80 |
| host-import | scattered | A ssbo | 2038.96 | 2051.20 | 1.00x | 1.01x | 407.79 |
| host-import | scattered | B bda | 2039.82 | 2050.90 | 1.00x | 1.01x | 407.96 |
| host-import | scattered | C bda+srt | 2040.46 | 2052.96 | 1.00x | 1.01x | 408.09 |

| fixture | pattern | descriptor prologue p1 (ps per invocation) | p2 (ps per invocation) |
| --- | --- | ---: | ---: |
| host-import | uniform | 1.49 | 0.59 |
| host-import | strided | -802.46 | -399.69 |
| host-import | scattered | 10.31 | 32.96 |

| fixture | variant | N | ns per dependent load, cold | warm | slope cold (ns) | slope warm (ns) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| host-import | A ssbo | 64 | 595.0 | 575.5 | 580.3 | 563.5 |
| host-import | A ssbo | 1024 | 581.2 | 564.3 | 580.3 | 563.5 |
| host-import | B bda | 64 | 744.5 | 724.5 | 720.9 | 703.4 |
| host-import | B bda | 1024 | 722.4 | 704.8 | 720.9 | 703.4 |
| host-import | C bda+srt | 64 | 1703.0 | 1672.0 | 1074.8 | 1055.5 |
| host-import | C bda+srt | 1024 | 1114.0 | 1094.0 | 1074.8 | 1055.5 |

| fixture | chains in flight (groups) | total cold (us) | total warm (us) | ns per dependent load, cold |
| --- | ---: | ---: | ---: | ---: |
| host-import | 1 | 740.16 | 722.88 | 722.8 |
| host-import | 16 | 774.85 | 738.66 | 756.7 |
| host-import | 128 | 763.23 | 743.30 | 745.3 |
| host-import | 1024 | 771.94 | 764.48 | 753.8 |

| fixture | dispatch shape | ms |
| --- | --- | ---: |
| host-import | 1 x 16384 groups | 6.908 |
| host-import | 64 x 256 groups, no barrier | 6.373 |
| host-import | 64 x 256 groups, barriers | 19.242 |
```

### --fixture host-visible

```
device 0: NVIDIA GeForce RTX 5090
device 1: AMD Radeon(TM) Graphics

selected: NVIDIA GeForce RTX 5090
driver version: 616.64.0, api 1.4.351
timestampPeriod: 1.0000 ns
fixture: host-visible
  VK_EXT_external_memory_host: present
  minImportedHostPointerAlignment: 4096 bytes
  guest memory type 2, heap 1, flags: HOST_VISIBLE | HOST_COHERENT
  heap size: 31561 MB
  guest buffer device address: 0x7e10000
dispatch: 16384 groups x 64 threads = 1048576 invocations, 16 loads per iteration
guest memory: 257 MB mapped in 16448 pages of 16 KB; page table 512 MB

=== run 1 (host-visible) ===

uniform   A ssbo    iterations=280
  pass 1 (ms): 19.273 19.291 18.716 19.166 18.408
    min 18.408 ms, 3.92 ps/load
  pass 2 (ms): 18.405 18.711 18.918 19.269 19.290
    min 18.405 ms, 3.92 ps/load

uniform   B bda     iterations=448
  pass 1 (ms): 30.656 30.321 30.026 30.104 30.003
    min 30.003 ms, 3.99 ps/load
  pass 2 (ms): 30.313 29.943 30.298 30.277 30.457
    min 29.943 ms, 3.98 ps/load

uniform   C bda+srt iterations=413
  pass 1 (ms): 28.250 27.889 27.942 28.462 28.169
    min 27.889 ms, 4.02 ps/load
  pass 2 (ms): 28.170 28.268 27.904 28.821 27.858
    min 27.858 ms, 4.02 ps/load

strided   A ssbo    iterations=3
  pass 1 (ms): 24.765 25.446 24.813 25.400 25.017
    min 24.765 ms, 492.03 ps/load
  pass 2 (ms): 25.355 26.337 23.408 26.733 23.194
    min 23.194 ms, 460.82 ps/load

strided   B bda     iterations=3
  pass 1 (ms): 24.332 23.970 26.897 23.969 26.785
    min 23.969 ms, 476.22 ps/load
  pass 2 (ms): 25.037 24.338 24.705 24.357 24.870
    min 24.338 ms, 483.54 ps/load

strided   C bda+srt iterations=6
  pass 1 (ms): 48.671 48.769 49.284 49.003 48.922
    min 48.671 ms, 483.50 ps/load
  pass 2 (ms): 48.748 48.702 48.550 48.517 48.439
    min 48.439 ms, 481.20 ps/load

scattered A ssbo    iterations=1
  pass 1 (ms): 34.427 34.566 34.451 34.443 34.727
    min 34.427 ms, 2052.03 ps/load
  pass 2 (ms): 34.422 34.518 34.427 34.427 34.429
    min 34.422 ms, 2051.69 ps/load

scattered B bda     iterations=1
  pass 1 (ms): 34.445 34.436 34.445 34.618 34.443
    min 34.436 ms, 2052.55 ps/load
  pass 2 (ms): 34.424 34.428 34.571 34.737 34.433
    min 34.424 ms, 2051.83 ps/load

scattered C bda+srt iterations=1
  pass 1 (ms): 34.445 34.454 34.526 34.275 34.230
    min 34.230 ms, 2040.25 ps/load
  pass 2 (ms): 34.440 34.453 34.824 34.440 34.619
    min 34.440 ms, 2052.76 ps/load

chain     A ssbo    N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 38.66 38.62 39.62 38.91 39.71 39.10 40.13 38.82 38.78
    min 38.62 us, 603.5 ns per dependent load
  pass 2 warm (us): 10.78 10.75 10.78 10.78 10.78 10.75 10.78 10.78 10.75
    min 10.75 us, 168.0 ns per dependent load

chain     B bda     N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 48.51 48.13 48.48 49.28 48.48 48.96 48.51 49.60 48.19
    min 48.13 us, 752.0 ns per dependent load
  pass 2 warm (us): 20.42 20.42 20.45 20.42 20.42 20.42 20.42 20.45 20.38
    min 20.38 us, 318.5 ns per dependent load

chain     C bda+srt N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 110.59 110.24 110.21 111.55 110.02 111.04 110.82 110.78 110.43
    min 110.02 us, 1719.0 ns per dependent load
  pass 2 warm (us): 48.48 48.42 48.42 48.51 48.42 48.38 48.48 48.42 48.38
    min 48.38 us, 756.0 ns per dependent load

chain     A ssbo    N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 600.38 601.34 609.38 602.40 610.02 601.31 814.62 604.26 611.65
    min 600.38 us, 586.3 ns per dependent load
  pass 2 warm (us): 157.09 157.12 156.86 157.06 157.15 157.12 588.06 157.06 157.12
    min 156.86 us, 153.2 ns per dependent load

chain     B bda     N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 752.00 750.69 748.51 755.55 747.23 748.99 747.07 754.62 747.07
    min 747.07 us, 729.6 ns per dependent load
  pass 2 warm (us): 691.14 602.69 303.10 303.62 303.10 303.14 303.49 303.39 303.62
    min 303.10 us, 296.0 ns per dependent load

chain     C bda+srt N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 1458.43 1149.25 1154.08 1148.64 1153.89 1147.23 1151.52 1154.14 1149.50
    min 1147.23 us, 1120.3 ns per dependent load
  pass 2 warm (us): 669.09 669.15 669.50 669.82 1194.05 668.99 669.38 669.47 669.34
    min 668.99 us, 653.3 ns per dependent load

chain-parallel B bda N=1024
     1 groups: pass1 747.49 us (730.0 ns/dep load), pass2 303.07 us (296.0 ns/dep load)
    16 groups: pass1 737.09 us (719.8 ns/dep load), pass2 331.55 us (323.8 ns/dep load)
   128 groups: pass1 717.82 us (701.0 ns/dep load), pass2 335.30 us (327.4 ns/dep load)
  1024 groups: pass1 712.26 us (695.6 ns/dep load), pass2 487.46 us (476.0 ns/dep load)

dispatch split, uniform C bda+srt, iterations=103
  1 x 16384 groups            : 6.823 ms
  64 x 256 groups, no barrier: 1.678 ms
  64 x 256 groups, barriers  : 16.166 ms

=== run 2 (host-visible) ===

uniform   A ssbo    iterations=285
  pass 1 (ms): 19.632 19.040 19.056 18.735 19.051
    min 18.735 ms, 3.92 ps/load
  pass 2 (ms): 19.035 19.261 19.542 19.856 19.256
    min 19.035 ms, 3.98 ps/load

uniform   B bda     iterations=441
  pass 1 (ms): 29.972 30.410 30.092 29.882 29.604
    min 29.604 ms, 4.00 ps/load
  pass 2 (ms): 29.922 29.851 29.567 29.634 29.538
    min 29.538 ms, 3.99 ps/load

uniform   C bda+srt iterations=424
  pass 1 (ms): 29.163 29.636 28.560 28.609 29.191
    min 28.560 ms, 4.01 ps/load
  pass 2 (ms): 28.988 29.098 29.022 28.576 28.950
    min 28.576 ms, 4.02 ps/load

strided   A ssbo    iterations=3
  pass 1 (ms): 26.004 23.796 27.807 24.893 25.075
    min 23.796 ms, 472.78 ps/load
  pass 2 (ms): 24.429 27.891 26.469 25.282 25.691
    min 24.429 ms, 485.36 ps/load

strided   B bda     iterations=6
  pass 1 (ms): 49.508 49.288 48.117 47.099 48.917
    min 47.099 ms, 467.89 ps/load
  pass 2 (ms): 51.189 51.272 51.459 48.279 48.262
    min 48.262 ms, 479.44 ps/load

strided   C bda+srt iterations=6
  pass 1 (ms): 46.347 46.669 48.646 48.687 48.862
    min 46.347 ms, 460.42 ps/load
  pass 2 (ms): 47.310 48.538 49.015 47.932 48.609
    min 47.310 ms, 469.98 ps/load

scattered A ssbo    iterations=1
  pass 1 (ms): 34.437 34.316 34.219 34.224 34.422
    min 34.219 ms, 2039.62 ps/load
  pass 2 (ms): 34.822 34.449 34.435 34.429 34.440
    min 34.429 ms, 2052.13 ps/load

scattered B bda     iterations=1
  pass 1 (ms): 34.438 34.435 34.432 34.653 34.426
    min 34.426 ms, 2051.97 ps/load
  pass 2 (ms): 34.427 34.464 34.442 34.454 34.482
    min 34.427 ms, 2052.00 ps/load

scattered C bda+srt iterations=1
  pass 1 (ms): 34.245 34.232 34.229 34.446 37.530
    min 34.229 ms, 2040.20 ps/load
  pass 2 (ms): 34.433 34.452 34.658 35.608 37.031
    min 34.433 ms, 2052.35 ps/load

chain     A ssbo    N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 39.49 39.65 39.81 39.74 39.52 39.71 39.23 39.17 39.26
    min 39.17 us, 612.0 ns per dependent load
  pass 2 warm (us): 11.17 11.17 11.20 11.17 11.23 11.14 11.23 11.14 11.23
    min 11.14 us, 174.0 ns per dependent load

chain     B bda     N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 49.15 49.06 49.09 49.41 49.47 49.12 49.22 49.25 48.86
    min 48.86 us, 763.5 ns per dependent load
  pass 2 warm (us): 20.80 20.83 20.83 20.86 20.83 20.80 20.93 20.83 20.77
    min 20.77 us, 324.5 ns per dependent load

chain     C bda+srt N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 112.19 110.14 110.59 110.24 112.26 109.98 112.93 111.33 111.58
    min 109.98 us, 1718.5 ns per dependent load
  pass 2 warm (us): 48.80 48.38 48.48 48.48 48.83 48.38 48.80 48.80 48.48
    min 48.38 us, 756.0 ns per dependent load

chain     A ssbo    N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 616.93 628.32 611.23 603.97 601.92 616.96 603.71 621.79 599.90
    min 599.90 us, 585.8 ns per dependent load
  pass 2 warm (us): 156.96 157.44 156.93 156.99 157.12 159.17 157.12 156.93 156.93
    min 156.93 us, 153.2 ns per dependent load

chain     B bda     N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 748.26 755.74 747.26 752.45 763.04 750.78 753.66 752.16 749.98
    min 747.26 us, 729.8 ns per dependent load
  pass 2 warm (us): 303.07 303.39 303.01 303.52 303.49 302.91 303.42 303.71 303.68
    min 302.91 us, 295.8 ns per dependent load

chain     C bda+srt N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 1151.74 1153.54 1151.71 1157.34 1153.54 1150.30 1154.27 1150.69 1151.68
    min 1150.30 us, 1123.3 ns per dependent load
  pass 2 warm (us): 669.31 669.12 1388.58 670.85 670.24 1319.71 669.57 1326.91 671.62
    min 669.12 us, 653.4 ns per dependent load

chain-parallel B bda N=1024
     1 groups: pass1 745.34 us (727.9 ns/dep load), pass2 303.07 us (296.0 ns/dep load)
    16 groups: pass1 736.74 us (719.5 ns/dep load), pass2 331.52 us (323.8 ns/dep load)
   128 groups: pass1 718.53 us (701.7 ns/dep load), pass2 335.01 us (327.2 ns/dep load)
  1024 groups: pass1 711.68 us (695.0 ns/dep load), pass2 487.01 us (475.6 ns/dep load)

dispatch split, uniform C bda+srt, iterations=106
  1 x 16384 groups            : 7.309 ms
  64 x 256 groups, no barrier: 1.729 ms
  64 x 256 groups, barriers  : 17.628 ms

=== run 3 (host-visible) ===

uniform   A ssbo    iterations=265
  pass 1 (ms): 18.589 18.570 17.418 17.637 18.576
    min 17.418 ms, 3.92 ps/load
  pass 2 (ms): 18.592 18.570 17.672 18.248 18.682
    min 17.672 ms, 3.97 ps/load

uniform   B bda     iterations=430
  pass 1 (ms): 29.708 29.736 29.912 29.914 31.810
    min 29.708 ms, 4.12 ps/load
  pass 2 (ms): 29.896 29.888 29.919 30.040 31.156
    min 29.888 ms, 4.14 ps/load

uniform   C bda+srt iterations=401
  pass 1 (ms): 27.497 27.670 27.535 28.051 27.068
    min 27.068 ms, 4.02 ps/load
  pass 2 (ms): 27.439 28.076 27.524 27.399 27.105
    min 27.105 ms, 4.03 ps/load

strided   A ssbo    iterations=3
  pass 1 (ms): 25.259 24.379 25.376 24.530 25.361
    min 24.379 ms, 484.36 ps/load
  pass 2 (ms): 25.947 24.170 26.464 23.754 26.768
    min 23.754 ms, 471.95 ps/load

strided   B bda     iterations=3
  pass 1 (ms): 24.229 25.475 23.689 24.374 24.479
    min 23.689 ms, 470.65 ps/load
  pass 2 (ms): 23.294 25.474 24.660 25.030 27.019
    min 23.294 ms, 462.81 ps/load

strided   C bda+srt iterations=6
  pass 1 (ms): 48.943 48.104 46.280 46.511 48.914
    min 46.280 ms, 459.75 ps/load
  pass 2 (ms): 48.976 48.747 47.044 48.693 48.524
    min 47.044 ms, 467.34 ps/load

scattered A ssbo    iterations=1
  pass 1 (ms): 34.198 34.214 34.371 34.389 34.479
    min 34.198 ms, 2038.39 ps/load
  pass 2 (ms): 34.399 34.719 34.598 34.395 34.598
    min 34.395 ms, 2050.09 ps/load

scattered B bda     iterations=1
  pass 1 (ms): 34.399 34.422 34.405 34.513 34.583
    min 34.399 ms, 2050.34 ps/load
  pass 2 (ms): 34.602 34.417 34.406 34.407 34.692
    min 34.406 ms, 2050.78 ps/load

scattered C bda+srt iterations=1
  pass 1 (ms): 34.556 34.419 34.422 34.414 36.855
    min 34.414 ms, 2051.22 ps/load
  pass 2 (ms): 34.415 34.398 34.427 34.723 37.095
    min 34.398 ms, 2050.30 ps/load

chain     A ssbo    N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 39.74 40.96 39.68 39.68 42.11 39.71 41.86 38.62 39.71
    min 38.62 us, 603.5 ns per dependent load
  pass 2 warm (us): 11.33 11.17 11.10 11.17 11.17 11.17 11.17 10.78 11.20
    min 10.78 us, 168.5 ns per dependent load

chain     B bda     N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 49.50 48.35 48.48 48.93 49.18 49.66 48.67 48.70 48.29
    min 48.29 us, 754.5 ns per dependent load
  pass 2 warm (us): 20.38 20.45 20.38 20.45 20.80 20.38 20.77 20.42 20.77
    min 20.38 us, 318.5 ns per dependent load

chain     C bda+srt N=64 (1 workgroup, caches evicted first)
  pass 1 cold (us): 109.98 111.10 110.88 110.18 109.57 113.25 110.27 110.37 110.37
    min 109.57 us, 1712.0 ns per dependent load
  pass 2 warm (us): 48.51 48.42 48.42 48.42 48.48 48.51 48.48 48.48 48.42
    min 48.42 us, 756.5 ns per dependent load

chain     A ssbo    N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 602.21 602.08 610.24 600.22 805.34 602.82 600.35 603.71 604.03
    min 600.22 us, 586.2 ns per dependent load
  pass 2 warm (us): 157.12 156.93 156.90 157.09 589.15 157.09 156.99 156.93 157.15
    min 156.90 us, 153.2 ns per dependent load

chain     B bda     N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 747.39 1023.23 748.16 748.26 747.42 755.23 746.78 753.50 750.62
    min 746.78 us, 729.3 ns per dependent load
  pass 2 warm (us): 303.36 303.30 303.23 513.66 303.04 303.46 302.94 303.10 303.10
    min 302.94 us, 295.8 ns per dependent load

chain     C bda+srt N=1024 (1 workgroup, caches evicted first)
  pass 1 cold (us): 1153.50 1149.12 1157.18 1149.76 1157.12 1148.16 1148.35 1156.61 1146.08
    min 1146.08 us, 1119.2 ns per dependent load
  pass 2 warm (us): 669.02 669.92 669.41 669.25 669.50 668.99 669.63 669.47 669.22
    min 668.99 us, 653.3 ns per dependent load

chain-parallel B bda N=1024
     1 groups: pass1 747.78 us (730.2 ns/dep load), pass2 302.94 us (295.8 ns/dep load)
    16 groups: pass1 743.81 us (726.4 ns/dep load), pass2 332.00 us (324.2 ns/dep load)
   128 groups: pass1 733.15 us (716.0 ns/dep load), pass2 336.64 us (328.8 ns/dep load)
  1024 groups: pass1 719.01 us (702.2 ns/dep load), pass2 487.04 us (475.6 ns/dep load)

dispatch split, uniform C bda+srt, iterations=100
  1 x 16384 groups            : 6.611 ms
  64 x 256 groups, no barrier: 1.509 ms
  64 x 256 groups, barriers  : 15.984 ms

fault words set near the mapped range: 0 (expected 0)
sink values differing from the expected 0x82928080: 0 of 256 (expected 0), sink[0] = 0x82928080

=== combined: host-visible, minimum over 3 runs x 5 samples ===

| fixture | pattern | variant | ps/load pass 1 | ps/load pass 2 | ratio vs A p1 | pass2 / pass1 | 200M reads p1 (ms) |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| host-visible | uniform | A ssbo | 3.92 | 3.92 | 1.00x | 1.00x | 0.78 |
| host-visible | uniform | B bda | 3.99 | 3.98 | 1.02x | 1.00x | 0.80 |
| host-visible | uniform | C bda+srt | 4.01 | 4.02 | 1.02x | 1.00x | 0.80 |
| host-visible | strided | A ssbo | 472.78 | 460.82 | 1.00x | 0.97x | 94.56 |
| host-visible | strided | B bda | 467.89 | 462.81 | 0.99x | 0.99x | 93.58 |
| host-visible | strided | C bda+srt | 459.75 | 467.34 | 0.97x | 1.02x | 91.95 |
| host-visible | scattered | A ssbo | 2038.39 | 2050.09 | 1.00x | 1.01x | 407.68 |
| host-visible | scattered | B bda | 2050.34 | 2050.78 | 1.01x | 1.00x | 410.07 |
| host-visible | scattered | C bda+srt | 2040.20 | 2050.30 | 1.00x | 1.00x | 408.04 |

| fixture | pattern | descriptor prologue p1 (ps per invocation) | p2 (ps per invocation) |
| --- | --- | ---: | ---: |
| host-visible | uniform | 0.37 | 0.53 |
| host-visible | strided | -130.25 | 72.47 |
| host-visible | scattered | -162.35 | -7.63 |

| fixture | variant | N | ns per dependent load, cold | warm | slope cold (ns) | slope warm (ns) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| host-visible | A ssbo | 64 | 603.5 | 168.0 | 584.7 | 152.2 |
| host-visible | A ssbo | 1024 | 585.8 | 153.2 | 584.7 | 152.2 |
| host-visible | B bda | 64 | 752.0 | 318.5 | 727.8 | 294.3 |
| host-visible | B bda | 1024 | 729.3 | 295.8 | 727.8 | 294.3 |
| host-visible | C bda+srt | 64 | 1712.0 | 756.0 | 1079.7 | 646.5 |
| host-visible | C bda+srt | 1024 | 1119.2 | 653.3 | 1079.7 | 646.5 |

| fixture | chains in flight (groups) | total cold (us) | total warm (us) | ns per dependent load, cold |
| --- | ---: | ---: | ---: | ---: |
| host-visible | 1 | 745.34 | 302.94 | 727.9 |
| host-visible | 16 | 736.74 | 331.52 | 719.5 |
| host-visible | 128 | 717.82 | 335.01 | 701.0 |
| host-visible | 1024 | 711.68 | 487.01 | 695.0 |

| fixture | dispatch shape | ms |
| --- | --- | ---: |
| host-visible | 1 x 16384 groups | 6.611 |
| host-visible | 64 x 256 groups, no barrier | 1.509 |
| host-visible | 64 x 256 groups, barriers | 15.984 |
```
