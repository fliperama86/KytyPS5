> Historical investigation notes from September 9?10, 2026. Later entries may supersede earlier findings. See the [current workaround](../demons-souls-workaround.md) and [performance results](../demons-souls-performance.md). Artifact paths identify local captures and may predate runtime-folder consolidation.

# Demon?s Souls: failure entering gameplay on RTX 5090

Local draft, 2026-09-10. Not posted externally.

Resolved on 2026-09-13: see [the tutorial-load bisect](tutorial-gpu-crash-2026-09-13.md).

## Reproduction and baseline

PPSA01342, version 01.005.000, on Windows build 26200, RTX 5090 (driver 616.64), Ryzen 9 9950X3D, 64 GB RAM. User advances through character creation and cinematics; the emulator fails before gameplay.

The unmodified author-provided 688d485 executable also failed locally. Its console ended at VS46 / PS76 / CS260 with a failed semaphore wait, masterSemaphore.cpp:53. That binary has no detailed fault capture, so it does not establish the same underlying shader fault as the instrumented build.

Author binary source: https://github.com/KytyPS5/KytyPS5/pull/500#issuecomment-5603171558
EXE SHA256: fdb9062085b1f46d3b599127af1005345ccea9f01558c271836e6e8cd5525235
Embedded commit: 688d485fd48a8fc66f54731f51a3c0b2484afd2c
The embedded commit was unavailable through GitHub when checked; equivalence to branch head 6c6e3e7 is unverified.

Another tester explicitly reported 01.005.000, then gameplay success with the author package. Thus an update-version incompatibility alone is not established:
https://github.com/KytyPS5/KytyPS5/pull/500#issuecomment-5601930489
https://github.com/KytyPS5/KytyPS5/pull/500#issuecomment-5603576248

## Instrumented GPU fault

Separate experimental source: 1d6e23a plus three upstream shader fixes and local diagnostics/workarounds. The exact patch is saved alongside this report. These measurements are not from the unchanged author executable.

NVIDIA Aftermath SDK 2026.3 decoded the saved Vulkan vendor binary successfully:

- Device state: Error_DMA_PageFault; engine reset true, adapter reset false.
- Write to unmapped GPU virtual address 0x7aced0000.
- Faulted compute warp at native instruction offset 0xd00.
- Aftermath shader hash 0xeed92266c3b35f03 matches saved SPIR-V for Kyty shader df890e8a1a32c65a, verified using GetShaderHashSpirv.
- No native instruction-to-SPIR-V mapping is available. Missing debug identifier: eed92266c3b35f03-305a7cf0470fbff1.nvdbg.
- Serializing compute submissions still reproduced the df890 failure in the immediate wait.
- Explicit storage-image coordinate guards also failed to prevent it.
- Core Vulkan validation did not report an error before device loss. This does not prove bindings/lifetimes are correct.

At the last traced dispatch: frame10987, tick791671, groups27x15x72. The storage image was R16G16B16A16Sfloat, 214x120, 72 layers, 1 mip, General layout, an actual 2DArray view spanning layer0/72. VMA allocation 0xf30000 equals the Vulkan memory requirement. Sampled image binding 3 aliases the output.

The output backing changed from image ID473:6 on the preceding frame to119:719 on the fault frame, after repeated replacements over recent frames; other input image IDs stayed unchanged. This is a lead for lifetime/overlap investigation, not proof of a stale descriptor or premature destruction. CPU trace metadata alone does not identify the driver allocation owning the bad address.

## Offline shader inspection

A small standalone Vulkan tool compiled and bound the saved shader without dispatching it. It did not reproduce a crash and cannot validate runtime resource access. The driver exposed statistics but zero internal representations and no Aftermath debug callback. Its native binary size differed from the crashed shader by128 bytes, so its code offsets cannot safely be equated.

## Next diagnostic

A separate executable enables NVIDIA device diagnostics resource tracking, shader debug info, and shader error reporting, in addition to the existing bounded CPU binding trace and saved vendor dump. The environment variable KYTY_DEBUG_NV_FAULT_DETAILS must equal1, graphics debug dumping must be enabled, and the device must support the extension/feature. Normal launches do not enable it.

This is a diagnostic, not a fix. Shader debug/error reporting may alter compilation or expose errors earlier. The next dump may identify a live/recently destroyed allocation at the fault address, or include instruction mapping. Whether the driver supplies those fields remains to be tested.

## Local evidence

- Author console: E:/Emulation/PS5/KytyPS5-DeS-688d485-author/_Diagnostics/first-run/console.log
- Detailed capture: E:/Emulation/PS5/KytyPS5-DeS-1d6e23a-compat/_Diagnostics/resource-trace/
- Decoded driver report: gpu-fault.json in that capture directory.
- Decoder and standalone tool: C:/Users/dudu/Projects/KytyPS5-DeS/_Build/aftermath/
- Current source patch: _Build/demons-souls-current-experimental.patch

No confirmed fix, driver-specific root cause, or damaged-dump diagnosis has been established.
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
