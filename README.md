# FrameGen

**Boost FPS by up to 8x through real-time frame generation for capture cards on Windows PCs**

## Features

- **2× – 8× frame generation**, with multiple supported engines, including:
  - **NVIDIA VFG** (RTX 4/5000 series)
  - **NVIDIA DLSS FG** (RTX 4/5000 series)
  - **AMD FSR** (2× only, most DirectX 12 GPUs incl. NVIDIA RTX 2/3000 series)
  - FrameGen's own GPU engine (Direct3D 11 compute, any modern GPU)
- **HUD & text stabilisation** - avoid inaccurate frame generation for persistent on-screen UI (eg. game HUD, window borders)
- **Sound passthrough** - view your console/capture device directly through your PC and PC speakers
- **G-SYNC and FreeSync mode support**

## Install

Run `FrameGen-Setup-1.3.0.exe` (from Releases, or build it — see below). The installer isn't
code-signed, so SmartScreen may warn: *More info › Run anyway*.

## Requirements

Windows 10/11 (64-bit), a DirectX 11 graphics card (NVIDIA RTX 40/50 for NVIDIA VFG), a USB (UVC)
HDMI capture card 

## Black Screen from Capture Card when trying to use a console (eg PS5)?
By default, some consoles/devices disable viewing through a capture card by default (eg. PS5/PS4). To fix this, turn **HDCP off** (PS5: Settings › System › HDMI).

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

Plug in your capture card/device and start FrameGen.

Keys: **H** shows/hides
the dashboard, **F** full screen, **M** cycles 2×/3×/4×/6×/8×, **0** off, **2** Low latency, **3** Smoothest,
**V** NVIDIA VFG, **D** NVIDIA DLSS FG, **A** AMD FSR, **C** compare original vs generated, **U** HUD stabilisation, **Ctrl+,** Settings.

If Windows blocks the camera ("Camera access needed"), turn on *Let desktop apps access your camera*
(Settings › Privacy & security › Camera); the dashboard has a button for it.

## Note
Not affiliated with NVIDIA, AMD, Sony, Microsoft or any game publisher. NVIDIA and AMD SDK
components are covered by their own licence terms.
