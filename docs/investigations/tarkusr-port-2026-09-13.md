# Porting the 21 TarkusR commits onto `des-on-599` — 2026-09-13

## Result

**Nothing was ported. All 21 commits are already on `des-on-599`.**

They arrived through the shared base `f69e86d`, not through `main`. `f69e86d` is the merge
`Merge branch 'KytyPS5:main' into demons-souls-shaders`; its second parent is the
`demons-souls-shaders` branch, which carries the same 21 TarkusR patches under different hashes
(`2fead00` … `b5cd9f9`), plus five further commits that build on them.

On `main` the same series appears a second time as `cdb32e6` … `1d6e23a`, a re-application of the
branch onto upstream `bbe3bd0`. Those are the hashes in the porting request. Cherry-picking them
onto `des-on-599` would re-apply work that is already there, on top of the newer form PR 599 and
later upstream commits have since built on — a regression, not a port.

The branch `des-on-599-tarkus` therefore contains **no code commits**, only this document.

## How the two chains line up

`git range-diff bbe3bd0..1d6e23a d2fa865..b5cd9f9` pairs them one to one, in the same order:

| # | requested (on `main`) | twin already in `f69e86d` | range-diff |
|---|---|---|---|
| 1 | `cdb32e6` shader: de-scalarize waterfall descriptor loops | `2fead00` | `!` |
| 2 | `04f2385` loader: attribute guest faults to their module and dump the faulting code | `eed800b` | `!` |
| 3 | `dbebb12` loader: report red-zone sites the patcher could not protect | `6609295` | `=` |
| 4 | `fb24587` renderer: bind unrepresentable sampled textures as null | `ec9bd1e` | `=` |
| 5 | `b8e4b28` shader: null enumerated table slots with reserved texture bits set | `c16fdb8` | `=` |
| 6 | `4f180a8` renderer: grow per-slice depth target binds to the cached slice count | `ce7b6a3` | `!` |
| 7 | `8af240f` shader: convert and pack typed buffer stores | `f55d56b` | `!` |
| 8 | `5045696` renderer: recognise dword pattern fills as clears | `e22b79c` | `=` |
| 9 | `79af497` renderer: clear depth and stencil from the HTILE fill value | `54470a7` | `!` |
| 10 | `05ecbf7` renderer: upload stencil planes written by the GPU | `f38424a` | `=` |
| 11 | `96b457e` gpu: synchronise GPU-written indirect arguments and predicates before the CP reads them | `8f09f3a` | `=` |
| 12 | `1e4af64` gpu: execute indirect dispatches from the host buffer | `cb6c959` | `=` |
| 13 | `f9db9de` renderer: shape null textures after the shader image dimension | `9dbcced` | `=` |
| 14 | `a126fce` shader: map pixel inputs the way the SPI interpolator settings do | `601d184` | `=` |
| 15 | `1f30d40` renderer: name shader modules by guest hash | `74410db` | `=` |
| 16 | `1360dcf` shader: remove phi webs with no non-phi consumer | `8bfb212` | `=` |
| 17 | `4db3baf` shader: branch on the whole wave for execz, execnz, vccz and vccnz | `5744bf3` | `!` |
| 18 | `b881f27` shader: fold thread bits of constant all-zero and all-one masks | `d7bf94e` | `!` |
| 19 | `0cb367b` shader: enumerate loop-bounded and readlane-probed indirect image tables (experimental) | `232f38f` | `=` |
| 20 | `3b27fae` tests: add a selector that runs only the shader cases | `71251ae` | `=` |
| 21 | `1d6e23a` renderer: use the general layout for a sampled depth target outside a feedback loop | `b5cd9f9` | `=` |

`=` means byte-identical patch (14 of 21). `git cherry -v des-on-599 1d6e23a bbe3bd0` marks those
same 14 as already present by patch id.

The seven `!` pairs differ only in surrounding API, and in every case the `f69e86d` twin is the
newer form:

- `cdb32e6`/`2fead00`: CMake hunk context; `ShaderRecompiler.cpp` context (`TranslateResult result;`
  instead of `if (options.stage == ShaderType::Vertex)`).
- `04f2385`/`eed800b`: `runtimeLinker.cpp` context — the twin sits after a `std::fflush(stdout)` the
  `main` copy does not have.
- `4f180a8`/`ce7b6a3`: the twin's test adds `resources.SetGpu(nullptr)` and `context.ShutdownGpu()`
  teardown, and calls `CheckRasterization(false, true)` (two-argument overload).
- `8af240f`/`f55d56b`: the twin calls `RebaseFormattedComponent(mem, info, component)` and
  `EmitBitcastU32ToF32` / `EmitCompareU32Constant`; the `main` copy uses the older
  `(mem, format, component)` and the `EmitTBuffer*` spellings, which no longer exist on this line.
- `79af497`/`54470a7`: the twin threads the HTILE fill through a local `depth_meta_clear` and a
  `ResolveRenderDepthTarget(CommandBuffer&, …)` signature without `submit_id`.
- `4db3baf`/`5744bf3`: the twin's switch has `BranchCondition::ScalarInstruction` where the `main`
  copy still has `GotoVariable`.
- `b881f27`/`d7bf94e`: the twin reads `program.wave_size`; the `main` copy reads
  `current_wave_size`.

No hunk exists on the `main` side that is missing from the `f69e86d` side.

## Evidence that the work is in the tree, not just in the history

For each of the 21, the twin's own diff reverse-applies cleanly to the `des-on-599` working tree
(`git show <twin> | git apply --reverse --check`): 20 of 21 clean.

The one that does not, `2fead00` (waterfall de-scalarization), is the commit PR 599's `2f7729b`
built on. Its substance is present and extended:

- `SyncGpuCleanBacking` — declared `src/kernel/memory.h:116`, defined `src/kernel/memory.cpp:940`,
  called from `pm4Handlers.cpp`, `graphicsRun.cpp` (five sites) and `pipelineCache.cpp`.
- `SrtMemorySync` / `SrtRuntime::sync_memory` — `SrtWalker.h:14,24`, wired in `pipelineCache.cpp`,
  `shaderReadObserver.h`, `ResourceMaterialization.cpp:250`.
- `DescriptorSource::IndirectImage::table_offset` / `key_bound` — `ShaderIR.h:472-473`.
- `WaterfallDescriptor.cpp` is 390 lines on `des-on-599`, against the 361 the commit adds.
- The selector from #20 is present and runs: `shader_recompiler_compute_tests.exe --shader-cases-only`.

The first cherry-pick attempt (`git cherry-pick -x cdb32e6`) confirms the same from the other side:
it reported `CONFLICT (add/add)` on `WaterfallDescriptor.cpp` — the file already exists on the
branch — along with nine content conflicts. That pick was aborted; nothing was committed.

## Where the five extra commits sit

`demons-souls-shaders` continues past the 21 with work the `main` chain does not have at all:

`bac2820` renderer: clear depth to the value a uniform HTILE fill proves ·
`05677f7` renderer: use the general depth layout only when a sampled aspect is written ·
`befaeb0` shader: carry the material key immediate through invariant indirect image tables ·
`e85a81c` shader: support texture tables indexed by a bitmask loop ·
`fbd44f9` shader: support texture tables indexed through a readfirstlane record

These are also already on `des-on-599`, for the same reason.

## Build

`_Build/build-demons-souls.ps1` on the unmodified branch: success, exit 0,
`_Build/windows/kyty_emulator.exe` written. Log `_Build/build-tarkus.log`. No source file was
changed, so this only re-confirms that `des-on-599` builds.

`_Build/build-targets.ps1` was then used to build the test targets the standard script does not
cover. One of them fails to link, on the unmodified branch:

    [347/366] Linking CXX executable kernel_file_system_tests.exe
    lld-link: error: undefined symbol: main

`tests/KernelFileSystemTests.cpp:369` does define `int main()`, so the target's source list or a
guard around it is wrong. Pre-existing; unrelated to any port.

## Tests

`ctest` from `_Build/windows`, whole suite: **46 of 48 passed**.

- `kernel_file_system` — Not Run, the link failure above.
- `fiber_migration` — SEGFAULT. Run directly, `fiber_migration_tests.exe` exits 127 with no output,
  crashing before its first case. The test comes from PR 599's `81ae243`.

Both are baseline failures of `des-on-599`; no code was changed, so there is nothing for the port to
have caused.

`ctest -R shader`: 5 of 5 passed (`shader_cfg`, `shader_vertex_metadata`,
`shader_recompiler_compute`, `shader_recompiler_alignbyte`, `shader_lod_feedback`).

The reported pre-existing crash of `shader_recompiler_compute_tests.exe` — exit 139 after about 51
host cases — **does not reproduce on this line**. The executable run with no arguments exits 0 after
62 `[host]` cases and prints `ShaderRecompilerComputeTests: all cases passed`;
`--shader-cases-only` also exits 0.

## Conclusion

The porting request rests on a false premise. The `main`-side hashes `cdb32e6` … `1d6e23a` are a
duplicate application of a series `des-on-599` already inherits from `f69e86d`, in a form that is
older than what the branch carries. There is no port to do, and doing one would undo work.
