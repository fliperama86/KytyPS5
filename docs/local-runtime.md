# KytyPS5 / Demon's Souls

Runtime directory: `E:/Emulation/PS5/KytyPS5`.

Start with **Play Demon's Souls.cmd**. This uses the confirmed collision-workaround build and the current character save.

- `kyty_emulator.exe`: main executable; CURRENT-BUILD.json records its hash and validation status.
- `_SaveData`: current saves; preserved during consolidation.
- `_Diagnostics`: current and previous run logs.
- `_Archive`: older installations, diagnostic binaries and their historical saves/logs. Historical records may contain the original paths; see FOLDER-MIGRATION.json.
- `Play Demon's Souls - performance.cmd`: launches the tested performance experiment with lighter collision tracing and Tracy network capture available.
- `kyty_emulator-profile.exe`: current profiling/optimization experiment. See [performance results](demons-souls-performance.md) for its measurements.
- `Play Demon's Souls - lighter workaround.cmd`: older staged variant. The current merged source has been tested with lighter tracing; see the performance report for the exact binary.

Use this folder for future installations. Active source: C:/Users/dudu/Projects/KytyPS5-DeS-1d6e23a. Its build scripts install here and archive the previous executable before replacement.
