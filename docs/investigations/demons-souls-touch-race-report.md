> Historical investigation notes from September 9?10, 2026. Later entries may supersede earlier findings. See the [current workaround](../demons-souls-workaround.md) and [performance results](../demons-souls-performance.md). Artifact paths identify local captures and may predate runtime-folder consolidation.

# Demon's Souls: collision-list mutation during traversal

**Current installation:** E:/Emulation/PS5/KytyPS5, launched with `Play Demon's Souls.cmd`. On September10 the user requested one emulator folder. The confirmed executable is now named `kyty_emulator.exe`; previous installations are under `_Archive`. Historical CPU-fault-install paths below now resolve relative to this main folder. FOLDER-MIGRATION.json records all moves.

## Reproduction

- Game: PPSA01342, 01.005.000; user's decrypted disc installation and exported PS5 character save.
- Save: Duds, level106 Knight, Nexus; USR-DATA89326bytes, SHA25639c73046683515d42b8302fd8d2491193684e676f25d0cf3652e27bc7abe363e.
- Windows PC: Ryzen9950X3D, RTX5090.
- Launch, skip opening cinematic by holding Cross about4s, choose Continue, then Continue Offline. Kyty maps Cross to J by default.
- Repeated fatal write to0x50 at eboot.bin+0xd8c1f5, with RCX=RDX=0. The unmodified author688d485 binary and the local1d compatibility build reproduce this instruction.

The save file is read successfully. This failure occurs later in a scheduled TouchManager::Think update; reading the file alone does not establish full save deserialization or gameplay.

## Evidence

The original guest instruction removes a paired start node from a temporary active list:

```
rax = end_node->start;
rdx = [rax + 0x50];
rcx = [rax + 0x58];
[rcx + 0x50] = rdx; // crashes with rcx == 0
```

Static guest disassembly and embedded TouchManager::Think/Touch inconsistent strings identify the code. The original eboot is unchanged on disk.

1. Fatal list snapshot (PID28980): all522nodes sorted and doubly linked consistently. Every end follows its start. The failing start was the only one of261 starts with a null temporary previous link.
2. Initial-list and actual traversal capture (PID10900): initial520nodes,495visits. The failing start was absent initially and never visited. Its end appeared during traversal. A preceding node's next pointer changed to the new end during traversal and changed back before the final snapshot.
3. Mutation caller capture (PID23288, navigation automated): insertion of target0x1070e81eb0 was recorded on guest thread18. The failing traversal was guest thread11 on the same list0x2f7a7fb18. Initial526nodes; final530nodes. Target start was absent initially and never visited; target end was visited at index246. This demonstrates cross-thread insertion during the failing sweep.

Target insertion caller RVAs, nearest first:

```
d597c4,8c0cfa,8bf2b8,8bf252,8befc1,8bbf45,14553da,5f7e66,
5f717f,610ad4,6241a9,415c8b,416e1f,417031,4182ec,41cf2d
```

Faulting worker caller RVAs:

```
844885,af4895,821e4e,820f1b,8182a6
```

## Limits and experiment

The captures establish the overlapping list mutation, but not which emulated scheduling/synchronization behavior permits it. They do not demonstrate save corruption. Bounded memory snapshots are not atomic across threads, and instrumentation changes timing.

An opt-in experiment serializes TouchManager traversal and insert/remove/update functions with a recursive host mutex, acquired before guest mutation spinlocks. Update's tailcall into insert releases and reacquires the host mutex. The original atomic XCHG unlock instructions are emulated atomically. Exact game/version and all instruction signatures are checked. All modifications are confined to loaded memory. This is an experimental game-specific workaround, not a general emulator fix.

Experiment PID25904 started11:42:37-03:00, automated Continue Offline at11:45:48. It reached the Nexus, visually confirmed with the character, HUD and environment at frame4335. It remained active through11:52:09/frame5006 at roughly4FPS. Executable kyty_emulator-touch-serialize.exe, SHA2566594aa1866fae3a4ef5cb831687ce346006157db2f61fa432f2a6188b248721f. This is one successful load and several minutes of idle in-game observation, not broad stability testing. No gameplay input was injected after the menu sequence.

The emulator's character USR-DATA was updated at11:49:03, still89326bytes, SHA256d6251cef279fbc9396c12e246c3ac83d39a7643fea875e78bed4f4d155157a13. The original console export remains preserved. Reloading the newly written save has not been tested.

The tested launcher is `Play Demon's Souls - collision workaround.cmd` in the CPU-fault install. A lighter variant, `kyty_emulator-touch-workaround.exe`, omits per-node traversal probes and snapshots by default while retaining serialization. It built successfully, SHA256f7509cb656a6ed5e4f020a9814d292a9e9b9c833267658d8ff48dfc6b3366a16, but has not been launched. Its separate launcher is `Play Demon's Souls - lighter workaround.cmd`. Set KYTY_DEBUG_DES_TOUCH_FULL_CAPTURE=1 to restore detailed tracing in this newer build.

## Local artifacts

- All run logs and analyses: E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-cpu-fault/_Diagnostics/{touch-list,touch-trace,touch-mutation,touch-serialize}.
- Per-binary provenance: same install/LOCAL-PROVENANCE.json.
- Source: src/loader/desTouchTrace.h and runtimeLinker.cpp integration.
- Patches: _Build/touch-mutation-diagnostics.patch; _Build/touch-serialize-experiment.patch.
- Analysis utility: _Build/analyze_touch_trace.py.
- The local1d source also contains earlier graphics experiments; it is not equivalent to the author688 binary. No graphics experiment was changed between these CPU trace runs.
- Separate earlier renderer/device-loss fault remains unresolved in its original character-creation path; it did not occur during the observed Nexus session. The in-game result above is based on captured images, not shader counts alone.
