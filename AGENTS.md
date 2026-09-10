# Working in this repository

Fork of KytyPS5 focused on getting Demon's Souls (PPSA01342) to run well. Read
[docs/README.md](docs/README.md) for the documentation index.

## Layout

- One emulator binary: `_Build/windows/kyty_emulator.exe`, written by CMake. Nothing is copied or
  staged. Build with `_Build/build-demons-souls.ps1` or `_Build/build-targets.ps1 -Targets <t>`.
- `_Runtime` is the working directory: saves, pipeline cache, `Kyty.ini`, Qt DLLs. The game dump
  lives outside the project (`E:/Emulation/PS5/Games/DemonsSouls-PPSA01342-dump`).
- Launch with the root `Play Demon's Souls.cmd`; `profile` as the first argument enables Tracy.
- `_Build` and `_Runtime` are ignored by git. Local tooling scripts live in `_Build`.

## Game procedure

[docs/reaching-the-nexus.md](docs/reaching-the-nexus.md) describes how to reach the benchmark scene
(wait for frame 700, hold Cross 2 s, then about twelve Cross presses 3 s apart) and how to measure.

## Rules

- Push only to the `fliperama86` fork. Never push to `origin` (official KytyPS5) or `upstream`.
- Work on `main`. Keep any branch short-lived. Game-specific or experimental behaviour goes behind
  a compile flag or a runtime setting so `main` stays safe for other games.
- No attribution lines in commit messages or pull request descriptions.
- Steer performance work by frame rate and Tracy zone self time on the render thread, not by
  whole-process CPU share or by sampled symbol names (this LTO build mis-attributes them).
- Document findings in `docs/` as you go and commit them with the code.
