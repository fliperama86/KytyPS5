> Historical investigation notes from September 9?10, 2026. Later entries may supersede earlier findings. See the [current workaround](../demons-souls-workaround.md) and [performance results](../demons-souls-performance.md). Artifact paths identify local captures and may predate runtime-folder consolidation.

# Demon?s Souls save investigation ? 2026-09-10

User clarified that each emulator run requires a new character and asked to find a save online or create one. The objective is a valid PS5 character save for loading past character creation, not just resetting the shared options file.

## Current findings

All inspected installs contain only the identical562-byte SAVEDATA0OptionsProfile0/USR-DATA. No character/progress data exists there. Old verbose6c log (20260909-vulkan/kyty.log) records reading that options file at lines1987772?1987793, plus earlier directory searches. It contains no other SAVEDATA0 or savedata mount/file write match. This old run is not a full save trace from the latest build, so it does not conclusively establish when the latest run attempted to save.

The game?s own logs in the dump?s logs/ directory have only initialization messages; no save-specific error was found. Debug menu concommand errors about cpuMarkerLevel/profileLoading_Print are also present at startup and are not demonstrated save failures.

Local libSaveData.cpp discards save metadata in SaveDataSetParam, returns zero metadata in GetParam and directory search, and keeps the save-memory API in a process-local vector. The metadata defect is documented by an open upstream PR tested with GTA V, not Demon?s Souls:
https://github.com/KytyPS5/KytyPS5/pull/434
The patch has NOT been applied. Missing metadata can hide an existing valid save, but cannot by itself account for the absence of the character file. Its relevance to the renderer fault remains unproven.

Evidence saved in _Build:
- savedata-verbose-extract.txt (PowerShell UTF-16 output)
- savedata-github-search.json (UTF-8)
- pr434-save-metadata.json and pr434-save-metadata-files.json
- save-related-game-strings.txt

Game executable strings identify Bluepoint save states and SAVEDATA0%s/USR-DATA-style storage. cp11_playerSave is explicitly a player-loadout export command; it has not been established as a full character-save generator. No stock character-save fixture was found among the game files. Do not manufacture arbitrary bytes or rename a PS3 save and claim compatibility.

## Online search

No verified freely downloadable compatible PS5 Demon?s Souls character save was found. Nexus/RPCS3 saves found are for PS3. A repository titled PS5 saves contained only a README. Commercial starter-save advertisements exist but no purchase, account login, or external request was made. No compatibility with PPSA01342/01.005.000 was established for them.

## Console export route prepared

Official Playstation5 Save Mounter2.0.0 supports PS5 game saves and claims support for all jailbreakable firmware:
https://github.com/n0llptr/Playstation-5-Save-Mounter
https://github.com/n0llptr/Playstation-5-Save-Mounter/releases/tag/v2.0.0

Downloaded release to E:/Emulation/PS5/Tools/SaveMounter-2.0.0/
Archive SHA25683a17e5ca7e286340a9b42eb7700e1897d1638911adb1de8789266980b047c79.
Executable, DLLs and mounter.elf extracted under app/. Matching tagged README,
MounterClient.cs, ElfldrClient.cs, Main.cs are under source/. Archive paths checked.
.NET8 WindowsDesktop runtime already installed. Nothing launched or deployed.

The official client deploys mounter.elf to elfldr TCP9021. Payload protocol TCP9090:
GET_FW; GET_USERS; SEARCH <user_hex> PPSA01342; MOUNT <user_hex> PPSA01342 <dir>;
READ_FILE <path> (OK bytecount followed by binary); UMOUNT. A mount exposes files
under /mnt/pfs/. Export the existing save contents and metadata, then unmount.
No CREATE operation is needed for exporting the user?s existing save.

At the time checked, PS5 192.168.15.42 timed out on2121 and1337. The earlier game
FTP helper is E:/Emulation/PS5/Tools/DumpSetup-12.20/dump_demons_souls_ftp.py.
Do not restart the hour-long game dump or stop the existing DNS/web host blindly.

An async question asks whether the user already has a character on their PS5;
no answer yet. The console may need to be powered on and jailbroken again. If no
character exists, create one on the PS5 and reach a real autosave (ideally a known
location such as the Nexus), then export it. Check game version before importing.
Keep the original export separate and import a copy into an isolated emulator
install. A save loading past the transition narrows the crash trigger; it does not
on its own prove that save handling caused the GPU fault, since loaded scenes use
different graphics workloads.

No emulator source changes, new emulator build, emulator launch, save replacement,
or external posting were performed during this investigation.


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
