# Reaching the Nexus

How to get Demon's Souls from a cold launch to the parked benchmark scene, by hand or scripted.
Every performance number in these docs was taken in this scene, so repeat it exactly.

## Prerequisites

- Launch with the root `Play Demon's Souls.cmd`. It sets the `KYTY_DEBUG_DES_TOUCH_*` variables the
  collision workaround needs; without them the game never reaches gameplay.
- The save in `_Runtime/_SaveData/PPSA01342` has the character standing at the Nexus archstone.
  Do not overwrite it.
- The window title carries the frame counter and frame rate, e.g. `frame: 1242, fps: 9`. Pace on
  the frame counter, not on wall-clock guesses.

## Sequence

1. Wait until the title shows **frame 1000** or later. The opening cinematic is running and has
   become skippable. Before that, input is ignored. The frame at which it becomes skippable has
   moved as performance improved (700 was enough at 8 FPS), so when in doubt wait longer; a late
   hold costs seconds, an early one is ignored.
2. **Hold Cross for 4 seconds.** Cross is the `J` key (VK `0x4A`, scancode `0x24`, see
   `hostInput.cpp`). The cinematic skips; the counter jumps by about 100 frames.
3. **Press Cross about 12 times, 3 seconds apart**, wall clock. This walks Press Any Button, then
   Continue, then Continue Offline. Menu transitions are animations on a wall clock, so frame-paced
   gaps outrun them at high frame rates.
4. Around the fourth or fifth press the frame rate drops from about 32 to about 10. That is the
   save loading, not a problem. About 50 seconds after the hold you are in the Nexus, facing the
   archstone, at roughly frame 1200 to 1250.
5. Confirm with a screenshot. `_Runtime/_Diagnostics/flat-plan/after-nav.png` shows the expected
   view.

If the game already sits on the title screen (frame counter far past 1000, 32 fps), start at step 2.

## Sending input from a script

Post the keystrokes straight to the game window with `PostMessage(WM_KEYDOWN)` /
`PostMessage(WM_KEYUP)`. Do not use `SetForegroundWindow` plus `keybd_event`: `SetForegroundWindow`
fails silently unless the caller already owns the foreground, and the keystrokes then land in
whatever window has focus. Windows UIPI also blocks input from a medium-integrity process to an
elevated emulator, so an elevated emulator needs an elevated sender.

The local helper does all of this:

```
powershell -NoProfile -ExecutionPolicy Bypass -File _Build\des-navigate.ps1 -NoElevate -WaitForFrame 1000 -HoldSeconds 4 -Presses 12 -GapSeconds 3
```

Drop `-NoElevate` when the emulator runs elevated for ETW sampling; the script then self-elevates.
Retry a few seconds later if it reports that the window does not exist yet. `_Build\des-window.ps1
-Info` reads the title, `-Shot <png>` takes the screenshot. These helpers live in the ignored
`_Build` folder; the sequence above is enough to rewrite them.

## Measuring

Leave the character parked. Wait about 100 seconds for shader compilation to settle, then take a
30-second sample:

```
powershell -NoProfile -ExecutionPolicy Bypass -File _Build\measure-des-performance.ps1 -ProcessId <pid> -OutputPrefix <folder>\<name>-steady-clean -Seconds 30
```

The JSON it writes reports frames per second from presented-frame deltas, CPU core-equivalents and
GPU utilisation. Keep the Tracy capture client disconnected during a measurement.

## Known trap

Looking around in the Nexus with the camera has ended sessions with `vkWaitSemaphores:
ErrorDeviceLost`. Parked, the scene is stable for as long as it has been measured.
