> Historical investigation notes from September 9?10, 2026. Later entries may supersede earlier findings. See the [current workaround](../demons-souls-workaround.md) and [performance results](../demons-souls-performance.md). Artifact paths identify local captures and may predate runtime-folder consolidation.

# GitHub and Reddit findings, 2026-09-10

The strongest actionable lead is the exact 688d485 author package in PR500,
not a newly discovered source fix for df890e8a1a32c65a.

- https://github.com/KytyPS5/KytyPS5/pull/500
  Current source head is still6c6e3e7, already tested locally. Reviewed all27
  issue comments via authenticated GitHub API, including September10 updates.
- Myoko reported RTX4070S, initially failing fb0becc9db83db77 on1d6e23a,
  then character creation crashes on865cc0d and explicitly game01.005.000:
  https://github.com/KytyPS5/KytyPS5/pull/500#issuecomment-5601930489
- Author uploaded a distinct package labelled688d485 September9,14:03UTC:
  https://github.com/KytyPS5/KytyPS5/pull/500#issuecomment-5603171558
- Myoko then reported creation + gameplay without crashes at1-2FPS:
  https://github.com/KytyPS5/KytyPS5/pull/500#issuecomment-5603576248
- AMD7900XTX tester larfy298 confirmed gameplay at2FPS:
  https://github.com/KytyPS5/KytyPS5/pull/500#issuecomment-5603286909
  This is evidence that01.005.000 can reach gameplay on a community build.
  It does not prove the user's GPU fault is fixed.

Downloaded exact author ZIP and verified archive contents/build identity:
ZIP SHA25611723163893a8ddfb8ecae7129d20b819fc8dec5543db251a7e618a9b29a94da
EXE SHA256fdb9062085b1f46d3b599127af1005345ccea9f01558c271836e6e8cd5525235
Embedded full commit688d485fd48a8fc66f54731f51a3c0b2484afd2c.
GitHub rejects both short and full commit lookup, so the source difference from
6c6e3e7 is unknown. Do not assume this package is source-identical to our build.
Installed unchanged in E:\Emulation\PS5\KytyPS5-DeS-688d485-author.
--help smoke check printed expected identity and CLI; no game launched yet.
Manual launcher Play Demon's Souls.cmd points at the existing dump and captures
stdout/stderr in a fresh random folder. Copied existing options save to new folder.

Other relevant fork/PR review:
- RainKikyou/demons-souls-shaders is exactly1d6e23a; not a new alternative.
- LordixDemon PR477 claims PPSA01341 in-game, based on older code. Maintainer
  intends individual cherry-picks. More speculative fallbacks, less exact match.
  https://github.com/KytyPS5/KytyPS5/pull/477
- Leclowndu93150 PR1 to TarkusR: +42% draw throughput claim, tested in-game;
  descriptor caching optimizations, not an identified fix for our fault.
  https://github.com/TarkusR/KytyPS5/pull/1
- Techx3 PR506: CPU scheduling/cache optimization, no game FPS benchmark on its
  isolated branch. Not evidence of our crash fix.
  https://github.com/KytyPS5/KytyPS5/pull/506
- KytyPlus branches/issues checked; no matching Demon Souls crash report found.
- Upstream and TarkusR GitHub Discussions are disabled; discussion is in PRs/issues.
- Upstream issues63/316 are older menu/compile failures. masterSemaphore also
  appears in unrelated titles, so the common fatal line is not a unique cause.
- Exact shader-hash searches found no public indexed issue fordf890e8a1a32c65a.
- Fork inventory saved _Build/github-forks.jsonl; only relevant candidates reviewed,
  not a source audit of every fork.

Reddit:
https://www.reddit.com/r/emulation/comments/1wae06y/ps5_emulator_kytyps5_can_now_run_demons_souls/
https://www.reddit.com/r/pcmasterrace/comments/1wafjbk/demons_souls_remake_gameplay_on_kytyps5_emulator/
These lead back to TarkusR PR500 and PSXTech video2MFrmQRiYNo, not a separate fix.
r/emulation includes a5090/9950X3D link:
https://x.com/Shadowth117/status/2097229552791740426
Direct X fetch failed and xcancel showed browser verification. Have not independently
verified that media's exact build/settings; do not claim a verified5090 fix.
Broader Reddit searches for exact688d485,1d6e23a,crash,black screen,settings found
no actionable fix for our GPU fault. Historical custom GTA builds mention Demon
Souls but not a matching public stable build.

Current local diagnostic PID27944 is still running; user controls it.
Author package is staged for the next comparative run. No emulator inputs injected,
no comments/issues posted, no message sent to anyone.

The resource-trace run ended with the same GPU fault and successfully saved both
captures. The exact author package has now been launched as PID 29400, using only
--game and default graphics settings. Logs are in the author's install directory
_Diagnostics\first-run. The user controls progression; local gameplay is pending.
