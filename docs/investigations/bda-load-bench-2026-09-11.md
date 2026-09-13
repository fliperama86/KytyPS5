# BDA load cost micro-bench (2026-09-11)

What a shader memory read costs when it is translated through the emulator's guest-address page
table (buffer device address) instead of read from a normally bound storage buffer. Measured to
decide whether per-draw descriptor fetch can move from the CPU into shaders.

## Method

`tests/BdaLoadBench.cpp` plus `tests/bda_load_bench.comp`, built as the `bda_load_bench` target
(`_Build/build-targets.ps1 -Targets bda_load_bench`). The program is headless and standalone: it
loads `vulkan-1.dll` by hand, creates a compute-only device, and shares nothing with the emulator
runtime.

The shader reproduces the shape that `DefineGetBdaPointer` / `LoadBdaDword` emit in
`src/graphics/shader/recompiler/backend/spirv/spirvEmitterMemory.cpp`: shift the guest address down
by `BufferCache::CACHING_PAGEBITS` (14), index a storage buffer of 64-bit device addresses, compare
the entry to zero, branch, set a fault bit on a miss, otherwise add the in-page offset and load
through a `PhysicalStorageBuffer` pointer with an alignment of four.

Fixture: a 257 MB device-local buffer as guest memory (256 MB walked by the access patterns plus a
1 MB descriptor tail), every 16 KB page mapped in a full-size 512 MB page table
(`BufferCache::BDA_PAGETABLE_SIZE`), and the 8 MB fault-bit buffer.

Variants, 64-thread workgroups, 16 u32 loads per invocation per iteration:

- **A** loads from a bound storage buffer.
- **B** loads through the page table.
- **C** is B plus a descriptor prologue per iteration: a three-level chain through the page table
  (user-data root to table pointer to V#, 8 dwords) whose V# supplies the load base
  (`dword0 | (dword1 & 0xffff) << 32`). This models an in-shader SRT fetch.

Patterns: **uniform** (every lane reads the same address, constant-buffer style), **strided** (lane
`i` reads its own 64-byte record, vertex-fetch style), **scattered** (pseudo-random dword anywhere
in 256 MB).

Dispatch: 16384 groups x 64 threads = 1048576 invocations; the iteration count is auto-calibrated
so a dispatch lands near 30 ms of GPU time; timed with a `vkCmdWriteTimestamp` pair; 5 samples per
cell, minimum reported; the whole run repeated 3 times.

Correctness gates that passed on every run: zero fault bits set anywhere near the mapped range, and
every sink value bit-exact against the accumulator computed on the host.

Hardware: NVIDIA GeForce RTX 5090, driver 616.64.0, Vulkan 1.4.351, `timestampPeriod` 1.0 ns.

## Results

`ps / load` is GPU time divided by the total number of data loads issued across the whole device,
so it is a throughput figure, not a latency. Values are the minimum over 15 samples (3 runs x 5);
the ratio column gives the spread of the per-run ratios.

| pattern | variant | ps / load | ratio vs A | 200M reads (ms) |
| --- | --- | ---: | ---: | ---: |
| uniform | A ssbo | 0.13 | 1.00x | 0.03 |
| uniform | B bda | 0.41 | 3.12-3.14x | 0.08 |
| uniform | C bda+srt | 0.60 | 4.59-4.62x | 0.12 |
| strided | A ssbo | 1.04 | 1.00x | 0.21 |
| strided | B bda | 1.23 | 1.18-1.19x | 0.25 |
| strided | C bda+srt | 1.29 | 1.23-1.24x | 0.26 |
| scattered | A ssbo | 46.80 | 1.00x | 9.36 |
| scattered | B bda | 46.88 | 1.00x | 9.38 |
| scattered | C bda+srt | 46.89 | 1.00x | 9.38 |

Cost of the descriptor prologue alone, as C minus B per invocation:

| pattern | prologue (ps per invocation) | 1M invocations (ms) |
| --- | ---: | ---: |
| uniform | 3.07-3.10 | 0.0031 |
| strided | 0.70-1.08 | 0.0010 |
| scattered | 0.01-0.39 | 0.0004 |

## Interpretation

1. A page-table read never costs 5x: the worst ratio is 3.1x, and even with the SRT prologue folded
   in it is 4.6x. Both occur only against the cheapest possible baseline - a lane-uniform L1 hit at
   0.13 ps/load - where the whole difference is 0.05 ms per 200 million reads.
2. On the pattern that actually moves data, scattered reads over 256 MB, BDA is free (1.00x): the
   page-table entry is an L2 hit hidden behind the DRAM latency of the real load, and 200M reads
   cost 9.4 ms either way.
3. Strided vertex-fetch-shaped reads pay 19%, which is 0.04 ms per 200 million reads. At the 22 ms
   of GPU time in a 90 ms frame this is far below measurement noise.
4. The in-shader SRT walk costs about 3 ps per invocation, so 0.003 ms per million invocations.
   Even 100 million shader invocations per frame would add 0.3 ms.
5. The GPU does not become the wall. Moving per-draw descriptor fetch into shaders is not blocked
   by BDA read cost on this machine; if it regresses, the cause will be somewhere else.

## Caveats

In the uniform and strided patterns the 16 loads of one iteration fall inside a single 16 KB page,
so the driver is free to fold their page-table lookups into one; the scattered pattern cannot be
folded and is the one that bounds the per-load cost. The ratios also sit on an optimised baseline -
16 consecutive dwords let the compiler widen the bound-SSBO loads - which flatters variant A. Both
effects exist in real recompiled shaders too, so the numbers are the honest ones for this shape,
but they are not a per-load hardware cost.

## Raw output

```
=== run 1 ===
device 0: NVIDIA GeForce RTX 5090
device 1: AMD Radeon(TM) Graphics

selected: NVIDIA GeForce RTX 5090
driver version: 616.64.0, api 1.4.351
timestampPeriod: 1.0000 ns
dispatch: 16384 groups x 64 threads = 1048576 invocations, 16 loads per iteration
guest memory: 257 MB mapped in 16448 pages of 16 KB; page table 512 MB

uniform   A ssbo    iterations=9299
  samples (ms): 20.334 20.664 20.425 21.372 21.006
  min 20.334 ms, 0.13 ps/load

uniform   B bda     iterations=4071
  samples (ms): 29.469 29.120 27.919 30.098 28.406
  min 27.919 ms, 0.41 ps/load

uniform   C bda+srt iterations=2722
  samples (ms): 30.021 27.536 29.157 30.239 27.525
  min 27.525 ms, 0.60 ps/load

strided   A ssbo    iterations=1645
  samples (ms): 29.154 28.668 29.827 29.219 29.338
  min 28.668 ms, 1.04 ps/load

strided   B bda     iterations=1305
  samples (ms): 26.953 27.949 27.410 27.074 27.979
  min 26.953 ms, 1.23 ps/load

strided   C bda+srt iterations=1281
  samples (ms): 28.387 27.779 28.486 28.250 27.795
  min 27.779 ms, 1.29 ps/load

scattered A ssbo    iterations=37
  samples (ms): 29.377 29.058 29.500 29.361 29.053
  min 29.053 ms, 46.80 ps/load

scattered B bda     iterations=37
  samples (ms): 29.453 29.100 29.432 29.419 29.110
  min 29.100 ms, 46.88 ps/load

scattered C bda+srt iterations=37
  samples (ms): 29.441 29.116 29.433 29.423 29.121
  min 29.116 ms, 46.90 ps/load

fault words set near the mapped range: 0 (expected 0)
sink values differing from the expected 0x5be59250: 0 of 256 (expected 0)

| pattern | variant | iterations | min dispatch (ms) | ps / load | ratio vs A | 200M reads (ms) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| uniform | A ssbo | 9299 | 20.33 | 0.13 | 1.00x | 0.03 |
| uniform | B bda | 4071 | 27.92 | 0.41 | 3.14x | 0.08 |
| uniform | C bda+srt | 2722 | 27.53 | 0.60 | 4.62x | 0.12 |
| strided | A ssbo | 1645 | 28.67 | 1.04 | 1.00x | 0.21 |
| strided | B bda | 1305 | 26.95 | 1.23 | 1.19x | 0.25 |
| strided | C bda+srt | 1281 | 27.78 | 1.29 | 1.24x | 0.26 |
| scattered | A ssbo | 37 | 29.05 | 46.80 | 1.00x | 9.36 |
| scattered | B bda | 37 | 29.10 | 46.88 | 1.00x | 9.38 |
| scattered | C bda+srt | 37 | 29.12 | 46.90 | 1.00x | 9.38 |

| pattern | descriptor prologue (ps per invocation) | 1M invocations (ms) |
| --- | ---: | ---: |
| uniform | 3.10 | 0.0031 |
| strided | 0.98 | 0.0010 |
| scattered | 0.39 | 0.0004 |
=== run 2 ===
device 0: NVIDIA GeForce RTX 5090
device 1: AMD Radeon(TM) Graphics

selected: NVIDIA GeForce RTX 5090
driver version: 616.64.0, api 1.4.351
timestampPeriod: 1.0000 ns
dispatch: 16384 groups x 64 threads = 1048576 invocations, 16 loads per iteration
guest memory: 257 MB mapped in 16448 pages of 16 KB; page table 512 MB

uniform   A ssbo    iterations=9305
  samples (ms): 20.388 20.431 21.062 20.582 21.435
  min 20.388 ms, 0.13 ps/load

uniform   B bda     iterations=3939
  samples (ms): 28.573 27.060 26.921 29.512 27.270
  min 26.921 ms, 0.41 ps/load

uniform   C bda+srt iterations=2753
  samples (ms): 30.772 27.678 29.543 27.823 29.649
  min 27.678 ms, 0.60 ps/load

strided   A ssbo    iterations=1639
  samples (ms): 29.158 29.778 30.370 28.870 29.768
  min 28.870 ms, 1.05 ps/load

strided   B bda     iterations=1333
  samples (ms): 28.361 27.939 28.315 28.078 28.653
  min 27.939 ms, 1.25 ps/load

strided   C bda+srt iterations=1278
  samples (ms): 29.055 27.735 28.369 28.009 27.719
  min 27.719 ms, 1.29 ps/load

scattered A ssbo    iterations=38
  samples (ms): 30.313 29.857 30.152 30.111 29.855
  min 29.855 ms, 46.83 ps/load

scattered B bda     iterations=37
  samples (ms): 29.454 29.106 29.451 29.410 29.362
  min 29.106 ms, 46.89 ps/load

scattered C bda+srt iterations=36
  samples (ms): 28.684 28.320 28.650 28.605 28.354
  min 28.320 ms, 46.89 ps/load

fault words set near the mapped range: 0 (expected 0)
sink values differing from the expected 0x4b934240: 0 of 256 (expected 0)

| pattern | variant | iterations | min dispatch (ms) | ps / load | ratio vs A | 200M reads (ms) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| uniform | A ssbo | 9305 | 20.39 | 0.13 | 1.00x | 0.03 |
| uniform | B bda | 3939 | 26.92 | 0.41 | 3.12x | 0.08 |
| uniform | C bda+srt | 2753 | 27.68 | 0.60 | 4.59x | 0.12 |
| strided | A ssbo | 1639 | 28.87 | 1.05 | 1.00x | 0.21 |
| strided | B bda | 1333 | 27.94 | 1.25 | 1.19x | 0.25 |
| strided | C bda+srt | 1278 | 27.72 | 1.29 | 1.23x | 0.26 |
| scattered | A ssbo | 38 | 29.86 | 46.83 | 1.00x | 9.37 |
| scattered | B bda | 37 | 29.11 | 46.89 | 1.00x | 9.38 |
| scattered | C bda+srt | 36 | 28.32 | 46.89 | 1.00x | 9.38 |

| pattern | descriptor prologue (ps per invocation) | 1M invocations (ms) |
| --- | ---: | ---: |
| uniform | 3.07 | 0.0031 |
| strided | 0.70 | 0.0007 |
| scattered | 0.01 | 0.0000 |
=== run 3 ===
device 0: NVIDIA GeForce RTX 5090
device 1: AMD Radeon(TM) Graphics

selected: NVIDIA GeForce RTX 5090
driver version: 616.64.0, api 1.4.351
timestampPeriod: 1.0000 ns
dispatch: 16384 groups x 64 threads = 1048576 invocations, 16 loads per iteration
guest memory: 257 MB mapped in 16448 pages of 16 KB; page table 512 MB

uniform   A ssbo    iterations=9345
  samples (ms): 20.410 21.201 20.582 20.948 20.818
  min 20.410 ms, 0.13 ps/load

uniform   B bda     iterations=3973
  samples (ms): 27.088 29.102 27.510 29.217 27.841
  min 27.088 ms, 0.41 ps/load

uniform   C bda+srt iterations=2711
  samples (ms): 29.581 27.487 29.220 30.556 27.303
  min 27.303 ms, 0.60 ps/load

strided   A ssbo    iterations=1657
  samples (ms): 29.274 28.895 32.130 29.203 29.226
  min 28.895 ms, 1.04 ps/load

strided   B bda     iterations=1313
  samples (ms): 27.011 27.923 27.620 27.210 27.725
  min 27.011 ms, 1.23 ps/load

strided   C bda+srt iterations=1275
  samples (ms): 28.330 27.685 28.349 28.128 27.673
  min 27.673 ms, 1.29 ps/load

scattered A ssbo    iterations=37
  samples (ms): 29.365 29.053 29.352 29.364 29.056
  min 29.053 ms, 46.80 ps/load

scattered B bda     iterations=37
  samples (ms): 29.423 29.384 29.124 29.447 29.107
  min 29.107 ms, 46.89 ps/load

scattered C bda+srt iterations=37
  samples (ms): 29.419 29.393 29.136 29.457 29.116
  min 29.116 ms, 46.90 ps/load

fault words set near the mapped range: 0 (expected 0)
sink values differing from the expected 0x5be59250: 0 of 256 (expected 0)

| pattern | variant | iterations | min dispatch (ms) | ps / load | ratio vs A | 200M reads (ms) |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| uniform | A ssbo | 9345 | 20.41 | 0.13 | 1.00x | 0.03 |
| uniform | B bda | 3973 | 27.09 | 0.41 | 3.12x | 0.08 |
| uniform | C bda+srt | 2711 | 27.30 | 0.60 | 4.61x | 0.12 |
| strided | A ssbo | 1657 | 28.90 | 1.04 | 1.00x | 0.21 |
| strided | B bda | 1313 | 27.01 | 1.23 | 1.18x | 0.25 |
| strided | C bda+srt | 1275 | 27.67 | 1.29 | 1.24x | 0.26 |
| scattered | A ssbo | 37 | 29.05 | 46.80 | 1.00x | 9.36 |
| scattered | B bda | 37 | 29.11 | 46.89 | 1.00x | 9.38 |
| scattered | C bda+srt | 37 | 29.12 | 46.90 | 1.00x | 9.38 |

| pattern | descriptor prologue (ps per invocation) | 1M invocations (ms) |
| --- | ---: | ---: |
| uniform | 3.10 | 0.0031 |
| strided | 1.08 | 0.0011 |
| scattered | 0.23 | 0.0002 |
```
