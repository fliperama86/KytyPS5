#pragma once

// Frame capture format shared by the capture side (src/graphics/replay/frameCapture.cpp) and the
// replay side (src/graphics/replay/frameReplay.cpp). See docs/frame-replay.md.
//
// A capture is a directory:
//
//   manifest.json     format_version, title_id, commit, frame, width, height, counts, gaps
//   ranges.bin        RangeRecord[]            every mapped guest range at capture time
//   memory.bin        PageRecord[]             every non-zero 16 KiB page of committed ranges
//   dirty-pages.bin   uint64_t[]               pages the CPU modified during the captured frames
//   dirty-events.bin  DirtyEventRecord[]       every CPU-dirty mark of the frames, in arrival
//                                              order, keyed to the GPU thread's progress (v3)
//   churn-events.bin  ChurnEventRecord[]       every other BDA-generation bump of the frames:
//                                              buffer registrations and retirements, guest map
//                                              and unmap calls; diagnostics only (v4)
//   prepare-events.bin PrepareEventRecord[]    every GpuResourceManager::PrepareBda of the
//                                              frames and whether it scanned: the ground truth
//                                              a replay has to reproduce (v5)
//   submissions.bin   SubmissionRecord stream  the captured frames, in processing-start order
//   registers.bin     RegisterFileRecord stream  command-processor register files at frame start
//   videoout.bin      VideoOutRecord stream    buffer registrations, one per attribute group
//   prt.bin           PrtApertureRecord[]      partially-resident-texture apertures (v2)
//   shaders.bin       ShaderRecord[]           the AGC shader map (v2)
//   frame.png         the presented frame (may be absent; then frame.raw + manifest width/height)
//
// All integers little-endian, all records packed, no padding. Streams are read until end of file.

#include <cstddef>
#include <cstdint>

namespace Libs::Graphics::Replay {

// Version 2 adds prt.bin and shaders.bin. A version 1 capture still replays: the apertures
// and the shader map are then recovered or reported missing, see docs/frame-replay.md.
//
// Version 3 adds dirty-events.bin, the *timing* of the frame's CPU writes. dirty-pages.bin stays
// for older readers and is what a v3 replay falls back to when the event stream is absent.
//
// Version 4 adds churn-events.bin, records several consecutive frames instead of one (see the
// Done submission record and the manifest's "frames"), and widens DirtyEventRecord by the frame
// index the mark belongs to. A version 3 stream of 24-byte dirty events still reads: the reader
// widens it with frame 0, which is the only frame such a capture has.
//
// Version 5 adds prepare-events.bin and doubles the resolution of the progress clock: it now ticks
// once when a draw or dispatch starts and once more after that draw's or dispatch's resource
// preparation stage, whether or not the stage needed a BDA preparation. A mark that arrives before
// a draw's PrepareBda and one that arrives after it therefore carry different progress values,
// which version 4 could not express. Progress values are not comparable across the two versions,
// so a v4 or older capture keeps the old replay path (the marker thread) and a v5 capture is
// applied inline on the GPU thread. See docs/frame-replay.md, phase E.
constexpr uint32_t kFormatVersion    = 5;
constexpr uint32_t kMinFormatVersion = 1;
constexpr uint64_t kPageSize         = 16384;

#pragma pack(push, 1)

// One mapped guest range as tracked by the kernel memory layer. `type` is the kernel's
// VirtualRangeType value; `prot` the guest protection bits. Replay maps committed ranges at the
// same address with the same protection and restores their pages; reserved ranges are reserved.
struct RangeRecord {
	uint64_t vaddr = 0;
	uint64_t size  = 0;
	uint32_t prot  = 0;
	uint32_t type  = 0;
	char     name[32] {};
};
static_assert(sizeof(RangeRecord) == 56);

// One 16 KiB page of guest memory. Zero pages are not written.
struct PageRecord {
	uint64_t vaddr = 0;
	uint8_t  data[kPageSize] {};
};
static_assert(sizeof(PageRecord) == 8 + kPageSize);

enum class SubmissionKind : uint32_t {
	Graphics        = 0, // command_dwords then constant_dwords follow the header
	Compute         = 1, // command_dwords follow the header; queue_id is the guest queue id
	FlipPreparation = 2, // no dwords; flip_request_id is set
	Done            = 3, // no dwords; marks GuestGpu::Done() and so the end of a captured frame.
	                     // v4: flip_request_id is that frame's GuestGpu frame number and queue_id
	                     // its progress count (draws plus dispatches), so a multi-frame capture
	                     // needs no per-frame arrays in the manifest to be replayed.
};

// One submission of the captured frame. The header is followed by `command_dwords` uint32_t and
// then `constant_dwords` uint32_t. Records appear in the order the GPU thread started processing
// them, so replaying them in file order on one thread satisfies every WAIT_REG_MEM as the
// original run did.
struct SubmissionRecord {
	uint32_t kind            = 0; // SubmissionKind
	uint32_t queue_id        = 0; // GuestGpu queue id as passed to SubmitCompute; 0 for graphics
	uint64_t flip_request_id = 0;
	uint32_t command_dwords  = 0;
	uint32_t constant_dwords = 0;
};
static_assert(sizeof(SubmissionRecord) == 24);

// One command processor's register file, raw bytes of the emulator's register structs. `size`
// bytes follow the header. queue_id 0 is the graphics processor, 1.. the compute processors, in
// GuestGpu::GetProcessor order; processors that were never created are not written.
//
// The `size` bytes are three blocks, back to back, each memcpy'd from the live struct:
// HW::Context, then HW::UserConfig, then HW::Shader (graphics/guest_gpu/hardwareContext.h). All
// three are trivially copyable and hold guest addresses only, so the same build restores them
// with a memcpy; `size` lets a replay reject a capture from a build whose structs changed.
struct RegisterFileRecord {
	uint32_t queue_id = 0;
	uint32_t size     = 0;
};
static_assert(sizeof(RegisterFileRecord) == 8);

// One video-out buffer registration (VideoOutRegisterBuffers2 call). `attribute_size` raw bytes of
// VideoOutBufferAttribute2 follow the header, then `count` pairs of uint64_t
// {data_address, metadata_address}.
struct VideoOutRecord {
	int32_t  handle         = 0;
	int32_t  set_index      = 0;
	int32_t  index_start    = 0;
	int32_t  count          = 0;
	int32_t  category       = 0;
	uint32_t attribute_size = 0;
};
static_assert(sizeof(VideoOutRecord) == 24);

// One partially-resident-texture aperture, as set by sceKernelSetPrtAperture. Only apertures
// with a non-zero size are written. An image whose resident head is a committed range and whose
// tail is a reserved hole is read through the aperture (Memory::TryReadPrtBacking), so a replay
// that does not restore these cannot read that image at all.
struct PrtApertureRecord {
	int32_t  index   = 0;
	uint64_t address = 0;
	uint64_t size    = 0;
};
static_assert(sizeof(PrtApertureRecord) == 20);

// One entry of the AGC shader map (graphics/shader/shader.cpp), which sceAgcCreateShader fills
// and every draw and dispatch resolves through. A replay runs no guest code, so it has to be
// restored; every field is a guest address or a plain number, and the structs they point at
// live in guest memory and come back with it.
struct ShaderRecord {
	uint64_t code_address        = 0; // ShaderMap key: the shader code base
	uint64_t user_data           = 0; // guest ShaderUserData*, may be 0
	uint64_t input_semantics     = 0; // guest ShaderSemantic*, may be 0
	uint32_t num_input_semantics = 0;
	uint32_t code_size_bytes     = 0;
	uint32_t scratch_size_dwords = 0;
	uint32_t type                = 0; // Prospero::ShaderBinaryType
};
static_assert(sizeof(ShaderRecord) == 40);

// One CPU-dirty mark of the captured frame (v3). The game dirties guest pages throughout the
// frame, interleaved with the GPU thread's draws, and the BDA scan runs once per generation bump
// (BufferCache::InvalidateBda, GpuResourceManager::PrepareBda), so *when* a mark arrives decides
// how many scans a frame pays. Replaying the whole set in one batch before the frame collapses
// hundreds of scans into a handful; see the phase C and phase D sections of docs/frame-replay.md.
//
// `progress` is GuestGpu::Progress() at the moment of the mark: a per-frame counter the GPU
// thread bumps when a draw or dispatch starts and again after its resource preparation stage
// (twice per draw and dispatch since version 5, once before it), and Done() resets. It is a clock that means
// the same thing in the game and in a replay, unlike wall time. `submission` is the index of the
// submission the GPU thread had started when the mark arrived, for reading the stream by hand;
// the replay ignores it. Records are in arrival order, so the stream is sorted by neither field.
struct DirtyEventRecord {
	uint32_t frame      = 0; // index of the frame inside the capture, 0 to frames - 1 (v4)
	uint32_t progress   = 0;
	uint32_t submission = 0;
	uint64_t vaddr      = 0;
	uint64_t size       = 0;
};
static_assert(sizeof(DirtyEventRecord) == 28);

// The version 3 layout of the record above, without the frame index. Only the reader knows about
// it: a v3 capture has exactly one frame, so every such record widens to frame 0.
struct DirtyEventRecordV3 {
	uint32_t progress   = 0;
	uint32_t submission = 0;
	uint64_t vaddr      = 0;
	uint64_t size       = 0;
};
static_assert(sizeof(DirtyEventRecordV3) == 24);

// The *other* things that bump the BDA generation, which is what decides how many BDA scans a
// frame pays (GpuResourceManager::PrepareBda). A CPU-dirty mark is one of them and has its own
// stream; this one records the rest, measured in the game and missing from a replay of a single
// frame: BufferCache::ChangeRegister on every buffer registration (InvalidateBda) and every
// retirement (BumpBdaGeneration), and GpuResourceManager::MapMemory / UnmapMemory, which the
// kernel calls on every guest map and unmap.
//
// These records are diagnostics: the replay does not consume them. They exist to answer whether
// the guest's buffer addresses repeat with a short period -- a ring the replay reproduces by
// looping several captured frames -- or march forward for ever, which no loop can reproduce.
// See the phase E section of docs/frame-replay.md.
enum class ChurnEventKind : uint32_t {
	BufferRegister = 0, // BufferCache::ChangeRegister<true>, vaddr/size are the buffer's
	BufferRetire   = 1, // BufferCache::ChangeRegister<false>
	Map            = 2, // GpuResourceManager::MapMemory, from the kernel's MapGpuRange
	Unmap          = 3, // GpuResourceManager::UnmapMemory, from the kernel's UnmapGpuRange
};

struct ChurnEventRecord {
	uint32_t frame    = 0;
	uint32_t progress = 0; // GuestGpu::Progress() when the event arrived
	uint32_t kind     = 0; // ChurnEventKind
	uint64_t vaddr    = 0;
	uint64_t size     = 0;
};
static_assert(sizeof(ChurnEventRecord) == 28);

// One GpuResourceManager::PrepareBda call of a captured frame (v5) -- the draw and dispatch
// preparations that pay for the BDA scan, and the ground truth a replay is measured against.
// `scanned` is 1 when the BDA generation had moved since the last call and the scan ran, which is
// exactly one entry of the `GpuResourceManager::SynchronizeBdaBuffers` Tracy zone. The rest
// describes what the scan had to do, so a replay that reaches the right *number* of scans can
// still be told apart from one that reaches the right *cost*.
struct PrepareEventRecord {
	uint32_t frame        = 0;
	uint32_t progress     = 0; // GuestGpu::Progress() when the preparation ran
	uint32_t scanned      = 0; // 0 or 1
	uint32_t dirty_ranges = 0; // ranges in the dirty set the scan took, 0 when it did not scan
	uint32_t synchronized = 0; // SynchronizeBuffersInRange calls the scan made
	uint32_t reserved     = 0;
	uint64_t dirty_bytes  = 0; // bytes those dirty ranges cover
};
static_assert(sizeof(PrepareEventRecord) == 32);

#pragma pack(pop)

// manifest.json keys, all at the top level, written by the capture side. The replay side needs
// only format_version and frame; everything else is for people and scripts.
//
//   "format_version": 4
//   "title_id": "PPSA01342"
//   "commit": "<git hash of the capturing build>"
//   "frame": <GuestGpu frame number of the first captured frame>
//   "width": <presented width>, "height": <presented height>
//   "ranges": <count>, "pages": <count>, "dirty_pages": <count>, "submissions": <count>
//   "prt_apertures": <count>, "shaders": <count>                                    (v2)
//   "dirty_events": <count>          records in dirty-events.bin                    (v3)
//   "progress_events": <count>       GuestGpu::Progress() summed over the captured
//                                    frames, that is draws plus dispatches          (v3)
//   "frames": <count>                consecutive frames in this capture             (v4)
//   "snapshot_frame": <number>       the frame whose end the memory, register,
//                                    video-out, aperture and shader snapshot is of:
//                                    the last of them                               (v4)
//   "frame_numbers": [<n>, ...]      their GuestGpu frame numbers                   (v4)
//   "dirty_events_per_frame": [...]  dirty-events.bin records per frame             (v4)
//   "progress_events_per_frame": []  draws plus dispatches per frame                (v4)
//   "churn_events": <count>          records in churn-events.bin                    (v4)
//   "prepare_events": <count>        records in prepare-events.bin                  (v5)
//   "prepare_scans": <count>         of those, the ones that scanned                (v5)
//   "prepares_per_frame": [...], "prepare_scans_per_frame": [...]                   (v5)
//   "churn_registers_per_frame": [], "churn_retires_per_frame": [],
//   "churn_maps_per_frame": [], "churn_unmaps_per_frame": []                        (v4)
//   "gaps": [ {"vaddr": <hex string>, "size": <hex string>, "reason": "<text>"}, ... ]
//           ranges the capture could not read back from the GPU (image-owned or unsupported)

} // namespace Libs::Graphics::Replay
