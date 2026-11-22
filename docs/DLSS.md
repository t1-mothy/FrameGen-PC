# NVIDIA DLSS Frame Generation

FrameGen can hand frame generation to NVIDIA DLSS Frame Generation through NVIDIA Streamline
(RTX 40: 2×, RTX 50: up to 6×; FrameGen offers 2×–4× and caps to what the card allows). Choose
**NVIDIA DLSS FG** in Mode (dashboard, Settings, right-click menu) or press **D**. On a GPU that
can't run it, choosing it says *"Your GPU does not support NVIDIA DLSS Frame Generation. Frame
generation is disabled, please select another option."* (or names the problem, e.g. Hardware-
accelerated GPU scheduling off) and generation stays off until another mode is picked.

DLSS-G only generates while FrameGen's window has keyboard focus (the dashboard says so) — click
the picture. Unlike VFG and FrameGen's own engine, DLSS-G needs motion vectors and depth, which a
capture card can't provide; FrameGen estimates motion with its own search and passes a flat depth.

## Setup (source builds)

```powershell
.\tools\get_streamline.ps1      # downloads the Streamline SDK into third_party\streamline (≈ 260 MB)
.\build.ps1                     # copies the signed runtime DLLs into build\streamline
.\build\framegen-cli.exe --check-dlss
```

Requirements: NVIDIA RTX 40/50, a current driver, Windows 10 2004+, *Hardware-accelerated GPU
scheduling* on. The installer ships `sl.interposer/common/dlss_g/reflex/pcl.dll` and
`nvngx_dlssg.dll` in `streamline\` with their licences (`streamline\licenses`): Streamline is MIT;
the DLSS-G / NGX binaries are NVIDIA's, distributed as part of FrameGen under the NVIDIA RTX SDK
licence. FrameGen only loads `sl.interposer.dll` if it carries NVIDIA's signature. `FG_NO_DLSS=1`
skips Streamline entirely.

## How it works

- `DlssG.cpp` — loads Streamline at runtime (manual hooking, no over-the-air downloads), starts it on a
  background thread at launch (slInit takes ~1.3 s; with manual hooking it runs alongside the D3D11
  start-up), checks support in the background, creates the D3D12 device + queue through Streamline's proxy.
- `DlssPresenter.cpp` — replaces the D3D11 presenter while DLSS is the engine. Direct3D 11 still
  composes each real frame (scaling, dashboard) into a window-sized texture shared with D3D12, plus
  a motion-vector image from FrameGen's own motion search (`FlowEngine::motionField`, t = 1) and a
  flat depth. D3D12 copies it into the proxy swap chain's back buffer, tags depth + motion vectors,
  sets constants (identity matrices, `mvecScale` = 1/size, camera motion included) and the PCL/Reflex
  markers, and presents. DLSS-G generates and paces the in-between frames.
- The pipeline still does repeat detection and cadence; real frames are presented at their content
  time + an adaptive delay. `slDLSSGGetState().numFramesActuallyPresented` feeds the FPS display.

## History

Until FrameGen 1.1 DLSS-G was behind `--experimental-dlss`: on the development PC it ended in a
GPU reset after a few seconds (a fault inside DLSS-G's own command list, NGX `0xbad00002`). The
cause was the PC — its GPU was linked at PCIe x2 through a faulty riser. After reseating, DLSS-G
ran without errors on the test pattern and live from a capture card (2× and 4×). If it ever
recurs: `FG_DRED=1` prints DRED breadcrumbs; NVIDIA's development DLLs go in `build\streamline_dev`
(run with `FG_SL_DEV=1`).
