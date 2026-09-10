> Historical investigation notes from September 9?10, 2026. Later entries may supersede earlier findings. See the [current workaround](../demons-souls-workaround.md) and [performance results](../demons-souls-performance.md). Artifact paths identify local captures and may predate runtime-folder consolidation.

# Demon's Souls investigation

Local game: PPSA01342, content version 01.005.000, decrypted from the user's PS5.
PC: Ryzen 9 9950X3D, RTX 5090, Windows NVIDIA driver 32.0.16.1664.
The user reported an X post showing commit 1d6e23a running version 01.004.000.
That report has not been independently verified and is a different game version.

| Build / configuration | Result on the local 01.005.000 dump |
| --- | --- |
| 6c6e3e7, Release + LTO | User reported failure waiting for a Vulkan timeline semaphore after VS 42 / PS 60 / CS 263. |
| 6c6e3e7 plus semaphore error diagnostics | Reproduced after character creation (user clarification): vkWaitSemaphores returned ErrorDeviceLost (-4), RTX 5090. |
| Same diagnostic build, shader optimization None | Reproduced ErrorDeviceLost (-4), waiting=165192 completed=165191 next=165193. |
| Same diagnostic build, Vulkan core validation | Reached intro. Stopped on user steering to try 1d6e23a; no conclusion about the crash scene. |
| Exact 1d6e23a, Release + LTO | Failed before menu: compute hash 0xfb0becc9db83db77, PC 0x1044, GetImageResource dword 0 is not a valid runtime value. |
| 1d6e23a plus only upstream 865cc0d | Passed resource-tracking tests, reached character creation, then failed tracking pixel shader 0x06240562ae88d4f0 at PC 0xfc (bitmask loop). |
| 1d6e23a plus 865cc0d, 59f7e2b, 6c6e3e7 | Resource-tracking tests passed. User finished character creation and cinematics rendered correctly, then crashed before gameplay. Shader subtree matches 6c6e3e7 exactly; renderer subtree matches 1d6e23a exactly. |
| Same hybrid, core validation | Same timeline semaphore wait assertion, without a Vulkan validation error beforehand. |
| Same hybrid, full GPU-assisted validation | Stopped earlier in compute shader 0x934a6e1197b99148: LDS load in invocation 0 races a store in invocation 19. This is not yet established as the gameplay crash cause. |
| Same hybrid, GPU-assisted address checks with shared-memory race check disabled | PID 30008 exited with the GPU wait assertion after VS42/PS60/CS259. No out-of-bounds report preceded it. Last new shaders: PS78ddc27714e07d6f and VS099f6dea04eb6e1d. |
| Hybrid plus local LDS wait synchronization fix, core validation | PID 35156 failed with the same GPU wait assertion after VS46/PS70/CS262. The original race shader now contains 41 SPIR-V control barriers (previously zero). This did not fix the gameplay crash. |
| Same LDS variant plus driver fault/checkpoint diagnostics | PID 28464 exited after VS43/PS73/CS262. EXT fault reports WriteInvalid at GPU address 0x6ed250000, instruction pointer fault 0x20009af80. NV top/bottom checkpoints both identify cs df890e8a1a32c65a. |

## Installed packages

- E:/Emulation/PS5/KytyPS5-DeS-6c6e3e7 — original build remains intact; diagnostic executable is separate.
- E:/Emulation/PS5/KytyPS5-DeS-1d6e23a — exact unmodified baseline executable.
- E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-compat — baseline plus the three shader-only descriptor fixes. Material-key-only executable backed up as kyty_emulator.865cc0d-only.exe.bak.

The original project's loader diagnostic patch is preserved in C:/Users/dudu/Projects/KytyPS5.
The 6c6e3e7 source checkout has only the semaphore diagnostics change.
The 1d6e23a source checkout contains the upstream 865cc0d, 59f7e2b and 6c6e3e7 patches, including their tests. The experimental LDS wait fix changes ValueOpcodes.cpp, spirvEmitterFlow.cpp and ShaderRecompilerComputeTests.cpp. It is deployed only as kyty_emulator-lds-sync.exe; the compatibility executable is preserved.
The preserved compatibility and LDS-sync executables retain the original 1d6e23a renderer.
The current source additionally contains optional device-fault/checkpoint diagnostics in
graphicContext.h, masterSemaphore.cpp, renderCompute.cpp, renderDraw.cpp, and vulkanWindow.cpp.
These report errors and insert NV markers when graphics debug dumping is enabled;
they do not replace the renderer scheduling or resource logic.

## Logs and backup

- E:/Emulation/PS5/KytyPS5-DeS-6c6e3e7/_Diagnostics/20260909-vulkan
- E:/Emulation/PS5/KytyPS5-DeS-6c6e3e7/_Diagnostics/20260909-no-shader-opt
- E:/Emulation/PS5/KytyPS5-DeS-6c6e3e7/_Diagnostics/20260909-validation
- E:/Emulation/PS5/KytyPS5-DeS-1d6e23a/_Diagnostics/first-run
- E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-compat/_Diagnostics/first-run
- E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-compat/_Diagnostics/shader-fixes
- E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-compat/_Diagnostics/validation
- E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-compat/_Diagnostics/gpu-assisted
- E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-compat/_Diagnostics/gpu-address
- E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-compat/_Diagnostics/lds-sync
- E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-compat/_Diagnostics/device-fault

The first directory contains save-before-debug.zip (original options profile).
It also contains a roughly 895 MB verbose kyty.log. Shader logging is primarily written
inside that log; actual binary shader dumps require --graphics-debug-dump true.
Later runs use normal console logging to avoid unnecessary verbose output.

The user controls the game to reproduce crashes. Do not inject keys or mouse input.
The local des-window.ps1 helper may inspect/capture/show a specific emulator window.

## Portable validation layer

_Build/validation-layers contains the Khronos validation DLL from the official
KhronosGroup/Vulkan-ValidationLayers Windows amd64 CI artifact, run 34379166999,
main commit 0b5c5e36ae38ebada5afb0d8424bbc623f45ea4f. Both Windows jobs succeeded;
the full cross-platform run had a failure elsewhere. Its manifest was generated
from that exact commit's official template by substituting layer name and DLL path.
Use a process-local VK_LAYER_PATH to load it; nothing was registered system-wide.

GPU address run additionally uses process-local VK_LAYER_GPUAV_SHARED_MEMORY_DATA_RACE=false,
VK_LAYER_VALIDATE_CORE=false, VK_LAYER_STATELESS_PARAM=false,
VK_LAYER_OBJECT_LIFETIME=false, and VK_LAYER_THREAD_SAFETY=false.
The executable and shader code are unchanged for that diagnostic run.

## LDS wait investigation

GPU-AV reported cs 0x934a6e1197b99148, local invocation 0 vs 19,
SPIR-V OpLoad result 2145 vs OpStore pointer 819/value 749. The original shader
has LDS stores then S_WAITCNT 0xc07f at guest PC 0x24c before LDS reads at 0x260.
Its emitted SPIR-V has no memory/control barriers. The translator emits a Waitcnt
IR marker, dead-code elimination removes it, and the backend treats it as a no-op.
Captured decoded shader is in _Build/race-shader-decoded.txt.
The validation layer documents limitations in shared-memory race detection;
the report is evidence to investigate, not proof of the final crash cause.

Experimental fix retains Waitcnt as a side effect and emits one subgroup-scope
acquire/release workgroup-memory execution barrier per wait in shaders using LDS.
Wave64 on a 32-lane host emits it once after both halves. This deliberately keeps
the synchronization wave-local, since different guest waves may branch differently.
It currently conservatively treats all wait counters alike when LDS is present.
New Wave32/64WaitcntLdsExchange tests exchange values through LDS over four rounds,
with two guest waves in a workgroup and no guest S_BARRIER. Baseline failed the
missing-barrier check. Both GPU tests and all 319 shader cases pass with the fix.
Resource tracking also passed. Logs are in the 1d6e23a source _Build directory:
lds-waitcnt-before.log, lds-waitcnt-after.log, lds-shader-cases.log.

## Driver fault diagnostics

Separate executable kyty_emulator-device-fault.exe SHA256
4F48E855AA858D0896E4749BC3C9A3DD52AEE98E8F5773F989A086B99BE361BB.
Uses the LDS synchronization variant plus VK_EXT_device_fault and
VK_NV_device_diagnostic_checkpoints, enabled only with --graphics-debug-dump true.
Each ordinary draw records its pixel hash (vertex hash if no pixel shader), and
each normal compute dispatch records its compute hash. Utility operations/mesh draws
are not separately marked, so checkpoints narrow the pending interval, not proof
that the marked shader itself caused the fault. Query on semaphore ErrorDeviceLost
prints checkpoint stages and EXT fault description/address/vendor fields.
No global GPU, TDR, driver, or registry settings have changed.
Source patches saved under the 1d6e23a _Build directory as
lds-waitcnt-fix.patch and device-fault-diagnostics.patch.
The build-compat.ps1 Install phase still targets the base compatibility executable;
do not use Install for the diagnostic variants. Copy the build output to its separate name.

First driver fault report: getFaultInfo counts Success (2 addresses, 0 vendors),
details Incomplete, blank description. Address records are InstructionPointerFault
0x20009af80 (precision 0x10), WriteInvalid 0x6ed250000 (precision 0x1000).
Both TopOfPipe/BottomOfPipe checkpoints: shader df890e8a1a32c65a.
That compute shader has only two OpImageWrite instructions (wave64 halves), no
buffer/physical-pointer writes, and no LDS. Its output is a 2D-array storage image.
Saved original decoded/IR source excerpt to _Build/fault-shader-ir.txt (6699 lines).
The last compiled shaders remain 2bb42a95967c8a1b and 5cab554642d70f6b, demonstrating
why last-compiled order alone did not identify the suspect shader.
Next run uses the same executable and scoped GPU-AV by shader debug-name regex:
VK_LAYER_GPUAV_SELECT_INSTRUMENTED_SHADERS=true
VK_LAYER_GPUAV_SHADERS_TO_INSTRUMENT=.*df890e8a1a32c65a.*
VK_LAYER_GPUAV_DEBUG_DUMP_INSTRUMENTED_SHADERS=true
Core/stateless/object/thread validation disabled for this run, like gpu-address.
Shared-memory race checking retains its default (the selected shader has no LDS).
Logs: _Diagnostics/scoped-gpuav. See emulator.pid for the process.

Scoped run PID 32260 failed after VS52/PS76/CS269, further in shader compilation
than previous failures. No GPU-AV validation error preceded GPU loss. New NV
top/bottom marker is compute 92760f03dce9b80d; driver reports WriteInvalid at
0x7f4cce000 and InstructionPointerFault at 0x2011fcc70. Only df890e8a1a32c65a
was instrumented, confirmed by dump_1_before.spv (257160 bytes) and dump_1_after.spv
(275140 bytes), copied from install root into _Diagnostics/scoped-gpuav.

Next isolated diagnostic: kyty_emulator-serialized.exe, PID 28980,
SHA256 49FE3427B3DBCC4E1C5D87B7BBB4DC0174C4D35E2288C0E17E2646940D3C2E21.
Process environment KYTY_DEBUG_SERIALIZE_DISPATCH=1 makes RenderExecutor flush
and wait after each normal compute dispatch. This is a diagnostic timing change,
not an accepted fix. It is disabled unless that environment variable is present.
Core validation and graphics debug dumping are enabled; GPU-AV is off.
Logs: _Diagnostics/serialized-dispatch. Play Demon's Souls - serialized test.cmd
sets the required variable for a manual repeat. It may reduce performance.

This binary also marks internal GPU operations:
ffff000000000001 = fault-buffer processing
ffff000000001000 + pipeline_slot = tiling/detiling
ffff000000000002 = D16 conversion
ffff000000000003 = BGRA16 channel conversion
ffff000000000004 = blit helper
The driver fault query now allocates the reported vendor binary capacity rather
than forcing zero, to avoid truncating the extended report.

Serialized PID 28980 also failed, after VS41/PS58/CS263. Checkpoints now include
utility GPU work, but both markers were still df890e8a1a32c65a. The wait failed
inside the immediate FlushAndWait after compute dispatch, so this narrows the
failure to that dispatch rather than a later CPU readback. Fault query Success:
InstructionPointerFault 0x20009af80, WriteInvalid 0x72f0e4000; vendor binary size
4936840 bytes (collected but not saved), description blank. Serialization is not a fix.
Read-only direct Vulkan feature query confirms RTX5090 and AMD iGPU both support
robustBufferAccess2=1, robustImageAccess2=1, nullDescriptor=1.

Current test PID 28608: kyty_emulator-image-bounds.exe
SHA256 01BDA5C91760A7FBA95A6578A631F72C3DCC82ECED4C7B1DDC25405D3822BB35.
Same source plus explicit bounds checks around storage-image writes in
spirvEmitterAnalysis.cpp: query the selected view/mip size, compare every unsigned
coordinate (including array layer), and discard out-of-range writes. This is an
experimental check/workaround; host robustness was already requested by the app.
The regression tests negative/too-large 2D coordinates while preserving valid
pixels. All 320 shader cases pass, plus resource_tracking. Logs:
1d6e23a/_Build/image-bounds-shader-cases.log and _Diagnostics/image-bounds.
Serialization is OFF for this run; GPU-AV is OFF; core validation is ON.
User controls the test. The corresponding manual launcher is
Play Demon's Souls - image bounds test.cmd.

Last inspection: PID 28608 is alive at the title screen, PRESS ANY BUTTON, around
frame 8829 / 38 FPS. No validation error. The user must advance it; no input was
injected. Gameplay outcome of the image-bounds variant remains pending.
The async question about whether the preceding LDS-sync run failed after creation
was not answered. Do not infer a more precise crash stage from shader counts alone.

Full current source diff from clean 1d6e23a is saved as
1d6e23a/_Build/demons-souls-current-experimental.patch. Prefer that complete diff;
earlier per-experiment patch exports can overlap in the shared test file.

2026-09-10 image-bounds outcome: user reports the same crash. PID28608 exited.
Both queue checkpoints remain df890e8a1a32c65a; driver reports InstructionPointerFault
0x20009e100 and WriteInvalid 0x7350e4000. Details query Success, vendor binary
5778056 bytes (not saved by that build). Wait2371915/completed2371914/next2371916.
The explicit image-coordinate guards did NOT solve entry into gameplay.
Next diagnostic captures a bounded history of the two suspect compute shaders'
actual bound image views/backings/allocations and saves the vendor fault binary.

2026-09-10 resource trace diagnostic PID27944:
kyty_emulator-resource-trace.exe SHA256
77A7A45904D3A5396EACCE703F21C144F61140FFF766F3EA350586119BB6817D.
Only diagnostic changes relative to image-bounds: bounded, mutex-protected CPU
history of last128 dispatches for df890e8a1a32c65a and 92760f03dce9b80d; records
queue tick, group dimensions, native descriptors, actual VkImage/view details,
VMA allocation size/offset/type. Does not touch cache LRU state or GPU contents.
KYTY_DEBUG_RESOURCE_TRACE names the history file written on device loss.
KYTY_DEBUG_GPU_FAULT_FILE names the full EXT vendor binary written on device loss.
Both paths in _Diagnostics/resource-trace. First entry also printed to console.
New header src/graphics/host_gpu/renderer/gpuFaultTrace.h is untracked and included
in _Build/demons-souls-current-experimental.patch (manual new-file diff appendix).
Build succeeded; resource_tracking, command_scheduler_timeline, stream_buffer_ring
all passed. Existing shader changes unchanged; their previous320-case pass remains.
GPU-AV and serialized dispatch are off; core validation and graphics debug dump on.
No input injected. Gameplay outcome pending user's navigation.

User steered to checking GitHub forks/discussions and Reddit. Research complete:
see1d6e23a/docs/investigations/github-reddit-research-2026-09-10.md.
Crucial new evidence: Myoko explicitly reported01.005.000 then gameplay success
with author-attached688d485 package. Exact package installed separately at
E:\Emulation\PS5\KytyPS5-DeS-688d485-author. Identity/CLI checked; local gameplay
untested. Sourcehash unavailable through GitHub; cannot infer equivalence to6c6e3e7.
Current diagnostic27944 remains running, no input injected. Staged author package
is the next recommended controlled comparison before further workaround patches.

Update: diagnostic27944 crashed while research was in progress. Captures saved:
resource-bindings.log369844B and gpu-fault.bin5484544B in_Diagnostics/resource-trace.
Stilldf890e8a1a32c65a, IP0x20009e100, WriteInvalid0x7aced0000,
waiting791671/completed791670/next791672. Last trace dispatch1226 atframe10987,
tick791671; output R16G16B16A16Sfloat214x120,layers72,mips1,General,allocation
size0xf30000=requirement0xf30000. Requested/actual view2DArraylayer0/72.
Output image recreated fromid473:6 atpreviousframe toid119:719 onfaultframe,
all otherinputIDsunchanged. Lots ofoutputbackingchurn; potentiallifetime/clobber
avenue ifauthorbaselinealsofails. No proof ofa staleview fromthisrecordalone.
Onlyexperimentaldiagnosticschangedthisturn; nouserinputinjected.
Nexttestusesunmodifiedauthor688d485 package, seeauthor_Diagnostics/first-run/PID.


2026-09-10 author package outcome: PID29400 failed. Unmodified688d485 ended at
VS46/PS76/CS260, masterSemaphore.cpp:53. No GPU details in that binary. Do not
claim that exact build has the same shader fault solely from this generic wait.

Offline resource-trace dump decoded with official NVIDIA Aftermath SDK2026.3:
Error_DMA_PageFault, write unmapped GPUVA0x7aced0000, faulted compute PC+0xd00.
Aftermath hash0xeed92266c3b35f03 independently matches df890 saved SPIR-V.
No source mapping: missing eed92266c3b35f03-305a7cf0470fbff1.nvdbg.
Decoder artifacts gpu-fault.json and gpu-fault.nv-gpudmp beside original binary.
Standalone compile/bind tool did NOT dispatch shader; no debug callback/internal
representations supplied. The statistic0x1000000010 is not established as64GB
actual local memory. No shader fix follows from that statistic.

New separate diagnostic: E:\Emulation\PS5\KytyPS5-DeS-1d6e23a-compat\kyty_emulator-nv-fault-details.exe
SHA256 3E64B1AFABE496DEE7DE8BD30885123CB5623C272AF6312CF3B3D6151DDC47F4
PID 19428; logs E:\Emulation\PS5\KytyPS5-DeS-1d6e23a-compat\_Diagnostics\nv-fault-details-20260910-010945
Only source change since resource-trace: opt-in NVIDIA diagnostics extension and
supported feature chain, resource tracking, shader debug info, shader error
reporting, supported vendor-binary flag. Enables only when graphics debug dump
is on and KYTY_DEBUG_NV_FAULT_DETAILS=1. No global driver setting or SDK injection.
Build passed. User navigation needed; not a fix and may change shader compilation.
Draft report: 1d6e23a/docs/investigations/demons-souls-crash-report.md. Not posted externally.
Startup verification: PID19428 created its Vulkan device successfully with core
validation enabled, and stdout confirms all three NVIDIA fault detail flags plus
EXT device fault and NVIDIA checkpoints enabled. Window shown atframe901/~49FPS.
No validation errors seen at startup. User must advance; no input injected.


## NVIDIA detail capture outcome, 2026-09-10

PID19428 exited. Final counts VS60/PS102/CS266. User reports the run is done; exact visual stage remains awaiting their clarification. Do not infer gameplay success from shader counts.

The diagnostic succeeded in adding embedded shader debug mapping. It failed at the same native instruction address0x20009e100 in df890e8a1a32c65a, with a write to unmapped GPUVA0x7727e6000. Waiting357091, completed357085, next357092; the final df890 dispatch was frame4009/tick357086. Dump size6,734,592 bytes. NVIDIA decoder: Error_DMA_PageFault, engine reset, one active compute shader, Aftermath hash0xeed92266c3b35f03, native size73,472 bytes.

Faulted warp PC+0xd00 now maps to comp.10000.spv:1286. In the standard SPIRV-Tools disassembly with its five header lines, line1286 is:

    %1165 = OpLoad %uint %1163
    %1163 = OpAccessChain %_ptr_StorageBuffer_uint %flattened_srt %uint_0 %uint_81

The synthetic filename/line is the driver's intermediate-language mapping, not a C++ source location. Without native assembly this is an association to the intermediate instruction, not proof of the exact native store operation. The only explicit shader writes remain the two output image writes near the end of the SPIR-V. Static SRT loads span dwords0..91, all constant indices.

Despite enabled resource tracking, a direct GetPageFaultInfo query returned success and resourceInfoCount=0. Therefore no destroyed/live application allocation was identified at the fault address. This neither establishes a stale output image nor completely excludes an application resource problem. A compiler-generated spill/temporary-memory write is now a stronger hypothesis, but a driver defect has not been established.

Latest output binding: ID516:3, VkImage0x26b530000026b53, memory0x26b540000026b54, output view0x26b570000026b57. Previous frame used ID768:4. Same correct214x120x72 format/view/allocation dimensions as before.

Offline compilation experiments, none dispatched GPU shader work:

- Added NonWritable on the SRT only, then on both read-only buffers.
- Ran SPIRV-Tools performance passes; validated output shrank257,744 to198,400 bytes.
- Requested VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT.
- Disabled buffer robustness in the standalone test device.

All exposed the same driver statistics:128 registers,73,344-byte native binary, local-memory statistic0x1000000010,3,072 bytes shared,zero stack. These statistics do not prove byte-identical native code, and their binary size differs from the actual crash by128 bytes. The upper32 bits of the local-memory statistic are not interpreted as a real64GB allocation. No demonstrated improvement resulted, so none of these variants was incorporated into the emulator or offered as another game retry.

Tools: aftermath/disassemble_spirv.cpp, optimize_spirv.cpp, inspect_shader.cpp. Matching logs and df890.spvasm are preserved there. Existing emulator source and binary are unchanged since the preceding NVIDIA diagnostic build. No emulator is running now. No confirmed fix.


Fresh-save comparison prepared 2026-09-10. Not launched yet.
Executable and DLLs copied unchanged from E:\Emulation\PS5\KytyPS5-DeS-688d485-author.
Executable SHA256: fdb9062085b1f46d3b599127af1005345ccea9f01558c271836e6e8cd5525235
Same portable launcher configuration, same game dump, default CLI options.
_SaveData starts empty. Existing save files in all other installations were preserved.
The copied options profile is the main intended variable in this comparison.
No character/progress save was found in the existing installs; only the identical
562-byte SAVEDATA0OptionsProfile0/USR-DATA file, SHA256
f9505e7294741341389dccf9f1c682349362605433fc83eb3e1cf729bc77c802.
A successful fresh run would implicate state/settings but would need a repeat
with the old profile restored in a separate test to establish causality.
A repeated GPU fault would weaken the existing-profile hypothesis, but would not
by itself rule out bugs in creating the first save. Current console logs do not
contain enough save API tracing to correlate those calls with the GPU fault.

Fresh test folder: E:\Emulation\PS5\KytyPS5-DeS-688d485-fresh-save


Save investigation updated: see C:/Users/dudu/Projects/KytyPS5-DeS-1d6e23a/docs/investigations/savedata-investigation.md.


## 2026-09-10 console save export/test
# Demon's Souls console save export - 2026-09-10

Export succeeded using the official Save Mounter 2.0.0 payload on PS5 Pro firmware 12.20, through existing Payload Manager 0.5.1 (HTTP8084) and FTP2121. The underlying elfldr listens only locally, so the payload was installed/launched through Payload Manager. User reported the game saved/closed before mounting. No controller input was injected.

- Console: 192.168.15.42, local user1559a3b9, titlePPSA01342.
- Export: E:\Emulation\PS5\Saves\DemonsSouls-PPSA01342-20260910-102547
- All six original encrypted containers backed up before mounting (container-manifest.json).
- Decrypted contents of three character saves plus options exported; each file downloaded and then independently hashed from a second remote read, all matched.
- All mounts unmounted successfully; mounter received EXIT and stopped. FTP and Payload Manager remain running.
- Stock mounter mounts the original containers. Four mounted-container hashes changed after mounting/unmounting; two untouched backup-container hashes stayed identical. No save-file writes, CREATE commands, or replacement operations were sent. Pre-mount containers remain preserved locally. Do not describe container bytes as unchanged.
- Payload remains installed at /data/pldmgr/payloads/save-mounter/save-mounter-2.0.0.elf; SHA256 a662175583f4933a04ce10a7d6f7be9aa7f5de002d02944f91e7f0267b604eba. No autoload config edited.

Characters from actual decrypted sce_sys/param.sfo:
- SAVEDATA0PlayerProfile0: Duds, level106 Knight, The Nexus,40:14:15; USR-DATA89326 bytes.
- SAVEDATA0PlayerProfile1: Duds, level1 Royalty, The Nexus,21:18:44; USR-DATA26650 bytes.
- SAVEDATA0PlayerProfile2: Moises, level4 Knight, Boletarian Palace,21:43; USR-DATA17437 bytes.
- Options USR-DATA571 bytes.

A checked ZIP of all decrypted contents and manifests is adjacent to this directory, suffixed -decrypted.zip. Original export is separate from emulator copies.

New isolated install: E:/Emulation/PS5/KytyPS5-DeS-688d485-console-save
Uses unchanged author688d485 binary SHA256 fdb9062085b1f46d3b599127af1005345ccea9f01558c271836e6e8cd5525235 and byte-verified copies of all four decrypted save directories under _SaveData/PPSA01342. No save-metadata PR434 or emulator code changes applied. Its launch script is Play Demon's Souls - console saves.cmd.

Launched PID13252 at2026-09-10T10:29:10-03:00. Diagnostics: _Diagnostics/first-run. Run state: C:/Users/dudu/Projects/KytyPS5-DeS-1d6e23a/_Build/console-save-run.json.
User instructed to select Load Game; no recognition/gameplay result confirmed yet. If metadata is blank or saves not recognized, review PR434 implementation using the exported param.sfo as ground truth. Avoid altering originals or guessing file contents.

Tool bug found: SEARCH output interpolates save DETAIL containing embedded newline, breaking the stock line-delimited protocol. Initial search last row was truncated. Direct mounting used names confirmed by FTP; parse local param.sfo for complete metadata. A new TCP connection was used after the malformed search response, so no leftover data entered subsequent commands.


### Imported-save initial run and isolated Nexus comparison
First all-save import runPID13252 ended at VS21 PS31 CS233 with a guest-code access violation (not GPU device loss): eboot.bin+0xd8c1f5, write to0x50. Disassembly confirms mov[rcx+0x50],rdx with a null backward pointer in a linked-list operation. No causal tie to the save parser/metadata established. Exact stage question pending; user input before crash unconfirmed. Disassembly: _Build/console-save-crash-disassembly.txt. Synthetic section headers were added only to a local _Build/eboot-disassembly-copy.elf for LLVM disassembly, original eboot untouched.

Prepared a second isolated install E:/Emulation/PS5/KytyPS5-DeS-688d485-nexus-save: unchanged author binary, only consolePlayerProfile0 (Duds,106,Knight,Nexus) plus the original emulator562-byte options profile. First experiment combined console settings with all three character saves; this one removes those extra variables. No save metadata fix applied.
Launched PID21792 at2026-09-10T10:33:42-03:00. Logs _Diagnostics/first-run, run record _Build/nexus-save-run.json. Awaiting manual title/menu selection and outcome. No input injected.


### 2026-09-10 Nexus-only result
PID21792 crashed with the same eboot.bin+0xd8c1f5 write to0x50 as the all-save import; VS21 PS31 CS228. It did not reach confirmed gameplay. Restoring the old emulator options and including only one Nexus character did not remove this CPU fault. No evidence yet establishes save corruption or links this CPU exception to the earlier GPU fault.
Now running unchanged author688 binary with its existing --printf-direction File --printf-output-file logging enabled. PID19504, started10:40:53-03:00, diagnostics E:/Emulation/PS5/KytyPS5-DeS-688d485-nexus-save/_Diagnostics/save-api-trace. Goal: identify SaveData calls and USR-DATA reads before failure without first applying a speculative metadata fix. No source modifications this turn.


### Save API trace outcome (PID19504)
The unchanged author688 build again crashed at eboot.bin+0xd8c1f5 while writing to0x50. No process remains running. The API trace establishes that Kyty opened the imported SAVEDATA0PlayerProfile0/USR-DATA successfully and read it completely TWICE:65536+23790=89326bytes per pass. Each pass closed the file and unmounted /savedata0. Options562bytes also read successfully. Character-file SHA256 remains39c73046683515d42b8302fd8d2491193684e676f25d0cf3652e27bc7abe363e.
Evidence: _Diagnostics/save-api-trace/api.log, save-api-extract.txt, outcome.json in the688d485-nexus-save install. Read sequences start near lines652774 and1034441; fatal near1137700. This proves enumeration/mount/file reading works, not successful deserialization or gameplay. It does not prove the original GPU crash was caused by save handling. No metadata fix was applied; the zero-metadata implementation did not prevent this character file being read. Imported saves alone are not a successful workaround. Existing console save export remains reusable for further debugging.


# Demon's Souls CPU fault investigation - 2026-09-10

Imported Nexus save: Duds,106 Knight,89,326-byte USR-DATA, read successfully twice by the unmodified author688d485 build. Repeated crash writes to0x50 at eboot.bin+0xd8c1f5. Full save read does not establish successful deserialization/gameplay.

## Function identification
Faulting function begins at guest RVA0xd8c070. At0xd8c1f5 it executes mov[rcx+0x50],rdx after loading rcx from[rax+0x58]. The source-linked strings identify this area as TouchManager: constructor at0xd8c460 references TouchManager::Think (RVA0x2482223) and TouchManager::DebugRender (RVA0x2522941); faulting function references Touch inconsistent! (0x24cd6d2) and Num Touched:%d;Size:%d (0x24c3ecf). These are obtained from the original game's ELF load segments, not guessed symbols. Linked-list state is invalid; origin of invalid state is unknown. Do not treat the save file as demonstrated corrupt, and do not claim a proven collision-system bug until further evidence.
Disassembly saved in fault-function-disassembly.txt; original eboot unchanged. LLVM uses a local copy with synthetic section headers only.

## Exact-author external debugger attempt
capture_guest_fault.py launches the unchanged author binary via the Windows debug API, passes ordinary first-chance exceptions through, and is designed to snapshot only the known fault. No game inputs or memory/register writes.
PID20000, diagnostics E:/Emulation/PS5/KytyPS5-DeS-688d485-nexus-save/_Diagnostics/cpu-fault-capture.
Attempt timed out after300s, ~748,800 handled access violations, no target capture. It detached successfully. The process was then closed to free the GPU for a diagnostic build; CloseMainWindow followed by forced termination if not exited within3s. This was our own capture process. Do not attribute its termination to the target crash.

## Local fatal-only diagnostic build
Added opt-in KYTY_DEBUG_GUEST_FAULT=1 reporting in src/loader/runtimeLinker.cpp. It runs only after existing illegal-instruction/GPU fault handlers decline a fault. Prints full integer registers, bounded guest frame walk, and small512-byte memory snapshots ofrax/rbx/rdi/rsp. Windows ReadProcessMemory prevents diagnostic reads raising recursive access violations. No guest state writes. Default behavior unchanged when variable absent.
New build compiles successfully; git diff --check passes. No semantic/game-logic change or save-metadata PR434 applied.
Patch: guest-fault-context.patch.
Install E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-cpu-fault, executable kyty_emulator-guest-fault.exe, SHA25617b5c3e47ee651684d79b759c87f3033b5f560b53ae6fe01a09394239aadec11.
This is the existing locally modified1d compatibility branch with earlier shader/diagnostic experiments. It is NOT identical to author688 and must reproduce the same CPU fault before comparing its context.
Copied Nexus save plus original emulator562-byte options into the separate install; all original exported saves preserved.
Launched PID32944 at11:00:21-03:00, _Diagnostics/first-run, run recordguest-fault-run.json. No input injected. Awaiting outcome.


## First local capture succeeded
PID32944 reproduced the exact same eboot.bin+0xd8c1f5 CPU fault as author688, with rcx=rdx=0. Stack returns:844885,af4895,821e4e,820f1b,8182a6 (all eboot RVAs). Guest thread18. VS29 PS54 CS270; gameplay still not visually confirmed.
RAX=0x109f998870 (start node), RBX=0x109f998890 (end marker referencing the start node), R13 sentinel0x236bfe620, RDI list base0x236bfe5b8. Node start coordinate low32 bits0xc1decccd (-27.85), end0x40099998 (~2.4). Start node temporary active next/prev at+0x50/+0x58 arebothnull.
The TouchManager job wrapper at0xd8c890 calls helper0xd8c070 on offsets0,0x98,0x130,0x1c8; the failing fourth call is a tail call, explaining its absence from the captured frame walk. Constructor/live job references corroborate TouchManager::Think. Thread18's call stack is a scheduled engine job, not the save I/O call stack.
The object memory contains old-looking 16-bit patterns in otherwise uninitialized-looking bytes. These may be allocator leftovers/padding, not evidence of GPU overwrite; do not assert memory clobber without proving a live field was damaged.

Added a second optional diagnostic, KYTY_DEBUG_DES_TOUCH_LIST=1, specifically gated to eboot.bin+0xd8c1f5. It snapshots up to1024 list nodes using ReadProcessMemory starting at sentinel+0x18, then prints key/pair/prev/next/active links. No writes. The next question is whether an end marker precedes its own start, or whether active-list membership was lost.
Built successfully. New executable kyty_emulator-touch-list.exe SHA256cfffc33a1ee09b180d95610ecedef37fc798fd88a3fba745133f81da0e2ff6ff in the same CPU-fault install. Launched PID28980 at11:06:28-03:00; _Diagnostics/touch-list. No user input injected. A pending async prompt asks the user to select Duds if needed. No response yet.

## Touch-list capture analyzed; execution trace launched (11:27)
PID28980 exited at the same guest instruction, eboot.bin+0xd8c1f5, rcx=rdx=0, thread10. Full list contains522 nodes/261 paired markers; sorted, closed and consistent prev/next links, with every end following its start. Only one start has active_prev=0: faulting start0x10b7c228b8 at index37/key-40, paired end at index495/key40. Analysis: CPU-fault install/_Diagnostics/touch-list/touch-list-analysis.json. This suggests skipped insertion/reset during sweep, but does not prove a race or corrupted save. Padding patterns are not evidence of a GPU overwrite.
Kernel review: KernelGetCurrentCpu always returns0, but it is absent from the full save API trace. Pthread affinity only stores masks; observed Setaffinity calls mostly surround Bink/audio thread creation. Event flags, semaphores and condition waits have real blocking implementations. No proven scheduler defect identified; no threading semantics changed.
Added temporary Windows-only desTouchTrace.h and runtimeLinker integration, gated by KYTY_DEBUG_DES_TOUCH_TRACE=1, exact PPSA01342/01.005.000 and expected instruction bytes. It instruments two in-memory MOV loads atd8c0f7 andd8c1e4 with UD2; handler performs the original load once, changes only destination/RIP, and records a bounded per-thread initial list plus visits. No game-file/save writes. Captures are not atomic across guest threads, and probes alter timing. This is diagnosis, not a fix. No input injected.
First link failed on LLVM inline TLS COMDAT bug. Internal-linkage TLS resolved the build error; retry build succeeded. New exe kyty_emulator-touch-trace.exe SHA2569ef5b118eade26445f686a0dff7728e4d26c39ce636ba190a2c7e2910effd5a6. PID10900 launched11:27:11-03:00 in the CPU-fault install; diagnostics _Diagnostics/touch-trace; run record _Build/touch-trace-run.json. Both probes installed successfully. Async prompt asks user to select Duds106 Nexus. No result yet.
Analysis utility _Build/analyze_touch_trace.py compares initial, actual visits and fatal snapshots. Rerun launcher Play Demon's Souls - touch trace.cmd. Previous binaries/captures and exported saves preserved.

## User authorized automated menu input; traversal capture result (11:35)
The user explicitly authorized holding the controller Cross/X button for about4s during cinematics and pressing it about3 times afterward. This supersedes the previous no-input restriction for advancing these test runs. Kyty default keyboard Cross is J (VK0x4A), confirmed in hostInput.cpp. Use targeted window input, inspect screens, and avoid destructive save/menu actions.
PID10900 exited with the same CPU fault. Trace thread18/sequence2224: initial520nodes, visited495, final520nodes. Target0x110375abf8 was absent from the initial snapshot and never visited as a start; its end0x110375ac18 was visited atindex494. Preceding node0x375ab5b70 next pointer changed from0x375b020a0 initially to the target end during traversal, then back before the fatal list snapshot. The object was inserted after traversal had begun and removed before the final snapshot. This establishes list mutation during the update; originating thread/caller remains to be captured. No save-corruption conclusion or emulator fix yet. Captures are not atomic and probes change timing.
Added insert/remove probes atd8b697/d8b7ce (MOV loads, expected bytes checked), recording bounded4096 events for high-address objects plus16 guest caller frames. New exe kyty_emulator-touch-mutation.exe SHA256e4c3b2a42f37e2078b29856ce6c3976ae10794896d8400794f52590b06830f6e. Build passed. PID23288 launched11:35:34-03:00 in the CPU-fault install, _Diagnostics/touch-mutation. Four probes installed successfully. Awaiting autonomous menu navigation and crash outcome.

## Cross-thread mutation confirmed; serialization experiment (11:42)
Autonomous navigation worked using targeted J (Cross) messages: held4.5s once the opening cinematic was visible; pressed J on Press Any Button, then Continue, then Continue Offline, inspecting screenshots at each stage. PID23288 reproduced the CPU fault without user help.
Mutation capture: target0x1070e81eb0 inserted by guest thread18 throughd597c4,8c0cfa,8bf2b8,8bf252,8befc1,8bbf45,14553da,5f7e66,5f717f,610ad4,6241a9,415c8b,416e1f,417031,4182ec,41cf2d. The crashing traversal was thread11, sequence1176, list0x2f7a7fb18/sentinel0x2f7a7fb80. Initial526nodes, final530nodes; target start absent initially and not visited; target end visited index246. This confirms insertion by another worker during the failing sweep. Which emulated behavior permits this scheduling overlap is unresolved.
Built opt-in KYTY_DEBUG_DES_TOUCH_SERIALIZE=1 experiment: recursive host mutex around sweep and insert/remove/update functions, acquired before guest mutation spinlocks; update-to-insert tailcall explicitly releases then reacquires. Exit probes reproduce the guest atomic XCHG before releasing host mutex. This is a game-specific timing workaround, not a demonstrated general scheduler fix. All12 signatures checked and only in-memory code patched. Can still expose other crashes, performance costs or callback dependencies. Original game files and saves preserved.
PID25904 launched11:42:37-03:00, kyty_emulator-touch-serialize.exe SHA2566594aa1866fae3a4ef5cb831687ce346006157db2f61fa432f2a6188b248721f; diagnostics _Diagnostics/touch-serialize. Build passed and all12 probes installed. Awaiting autonomous navigation and result.

## Success: Nexus reached, session left running; lighter build staged (11:52)
PID25904 reached in-game Nexus. Visual proof at frame4335 (character/HUD/world), then frame4773 and5006; continued animation, about4FPS. At11:52:09 it remained running without the CPU fault. No post-menu gameplay inputs were injected. The observed session remained in-game for several minutes; broader stability and the previous character-creation GPU-fault path are untested. Screenshots _Diagnostics/touch-serialize/nexus-first-gameplay.png and nexus-still-running.png.
Game wrote character USR-DATA at11:49:03:89326bytes, SHA256d6251cef279fbc9396c12e246c3ac83d39a7643fea875e78bed4f4d155157a13. Original console export unchanged/preserved. Reloading the newly written save not yet tested.
Tested launcher: CPU-fault install/Play Demon's Souls - collision workaround.cmd, uses kyty_emulator-touch-serialize.exe (6594aa...721f), detailed capture enabled. Keep this working session open.
Lighter variant built/staged, not launched: kyty_emulator-touch-workaround.exe SHA256f7509cb656a6ed5e4f020a9814d292a9e9b9c833267658d8ff48dfc6b3366a16. It keeps9serialization probes and omits per-node/insert/remove diagnostic captures; KYTY_DEBUG_DES_TOUCH_FULL_CAPTURE=1 restores detailed captures. Separate Play Demon's Souls - lighter workaround.cmd. No performance result for this variant yet. Source/runtime behavior remains opt-in and exact-version/signature gated. Build succeeded and git diff --check passed.
Concise report docs/investigations/demons-souls-touch-race-report.md now includes results and limits. This is a demonstrated game-specific workaround for one loading path, not a proven general scheduler fix. User authorized automated menu navigation; no need to ask again to reproduce these tests.

## Single emulator folder requested and implemented (12:09)
User confirmed Nexus gameplay and requested a single folder instead of separate installations. Canonical emulator install is now E:\Emulation\PS5\KytyPS5. Main entry point Play Demon's Souls.cmd; confirmed binary renamed to kyty_emulator.exe, SHA2566594aa1866fae3a4ef5cb831687ce346006157db2f61fa432f2a6188b248721f. All eight older sibling installs are under its _Archive; prior CPU diagnostic binaries/scripts under _Archive\Diagnostic-builds. The staged lighter executable/launcher remains in the same main folder. Only Games,KytyPS5,Saves,Tools remain at E:\Emulation\PS5 top level. No game dump or original PS5 export moved.
Before relocating, used Options->Settings->Exit Game->Save and Exit Game. Latest character save written, then CloseMainWindow closed PID25904 normally (no forced termination). Window helper now sets the extended-key flag for arrow/navigation keys, necessary for SDL to interpret Down correctly. Runtime menu input was used solely to save/exit and later restore the session.
Moved directories with validated absolute paths under E:\Emulation\PS5, rejected junctions/existing destinations, no deletion. Confirmed executable, staged lighter executable and both active USR-DATA hashes matched before/after. Migration mapping with pre-move hashes is FOLDER-MIGRATION.json and source _Build/folder-consolidation.json. Historical log paths remain original; resolve through that mapping.
Updated active-source build-compat.ps1 and build-demons-souls.ps1 Install phases to use install-single-folder.ps1: canonical destination, backup prior binaries under _Archive\Builds, refuse replacement while emulator runs, update CURRENT-BUILD.json. Syntax checked, not invoked (would replace confirmed build). Main README/provenance updated. CWD official source _Build/active-demons-souls.json points to active source and install. Keep using this ONE runtime folder for all future tests/build installs.
Relaunched confirmed binary from new folder, PID39772 at12:08:57, diagnostics _Diagnostics\consolidated-folder, run record active source _Build/active-install.json. Startup/probes succeeded; restoring saved character through menus remains in progress. Source checkouts on C: were not moved because build trees contain absolute source/dependency paths; this consolidation covers runtime installations.

Consolidation verification complete: PID39772 loaded the saved character back into the Nexus from the canonical folder. Visually verified at frame4053, about4.2FPS, character/HUD/world and688souls visible. Screenshot E:\Emulation\PS5\KytyPS5\_Diagnostics\consolidated-folder\nexus-after-move.png. Save-and-exit followed by reload now succeeds on this path. Process left running; lighter build remains untested. Use E:\Emulation\PS5\KytyPS5\Play Demon's Souls.cmd for future launches and the single-folder installer for future builds.
