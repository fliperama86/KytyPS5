# KytyPS5 / Demon's Souls

Everything except the game dump lives in one project folder: `C:/Users/dudu/Projects/KytyPS5`.

| Path | Contents |
| --- | --- |
| `C:/Users/dudu/Projects/KytyPS5` | Source, branch `demons-souls-workaround` |
| `C:/Users/dudu/Projects/KytyPS5/_Build` | Build and profiling scripts, `deps/` (Qt 6.10.3, glslang), `windows/` build tree, `build-logs/` |
| `C:/Users/dudu/Projects/KytyPS5/_Runtime` | Emulator install: executables, Qt runtime, `_SaveData`, `_PipelineCache`, `_Diagnostics` |
| `E:/Emulation/PS5/Games/DemonsSouls-PPSA01342-dump` | The game dump, the only part kept on the emulation drive |

Start with **`_Runtime/Play Demon's Souls.cmd`**. It uses the confirmed collision-workaround build and the
current character save. Each launcher sets its own working directory, so they run from wherever the
`_Runtime` folder sits.

- `kyty_emulator.exe`: main executable; `CURRENT-BUILD.json` records its hash and validation status.
- `_SaveData`: current saves.
- `_PipelineCache`: driver pipeline cache. Release builds with a clean tree use it; dirty builds
  disable it and recompile every shader on each run.
- `_Diagnostics`: current and previous run logs, traces and benchmark evidence.
- `Play Demon's Souls - performance.cmd`: the performance experiment with lighter collision tracing
  and Tracy network capture available.
- `kyty_emulator-profile.exe`: profiling/optimization experiment. See
  [performance results](demons-souls-performance.md) for its measurements.
- `Play Demon's Souls - lighter workaround.cmd`: older staged variant.

## Building

`_Build/build-demons-souls.ps1 -Phase Configure|Build|Test|Install` resolves the source from its own
location, so it needs no editing if the folder moves. It expects the Visual Studio Build Tools,
LLVM at `C:/Program Files/LLVM/bin`, and `_Build/deps`.

## History

This layout replaced a split across three source checkouts and a separate runtime folder on `E:`.
The two retired checkouts, and the uncommitted work found in them, are under
`C:/Users/dudu/Projects/_retired`. Investigation notes under `docs/investigations/` were written
before the consolidation and still quote the older paths; `FOLDER-MIGRATION.json` in `_Runtime`
records the earlier runtime move.
