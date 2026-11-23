FrameGen — real-time frame generation for console capture cards
=================================================================

FrameGen takes your console's picture from a USB capture card and shows it at 2x, 3x or 4x the
frame rate (e.g. a 30 fps game at 60 or 120 fps), with the console's sound.

Getting started
---------------
1. Plug in your capture card (console HDMI -> card's IN port -> USB to the PC).
   PlayStation 5 / 4: turn HDCP off (Settings > System > HDMI).
2. Start FrameGen from the Start menu. It picks the card and the best settings by itself.
3. Pick a Mode on the dashboard (or right-click the picture):
   - NVIDIA VFG (AI): the cleanest picture. Needs an NVIDIA RTX 40 or 50 series graphics card.
   - NVIDIA DLSS FG: NVIDIA DLSS Frame Generation (RTX 40/50). Works while FrameGen's window
     is focused (click the picture).
   - Low latency / Smoothest: FrameGen's own engine, runs on any DirectX 11 graphics card.

Controls
--------
Everything is on the dashboard and in the right-click menu. Keys:
  H  show/hide the dashboard      F  full screen (Esc to leave)
  M  2x / 3x / 4x / 6x / 8x       0  frame generation off
  V  NVIDIA VFG   D  NVIDIA DLSS FG   2  Low latency  3  Smoothest
  C  compare original/generated   U  HUD & text stabiliser
  Ctrl+,  Settings                Ctrl+M  mute

Tips
----
- Sound comes out of Windows' default output. Pick another in Settings > Sound.
- For the most even motion, set your display to a multiple of the output frame rate
  (e.g. 120 or 240 Hz for 60/120 fps), or use G-SYNC / FreeSync (Settings > Display).
- "Camera access needed": turn on Windows Settings > Privacy & security > Camera >
  "Let desktop apps access your camera".
- Windows SmartScreen may warn about the installer because it isn't code-signed:
  click "More info" > "Run anyway".

Troubleshooting
---------------
Logs and crash reports: %LOCALAPPDATA%\FrameGen (FrameGen.log, crash-*.dmp).
Settings: %APPDATA%\FrameGen\settings.ini (delete it to reset everything).
Uninstall: Windows Settings > Apps > Installed apps > FrameGen.

Licences: LICENSE.txt (FrameGen, MIT) and THIRD_PARTY_NOTICES.txt (NVIDIA VFG and others).
