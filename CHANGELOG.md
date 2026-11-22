# Changelog

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
