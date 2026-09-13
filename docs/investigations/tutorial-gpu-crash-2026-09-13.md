# Tutorial-load GPU crash: bisected against PR 599, fixed by the compact pixel-shader LDS layout

Date: 2026-09-13. Machine: RTX 5090, Ryzen 9 9950X3D, local console session. Dump: PPSA01342, 01.005.000.

## Symptom

New Game, skip the cinematic, tutorial load: `vkWaitSemaphores: ErrorDeviceLost`. The NVIDIA
fault report names compute shader `df890e8a1a32c65a` (the 214x120x72 froxel volume pass) with an
instruction-pointer fault at `0x20009e100` and a `WriteInvalid` page fault. This is the crash of
[the September 10 report](demons-souls-crash-report.md). Four runs of main `5d88f53` reproduced it
four times. The Nexus, loaded from the imported save, never hits it.

## Bisect

Upstream draft PR KytyPS5/KytyPS5#599 survives the same load on this dump. Its own work is eleven
commits on top of the base main already shares (`f69e86d`). Each row is one New Game drive:

| Build | Tutorial load |
| --- | --- |
| main `5d88f53` | crash |
| base + PR commit 1 (`81ae243`, fiber TLS, save capacity) | crash |
| base + commits 1-2 (`2f7729b`, descriptor addressing, texture feedback, SRT masks) | survives |
| base + commits 1-3, 1-5, full PR | survives |
| commit 1 + `SrtWalker.cpp` from commit 2 | crash |
| commit 1 + `SrtWalker.cpp`, `WaterfallDescriptor.cpp`, `ResourceTracking.cpp` from commit 2 | crash |
| commit 2 with LOD-stats feedback disabled | survives |
| commit 2 with LOD-stats and the compact LDS layout disabled | crash |
| main + the compact LDS layout ported | survives |

The faulting shader's SPIR-V is byte-identical between commit 1 and commit 2 (`spirv-val` clean),
so the shader itself was never wrong. Its image-write coordinates are pure thread ids. What changes
is every pixel shader that touches LDS: the base emits an invocation-private `Function` array of
`lds_size_dwords`, or 8192 dwords when the workgroup input carries no size, for each of them.
`PlanFunctionLdsLayout` maps the accesses (`LaneId << 2` plus a constant offset) to one slot per
distinct offset and sizes the array to that count. 14 pixel shaders in the tutorial shrink as a
result. The mechanism by which the oversized private arrays fault a later compute dispatch is not
established; the bisect is.

## Port

`src/graphics/shader/recompiler/ir/passes/FunctionLdsLayout.h` is the PR's file verbatim. The
emitter changes are the PR's: `EmitterState` gains `compact_lds_dwords`, `function_lds_slots` and
`function_lds_index_slots`; `SpirvEmitter.cpp` plans the layout after `state.stage` is set;
`DwordIndex` records the slot of each LDS access; `EmitMemoryElementPointer` substitutes the slot
index; `EmitProgram` sizes the array with `LdsStorageDwordCount`. The original address and bounds
checks are kept, only the storage indices change. Not game-specific, no gate.

## Not the fix, ruled out on this dump

The evaluator rewrite, the waterfall table-offset and mask changes, the merge-Phi predecessor
change, the permutation-vector lifetime change (commit 9), the RBX clobbers (commit 7), and the
LOD-stats feature. The two loader patches of the PR's Demon's Souls profile were not needed either:
commit 2 alone passes with the profile off.

## Artifacts

Bisect worktree `C:\Users\dudu\Projects\KytyPS5-bisect` (detached, own build tree, launcher
`Play Bisect.cmd`); shader dumps in its `_Build\shaders-81ae243` and `_Build\shaders-2f7729b`;
fault traces `_Build\newgame2-*` to `newgame4-*` in the main checkout; `spirv-dis`/`spirv-val`
built from the SPIRV-Tools submodule in the session scratch directory.
