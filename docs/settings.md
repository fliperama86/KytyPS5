# Settings

Every runtime gate this fork adds or relies on. Game-specific and investigation-only
behaviour is off unless something on this page turns it on, so `main` stays safe for
other titles. `kyty_emulator.exe --help` prints the full option list, including the
window, user, and path options omitted here.

## Command-line flags

| Flag | Default | What it does | Exists for |
| --- | --- | --- | --- |
| `--shader-lds-waitcnt-barrier <true\|false>` | `false` | Emits a subgroup-scope `OpControlBarrier` for every `S_WAITCNT` in a compute shader that uses LDS. | Demon's Souls, whose waves exchange LDS data after `S_WAITCNT` without an `S_BARRIER`. It costs frame rate and is not needed by other titles. |
| `--shader-storage-bounds-check <true\|false>` | `true` | Wraps each storage-image write in an `OpImageQuerySize` compare and branch; `false` emits the plain `OpImageWrite`. | The Demon's Souls GPU-crash investigation (out-of-bounds `IMAGE_STORE` losing the device). On by default because an unbounded write can take the device down on any game. |
| `--graphics-debug-dump <true\|false>` | `false` | Names Vulkan objects, dumps pipelines and shaders, and enables `VK_EXT_device_fault` plus NV diagnostic checkpoints. | Device-loss and GPU-fault investigations. |
| `--thread-affinity <auto\|none>` | `auto` | `auto` enumerates the host L3 caches at startup and, when there are at least two of unequal size in one processor group, pins the render and presentation threads to the largest cache's logical processors and every guest thread to the rest, logging one line with the sizes and the derived masks. A uniform host, a single L3 cache, more than one processor group and every non-Windows platform derive nothing. `none` derives nothing anywhere. | Multi-CCD hosts with asymmetric L3, such as the Ryzen 9 9950X3D; see the CCD affinity section in [demons-souls-performance.md](demons-souls-performance.md). |
| `--profiler-direction <None\|Network>` | `None` | Starts the Tracy profiler server. `Play Demon's Souls.cmd profile` passes it. | Frame-rate and render-thread zone work. |
| `--vulkan-validation <true\|false>` | `false` | Vulkan validation layers. | General debugging. |
| `--gpu-assisted-validation <true\|false>` | `false` | GPU-assisted validation; implies `--vulkan-validation`. Very slow. | Finding out-of-bounds shader accesses. |
| `--spirv-debug-printf <true\|false>` | `false` | Routes `debugPrintfEXT` from generated SPIR-V to the console. | Shader recompiler debugging. |
| `--shader-validation <true\|false>` | `false` | Runs the SPIR-V validator on every generated module. | Shader recompiler debugging. |
| `--shader-optimization-type <None\|Size\|Performance>` | `None` | SPIR-V optimizer pass level. | Shader experiments. |
| `--shader-log-direction <Silent\|Console\|File>` | `Silent` | Dumps decoded RDNA2, IR, and SPIR-V (folder from `--shader-log-folder`). | Shader recompiler debugging. |
| `--command-buffer-dump <true\|false>` | `false` | Writes guest command buffers (folder from `--command-buffer-dump-folder`). | GPU command-stream investigations. |
| `--readback-linear-images <true\|false>` | `false` | Reads writable linear images back to guest memory on submit. | Titles that read GPU-written linear images on the CPU. |
| `--playgo-hack` | off | Uses the bundled PlayGo stub fallback when the title's chunks do not load. | Games that stall in PlayGo. |
| `--redzone` (Windows) | off | Protects the guest SysV red zone across host callbacks. | Guest-fault investigations. |
| `--rd` | off | Loads the RenderDoc capture layer. | Frame captures. |

## Environment variables

All are read once, on the code path named, and all are off when unset except the three affinity
masks, which fall back to whatever `--thread-affinity` derived. The root `Play Demon's Souls.cmd`
sets the `KYTY_DEBUG_DES_TOUCH_*` group.

| Variable | Default | What it does | Exists for |
| --- | --- | --- | --- |
| `KYTY_DEBUG_DES_TOUCH_TRACE` | unset | `1` installs the guest `TouchManager` probes. Requires PPSA01342, `01.005.000`, `eboot.bin`; any other title logs one line and leaves the hooks uninstalled. A probe-byte mismatch on a matching title is still fatal. | The Demon's Souls collision-list race workaround; without it the game does not reach gameplay. |
| `KYTY_DEBUG_DES_TOUCH_SERIALIZE` | unset | `1` serializes `TouchManager` traversal and mutation behind a host recursive mutex. | Same workaround, lighter mode. |
| `KYTY_DEBUG_DES_TOUCH_FULL_CAPTURE` | on unless serializing | `1` records every list mutation. Implied when `KYTY_DEBUG_DES_TOUCH_SERIALIZE` is not set. | Collision-list race report evidence (`tools/analyze_touch_trace.py`). |
| `KYTY_DEBUG_DES_TOUCH_LIST` | unset | `1` dumps a bounded snapshot of the guest collision list on a fault at `eboot.bin+0xd8c1f5`. | The same race investigation. |
| `KYTY_DEBUG_GUEST_FAULT` | unset | `1` prints guest registers and a code window for an unhandled guest exception. | CPU-fault investigation. |
| `KYTY_DEBUG_NV_FAULT_DETAILS` | unset | `1` adds `VK_NV_device_diagnostics_config`. Only has an effect with `--graphics-debug-dump true`. | NVIDIA fault-detail capture. |
| `KYTY_DEBUG_RESOURCE_TRACE` | unset | Set to a file path: records the last 128 compute dispatches with their bindings and writes them there on device loss. | GPU crash report. |
| `KYTY_DEBUG_GPU_FAULT_FILE` | unset | Set to a file path: saves the vendor fault binary from `VK_EXT_device_fault`. Needs `--graphics-debug-dump true`. | GPU crash report. |
| `KYTY_DEBUG_SERIALIZE_DISPATCH` | unset | Set to anything: flushes and waits after every compute dispatch. Very slow; isolates which dispatch faults. | GPU crash report. |
| `KYTY_GPU_THREAD_AFFINITY` (Windows) | derived | Hexadecimal CPU mask pinned onto the render thread `Thread_Gpu` with `SetThreadAffinityMask`. Logs one line with the thread id and mask. Overrides the mask `--thread-affinity auto` derived for that thread; unset, empty or unparseable keeps the derived mask. | Scheduling experiments on multi-CCD hosts; see the CCD affinity section in [demons-souls-performance.md](demons-souls-performance.md). |
| `KYTY_PRESENT_THREAD_AFFINITY` (Windows) | derived | The same, for the video-out presentation thread. | Same. |
| `KYTY_GUEST_THREAD_AFFINITY` (Windows) | derived | The same, for the guest main thread and every thread the guest creates through `pthread_create`. | Same. |
| `KYTY_BORDERLESS` (macOS) | unset | Set to anything: creates a borderless SDL window, avoiding macOS 26 title-bar exceptions under Rosetta. | Upstream macOS workaround. |
