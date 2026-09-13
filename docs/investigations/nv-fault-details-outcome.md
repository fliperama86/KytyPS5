> Historical investigation notes from September 9?10, 2026. Later entries may supersede earlier findings. See the [current workaround](../demons-souls-workaround.md) and [performance results](../demons-souls-performance.md). Artifact paths identify local captures and may predate runtime-folder consolidation.

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
