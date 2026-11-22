# DLSS Frame Generation (experimental)

FrameGen can hand frame generation to NVIDIA DLSS Frame Generation (RTX 40: 2×, RTX 50: up to 6×)
through NVIDIA Streamline. It's **off unless started with `--experimental-dlss`** (or
`FG_EXPERIMENTAL_DLSS=1`), because on the development PC it ends in a GPU reset after a few seconds.

## Setup

```powershell
.\tools\get_streamline.ps1      # downloads the Streamline SDK into third_party\streamline (≈ 260 MB)
.\build.ps1                     # copies the signed runtime DLLs into build\streamline
.\build\framegen-cli.exe --check-dlss
```

Requirements: NVIDIA RTX 40/50, a current driver, Windows 10 2004+, *Hardware-accelerated GPU
scheduling* on. The Streamline SDK is MIT licensed; the DLSS-G / NGX binaries (`nvngx_dlssg.dll`,
`sl.*.dll`) are NVIDIA's and come with their own licence (`third_party\streamline\license.txt`,
`*.license.txt`) — check it before redistributing them in an installer. FrameGen only loads
`sl.interposer.dll` if it carries NVIDIA's signature.

## How it works

- `DlssG.cpp` — loads Streamline at runtime (manual hooking, no over-the-air downloads), starts it
  before any DXGI/D3D call, creates the D3D12 device + queue through Streamline's proxy.
- `DlssPresenter.cpp` — replaces the D3D11 presenter while DLSS is the engine. Direct3D 11 still
  composes each real frame (scaling, dashboard) into a window-sized texture shared with D3D12, plus
  a motion-vector image from FrameGen's own motion search (`FlowEngine::motionField`, t = 1) and a
  flat depth. D3D12 copies it into the proxy swap chain's back buffer, tags depth + motion vectors,
  sets constants (identity matrices, `mvecScale` = 1/size, camera motion included) and the PCL/Reflex
  markers, and presents. DLSS-G generates and paces the in-between frames.
- The pipeline still does repeat detection and cadence; real frames are presented at their content
  time + an adaptive delay. `slDLSSGGetState().numFramesActuallyPresented` feeds the FPS display.

## Current state (22 Nov 2026)

What works: Streamline loads (signature verified), DLSS-G reports support for up to 5 generated
frames (RTX 50 series), the swap chain and all inputs are accepted without errors, and DLSS-G does
present 2 frames per real frame.

What fails: a few seconds after DLSS-G starts generating, its own command list
(`nv.sl.dlss_g.cmdCtx.dlssg`, op 150 of 181, a CopyResource) faults at GPU address 0, NGX reports
`0xbad00002` (platform error) and the device is removed (`DXGI_ERROR_DEVICE_HUNG`); the driver logs
"Graphics SM Warp Exception: MMU Fault". DRED output: run with `FG_DRED=1`. NVIDIA's development
DLLs: build copies them nowhere by default — copy `third_party\streamline\bin\x64\development\*` to
`build\streamline_dev` and run with `FG_SL_DEV=1`.

Leads, in order:
1. **Hardware.** Rule out the PC itself (PCIe link width, NVIDIA driver errors in the event log)
   by re-testing on another RTX 40/50 PC before changing code.
2. Install Windows *Graphics Tools* (Settings › System › Optional features) to get the D3D12 debug
   layer, run with it enabled and read the exact error behind NGX's platform error.
3. Inputs to double-check: depth format/state (currently R32_FLOAT render target), whether a
   HUD-less colour + UI alpha tag is needed, Reflex sleep (`slReflexSleep`) per frame.
