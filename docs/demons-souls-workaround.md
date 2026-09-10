# Demon's Souls experimental workaround

This branch starts at TarkusR/KytyPS5 commit
`1d6e23a31693fa3eaf547d2c48f63626d6301baf` and preserves the local shader
compatibility changes, GPU fault diagnostics, and collision tracing used during
the Demon's Souls investigation.

The collision serialization experiment reached the Nexus on Windows with
**PPSA01342, version 01.005.000**, using a character save exported from the owner's
PS5. The owner confirmed gameplay. Saving through the game's menu, closing the
emulator, and reopening that save also returned to the Nexus. Observed performance
was approximately 4 FPS on a Ryzen 9 9950X3D and GeForce RTX 5090 with full tracing.

This is a game-specific experiment. The underlying emulator synchronization
problem remains unresolved. The earlier GPU device-loss crash after character
creation is also unresolved; it did not occur during the observed Nexus sessions.
Other regions, game versions, extended gameplay, and Linux/macOS runtime behavior
have not been validated for this workaround.

## Build and run

Follow the main README's build instructions using this branch. When configuring
a local build, use these metadata values in addition to the normal CMake options:

```text
-DKYTY_BUILD_ORIGIN=Fork
-DKYTY_BUILD_REPOSITORY=fliperama86/KytyPS5
```

Run from the emulator installation directory with your own decrypted game folder
and save. In PowerShell, enable serialization and the full tracing mode used by
the successful local build:

```powershell
$env:KYTY_DEBUG_DES_TOUCH_TRACE = '1'
$env:KYTY_DEBUG_DES_TOUCH_SERIALIZE = '1'
$env:KYTY_DEBUG_DES_TOUCH_FULL_CAPTURE = '1'
$env:KYTY_DEBUG_GUEST_FAULT = '1'
$env:KYTY_DEBUG_DES_TOUCH_LIST = '1'
& .\kyty_emulator.exe --game 'D:/Games/DemonsSouls-PPSA01342'
```

The loader checks the title, version, and expected instructions before installing
any probes. A mismatch stops the diagnostic launch. Only loaded executable memory
is patched; this code does not modify the game executable or save files on disk.
The running game still writes saves normally.

At the opening cinematic, hold Cross for about four seconds. Choose Continue,
then Continue Offline. Cross maps to J on the default keyboard layout. Initial
level shader compilation can take several minutes.

The successful executable predates the optional lighter capture mode now in this
source. Setting `KYTY_DEBUG_DES_TOUCH_FULL_CAPTURE=1` retains the full tracing
behavior; the current source has also been built with the lighter mode available.
Omitting that variable while serialization is enabled removes three per-node
probes and the initial list snapshots. That lighter mode has not been tested in
the game, and no performance improvement is established.

## Collision failure and evidence

Repeated loads failed on a write to address `0x50` at `eboot.bin+0xd8c1f5`, inside
the game's TouchManager traversal. An end node referred to a paired start whose
temporary active-list links were null.

Bounded captures showed:

1. A final list with 522 consistently linked and sorted nodes. The failing start
   was the only one of 261 starts with a null temporary previous link.
2. A traversal where the failing start was absent from the initial snapshot and
   was never visited, while its end appeared during the traversal.
3. An insertion recorded on guest thread 18 while guest thread 11 was traversing
   the same list. There were 526 initial nodes and 530 final nodes; the new end
   was visited at index 246 without visiting its start.

These observations establish overlapping list mutation during the failing sweep.
They do not establish save corruption or identify the emulator behavior allowing
that overlap. Snapshots are bounded and not atomic, and instrumentation affects
timing.

`src/loader/desTouchTrace.h` installs UD2 probes and handles them through the host
exception handler. A recursive host mutex serializes traversal and the game's
insert/remove/update functions, acquired before the guest mutation spinlocks.
The update-to-insert tailcall releases and reacquires that mutex. Original MOV
instructions are reproduced in the saved CPU context, and XCHG unlock operations
are reproduced atomically. Full serialization mode installs 12 probes; lighter
mode installs 9.

For investigation without serialization, enable `KYTY_DEBUG_DES_TOUCH_TRACE=1`
and remove `KYTY_DEBUG_DES_TOUCH_SERIALIZE` from the environment. Include the two
guest fault diagnostic variables to obtain the fatal list and register dump.
Analyze a resulting console log with:

```text
python tools/analyze_touch_trace.py path/to/console.log
```

The analyzer writes `touch-trace-analysis.json` alongside that log.

## Other changes retained in this branch

- Shader resource tracking accepts additional indirect-image patterns: scalar
  material-key immediates, zero-stride material buffers, read-first-lane probes,
  and guarded find-LSB indices into dense descriptor tables.
- Shader emission retains LDS waits as subgroup execution/memory barriers and
  adds coordinate bounds checks to storage-image writes. Regression cases cover
  these behaviors. These changes alone did not resolve the earlier GPU crash.
- GPU device-loss diagnostics report supported Vulkan fault information and
  NVIDIA shader checkpoints when graphics debug dumping is enabled.
- `KYTY_DEBUG_GPU_FAULT_FILE` selects a vendor fault-binary output path;
  `KYTY_DEBUG_NV_FAULT_DETAILS=1` enables additional supported NVIDIA diagnostics.
- `KYTY_DEBUG_RESOURCE_TRACE` selects a bounded binding-history output path for
  two shaders implicated in the earlier GPU failure.
- The presence of `KYTY_DEBUG_SERIALIZE_DISPATCH` enables a separate compute
  dispatch serialization experiment. It is not needed for the collision
  workaround and did not resolve the earlier GPU failure.

## Validation

The local Release build uses clang-cl, LTO, and Qt 6.10.3 on Windows. Validation
covers the launcher build, resource-tracking tests, shader cases (including LDS
exchange and out-of-bounds storage writes), and scheduler/stream-buffer tests.
The runtime evidence is limited to the full-capture Nexus load and save/reload
described above; it is not a claim of broad game compatibility.
