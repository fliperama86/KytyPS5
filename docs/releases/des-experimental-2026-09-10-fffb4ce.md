> Release notes preserved as published on September 10, 2026. See the [current documentation](../README.md) for subsequent validation.

Windows x64 build of `fffb4cef55c9e537095093bb796ee4509e899c03`, merging TarkusR's `demons-souls-shaders` at `f69e86d` and official KytyPS5 `main` at `2e315a3` while retaining the local Demon's Souls collision workaround and fault diagnostics.

Includes upstream shader, depth-rendering, filesystem, audio, and library updates. The merge preserves LDS wait ordering and storage-image bounds checks, and adapts image-clear recognition and GPU allocation tracing to the updated code.

Download and extract **KytyPS5-DeS-fffb4ce-Windows-x64.zip**, then run **Play Demon's Souls.cmd**. Add your decrypted game directory in the launcher, or drag that directory onto the CMD file. Qt and the VC++ runtime DLLs are included. When updating, retain your `_SaveData` directory.

The workaround supports **PPSA01342 / 01.005.000 on Windows** and enables collision serialization with full tracing. Games, firmware, and saves are not included.

Validation: Windows Release build succeeded; **36 of 37 CTest cases passed** on the merged source, including the shader compute and control-flow suites. The remaining `kernel_file_system` failure is the existing Windows socket limitation combining `PEEK` and `WAITALL`. Packaged command-line and Qt launcher startup checks passed, and all packaged binaries and DLL dependencies were checked for Windows x64. The ZIP was checked against the staged files; download and per-file SHA256 checksums are included.

**Experimental:** this merged source has not yet been tested in a new game session. An earlier local full-capture build reached the Nexus and saved/reloaded at approximately 4 FPS on an RTX 5090 / Ryzen 9 9950X3D. The earlier GPU crash after character creation remains unresolved; other versions, lighter tracing, and extended gameplay remain untested.

[Workaround details and limitations](https://github.com/fliperama86/KytyPS5/blob/fffb4cef55c9e537095093bb796ee4509e899c03/docs/demons-souls-workaround.md) | [Changes since the first release](https://github.com/fliperama86/KytyPS5/compare/7be5d03f39e6bcdb409b0c7312da2257ea6b4f25...fffb4cef55c9e537095093bb796ee4509e899c03)
