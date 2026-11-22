# FrameGen for Windows

**Real-time frame generation for capture cards on Windows PCs.**

FrameGen takes a game console's picture from a USB capture card, generates extra frames in
between on your graphics card, and shows it at 2×–4× the frame rate — e.g. a 30 fps PlayStation 5
game at 60 or 120 fps — with the console's sound. The Windows sibling of FrameGen for Mac: same
simple dashboard (H), same Settings, same automatic behaviour.

## Features

- **2× / 3× / 4× frame generation**, with a choice of engine:
  - **NVIDIA VFG** (RTX 40/50): NVIDIA's AI video frame generation, included in the installer. It
    gives the cleanest picture on fast camera movement (≈ 3 ms per frame at 1080p on a current RTX
    GPU). See [docs/VFG.md](docs/VFG.md).
  - FrameGen's own GPU engine (Direct3D 11 compute, any modern GPU): *Low latency* (≈ 2 ms) and
    *Smoothest* (searches motion for every generated frame).
- **Any console and signal rate** — captures as fast as the card allows (up to 120 Hz), detects the
  game's real frame rate (30 fps games arrive as repeated frames on a 60 Hz signal) and smooths
  frame timing; 120 Hz and 50 Hz (PAL) consoles work too.
- **HUD & text stabilisation** — on-screen UI is detected, hidden from motion estimation and pasted back.
- **Sound passthrough**, delayed to stay in sync, with volume / mute / sync offset and output choice.
- **Adapts to the PC** — steps down 4× → 3× → 2× → other mode on slower GPUs, never generates more
  frames than the display can show, suggests an even refresh rate (or use G-SYNC / FreeSync mode).
- **Dashboard** (H) with output vs game FPS, graph, delay and quick controls (mouse or keys), a
  **right-click menu** with every quick option, and **Settings** (Ctrl+,).
- Experimental: **NVIDIA DLSS Frame Generation** (RTX 40/50) — see [docs/DLSS.md](docs/DLSS.md).

## Install

Run `FrameGen-Setup-<version>.exe` (from Releases, or build it — see below). It installs for your
Windows account, no administrator rights needed, into `%LOCALAPPDATA%\Programs\FrameGen`, with
Start menu and desktop shortcuts. Uninstall from Windows Settings › Apps. The installer isn't
code-signed, so SmartScreen may warn: *More info › Run anyway*. The first start of the installer
can take a few seconds while antivirus scans it (it carries NVIDIA's ≈ 200 MB AI model).

Logs and crash reports: `%LOCALAPPDATA%\FrameGen` (Settings › Interface › Open logs folder).

## Requirements

Windows 10/11 (64-bit), a DirectX 11 graphics card (NVIDIA RTX 40/50 for NVIDIA VFG), a USB (UVC)
HDMI capture card. PS5 / PS4: turn **HDCP off** (Settings › System › HDMI).

## Build

Visual Studio 2022 or newer (Build Tools are enough) with "Desktop development with C++":

```powershell
.\build.ps1
```

→ `build\FrameGen.exe` (double-click to run) and `build\framegen-cli.exe` (same app, console output —
for the test tools). No other dependencies; shaders compile at runtime.

```powershell
.\package.ps1
```

→ `dist\FrameGen-Setup-<version>.exe`. NVIDIA VFG is included when NVIDIA's runtime DLLs are in
`third_party\nvvfx\runtime` (not in the repository — NVIDIA's licence, see [docs/VFG.md](docs/VFG.md)).

## Using it

Plug in the capture card and start FrameGen — it picks the card, the best capture mode and your
saved settings. Everything is on the dashboard and the right-click menu. Keys: **H** shows/hides
the dashboard, **F** full screen, **M** cycles 2×/3×/4×, **0** off, **2** Low latency, **3** Smoothest,
**V** NVIDIA VFG, **C** compare original vs generated, **U** HUD stabilisation, **Ctrl+,** Settings.

If Windows blocks the camera ("Camera access needed"), turn on *Let desktop apps access your camera*
(Settings › Privacy & security › Camera); the dashboard has a button for it.

## How it works / development

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) (architecture, engine, measurements, test tools)
and [docs/PLAN.md](docs/PLAN.md) (goals, milestones, lessons from the Mac version).

## License

[MIT](LICENSE). Not affiliated with NVIDIA, Sony, Microsoft or any game publisher. NVIDIA SDK
components (optional, not included) are covered by NVIDIA's own licence terms.
