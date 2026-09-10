> Release notes preserved as published on September 10, 2026. See the [current documentation](../README.md) for subsequent validation.

Experimental Windows x64 build from [7be5d03](https://github.com/fliperama86/KytyPS5/commit/7be5d03f39e6bcdb409b0c7312da2257ea6b4f25), based on TarkusR's `1d6e23a`.

### Download and run

1. Download `KytyPS5-DeS-7be5d03-Windows-x64.zip` and extract the complete folder.
2. Open **Play Demon's Souls.cmd**, add your decrypted game directory in the launcher, and launch the game. You can also drag the game directory onto the CMD file.
3. For an existing character, choose Continue, then Continue Offline. Cross maps to J on the default keyboard layout.

The supplied CMD enables collision serialization with full tracing. The workaround supports **PPSA01342 / 01.005.000 on Windows** and checks the executable's instruction signatures. Starting `launcher.exe` directly does not enable it. Qt and Microsoft VC++ runtime DLLs are bundled.

### Status

Earlier local builds with full capture reached the Nexus and successfully saved/reloaded there, at approximately 4 FPS on an RTX 5090 / Ryzen 9 9950X3D. This release rebuild passed the tests and startup checks below; it has not undergone another full gameplay session.

The underlying synchronization problem and the separate GPU crash after character creation remain unresolved. The lighter tracing mode and extended gameplay are untested. See [the investigation and setup notes](https://github.com/fliperama86/KytyPS5/blob/7be5d03f39e6bcdb409b0c7312da2257ea6b4f25/docs/demons-souls-workaround.md).

### Validation

- Release build with clang-cl 22.1.6, LTO, and Qt 6.10.3.
- All 10 selected CTest cases and shader cases passed.
- Packaged emulator help and GUI startup checked with development-tool directories removed from PATH.
- All 30 packaged PE binaries checked for x64 architecture and dependency resolution.
- ZIP CRC and every packaged file hash verified. Download checksum: `SHA256SUMS.txt`; internal file checksums: `KytyPS5/FILES.sha256`.
