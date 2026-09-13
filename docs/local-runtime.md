# KytyPS5 / Demon's Souls

Everything except the game dump lives in one project folder: `C:/Users/dudu/Projects/KytyPS5`.

| Path | Contents |
| --- | --- |
| `C:/Users/dudu/Projects/KytyPS5` | Source, branch `demons-souls-workaround` |
| `C:/Users/dudu/Projects/KytyPS5/_Build` | Build and profiling scripts, `deps/` (Qt 6.10.3, glslang), `windows/` build tree, `build-logs/` |
| `C:/Users/dudu/Projects/KytyPS5/_Runtime` | Emulator install: executables, Qt runtime, `_SaveData`, `_PipelineCache`, `_Diagnostics` |
| `E:/Emulation/PS5/Games/DemonsSouls-PPSA01342-dump` | The game dump, the only part kept on the emulation drive |

Start with **`Play Demon's Souls.cmd`** at the repository root. There is one emulator binary,
`_Build/windows/kyty_emulator.exe`, the one CMake writes; the launcher runs it in place with
`_Runtime` as the working directory and on PATH, so whatever was built last is what runs. Nothing is
copied or staged. `Play Demon's Souls.cmd profile` runs the same binary with the Tracy profiler
listening; it is on-demand and costs nothing until a capture client connects.

`_Runtime` holds only data and the Qt DLLs:

- `Kyty.ini`: launcher configuration.
- `_SaveData`: current saves.
- `_PipelineCache`: the Vulkan driver's pipeline cache, about 75 MB for Demon's Souls. Saved every
  20 s while new pipelines appear and again on exit, dirty builds included, keyed on the driver
  rather than the emulator revision. It removes driver pipeline compilation on later runs; it does
  not cache shader translation, which is what the first-encounter stutter mostly is.
- `_Diagnostics`: current and previous run logs, traces and benchmark evidence.

## Building

`_Build/build-demons-souls.ps1 -Phase Configure|Build|Test` resolves the source from its own
location, so it needs no editing if the folder moves. It expects the Visual Studio Build Tools,
LLVM at `C:/Program Files/LLVM/bin`, and `_Build/deps`.

## History

This layout replaced a split across three source checkouts and a separate runtime folder on `E:`.
The two retired checkouts, and the uncommitted work found in them, are under
`C:/Users/dudu/Projects/_retired`. Investigation notes under `docs/investigations/` were written
before the consolidation and still quote the older paths; `FOLDER-MIGRATION.json` in `_Runtime`
records the earlier runtime move.
