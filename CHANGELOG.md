# Changelog

## 1.3.0
- New mode: AMD FSR (2×) — AMD's FSR Frame Generation (FidelityFX SDK 2.4, FSR 3.1.6), included
  in the installer. Runs on most DirectX 12 graphics cards: AMD, Intel and NVIDIA including RTX
  20/30 series. Makes 2× only (with a note when more is asked for). FrameGen feeds it motion from
  its Smoothest search, tuned for FSR (plain sky moves with the camera instead of standing still,
  which had made FSR tear building edges and break up wires during camera pans). On real game
  footage it shows no block edges and handles scene changes and menu fades cleanly. Key A, or the
  Mode menu.
- Installer: installs for everyone on the PC, by default to C:\Program Files\FrameGen (Windows
  asks for administrator permission), or to a folder you choose (type it or Browse…; Browse
  installs into a FrameGen folder inside the one picked). Drive roots and folders already holding
  other files are refused with a plain explanation. Silent install: /S /D=folder. An existing
  per-user install (1.2.4 and earlier, in AppData) is moved over automatically; settings are kept.
  Uninstalling asks for administrator permission too.
- Uninstall (and moving the install to another folder) removes only the files the installer put
  there (recorded in uninstall.list), never anything else in the folder.
- New installer wording.
- NVIDIA VFG can be tried on RTX 30 series cards (experimental): NVIDIA's runtime only contains
  GPU code for RTX 40/50, so it will most likely be refused with the usual "doesn't support" message.

## 1.2.4
- Fixed the remaining stutter on high-refresh displays: redrawing the frame already on screen
  (dashboard / FPS counter updates, periodic refresh) emptied the frame queue, throwing away the
  upcoming frames. Bug introduced in 1.2.1.

## 1.2.3
- A generated frame that arrives a few ms late is shown a refresh late instead of being skipped
  (the skips were a stutter every ~0.3 s and 2x gave ~56 instead of 60 fps on 280 Hz). The
  presenter only skips ahead when frames come faster than the display refreshes, or when it has
  stayed a frame behind for more than 1/4 s (then it catches up once).
- The 10 s log line also counts frames shown late.

## 1.2.2
- Variable refresh pacing no longer busy-spins (it woke millions of times per second and starved
  frame generation): it sleeps until the next frame is due, checking every 2 ms.
- V-Sync pacing checks that the display's refresh signal is regular; on setups where it isn't
  (G-SYNC/FreeSync active, display quirks) it presents on a steady timer instead, and a refresh
  wait that returns instantly can no longer spin.
- After a graphics driver reset FrameGen logs Windows' reason and restarts itself once (a second
  reset shows a message) instead of failing silently with a frozen picture.
- Switching modes while NVIDIA VFG runs releases its CUDA interop safely (possible cause of the
  GPU resets seen when changing multiplier).

## 1.2.1
- High-refresh displays (e.g. 240/280 Hz): generated frames are no longer lost when the GPU is
  busy and a refresh is missed — a frame leaves the queue only once it's on screen, and frames are
  shown in order unless the presenter falls more than ~1.5 refreshes behind. At 120 Hz and above
  one extra frame may be queued (≤ 1 refresh of delay). Fixes 2× giving ~50 fps from 30.
- FrameGen asks Windows for high GPU scheduling priority.
- The 10 s log line also counts presenter wake-ups, skipped refreshes and frames never shown.

## 1.2.0
- Frame generation up to 8× (dashboard: Off/2×/3×/4×/6×/8×; Settings and right-click menu: every
  step 2×–8×; M cycles 2×→3×→4×→6×→8×). DLSS-G caps to what the card allows (RTX 50: 6×).
  Automatic step-down goes 8× → 6× → 4× → 3× → 2×.
- When the display can't show more frames, the dashboard says so and offers "Generate anyway".
- FrameGen.log records the PC (GPU, display rate), the settings, every generation change with the
  reason for any limit, and a status line every 10 s — for troubleshooting on other PCs.
- Fixed a refresh-rate tip that appeared for 59.94 Hz displays.

## 1.1.0
- NVIDIA DLSS Frame Generation is a regular engine ("NVIDIA DLSS FG", key D), included in the
  installer. It was experimental because of GPU resets that turned out to be a hardware fault on
  the test PC. Choosing it on a GPU that can't run it explains why and turns generation off.
- Streamline starts in the background, so launching stays fast (≈ +0.1 s).
- DLSS-G's frame count is capped to what the card allows (RTX 40: 2×), with a note on the dashboard.

## 1.0.0
- Installer (per user, no admin rights) with NVIDIA VFG included; uninstall from Windows › Apps.
- Logs and crash reports in %LOCALAPPDATA%\FrameGen (Settings › Interface › Open logs folder).
- NVIDIA VFG is always offered; on a GPU that can't run it, choosing it explains why and turns
  frame generation off until another mode is picked.
- HUD & text stabilisation is off by default (it helps only in some games).

## 0.1.0
- First Windows version: capture card (Media Foundation, NV12 up to 1080p120) and test pattern,
  repeat detection + cadence (ported from the Mac app), FrameGen's own GPU frame generation
  (Low latency / Smoothest, 2×–4×), HUD stabiliser, WASAPI sound passthrough with output choice,
  dashboard + Settings window, automatic step-down and display cap, test tools.
- Experimental DLSS Frame Generation through NVIDIA Streamline (`--experimental-dlss`).
- NVIDIA VFG engine (Video Effects SDK, RTX 40/50): selectable Mode, VFG quality setting, runtime
  folder picker. It's the default on PCs where it's installed (docs/VFG.md).
- Mode is chosen from a menu listing every engine (dashboard, Settings, right-click menu, V key).
- Fixed: the dashboard's controls didn't respond to the mouse (callbacks were never connected).
  New right-click menu with every quick option.
- Own engine, fewer block artefacts: soft overlapped-block blending of candidate vectors, a 5×5
  neighbourhood and frame-wide dominant-motion candidates. On real gameplay the block grid dropped
  from 1.032 to 1.014, and PSNR vs VFG rose from 31.4 to 32.0 dB.
- `--check-real` compares engines on real captured frame pairs.
