# FrameGen for Windows — development notes

## Layout

```
src/
  main.cpp            entry points (FrameGen.exe = Windows app, framegen-cli.exe = console build)
  App.*               window, sources, generation settings, auto-adjust, display cap, dashboard wiring
  Core.*              clock (QPC), logging, VideoFrame, Stats, command-line options
  Gpu.*               D3D11 device, runtime HLSL compilation, image pools, colour conversion,
                      sparse luma, PNG export, GPU profiler, Uploader (separate device for card uploads)
  Capture.*           Media Foundation capture card source (format choice, timestamps, CPU repeat samples)
  Synthetic.*         test pattern (Direct2D) + signal-chain simulation + capture-noise simulation
  Pipeline.*          repeat detection, cadence, scheduling, background engine swap (port of Pipeline.swift)
  FlowEngine.*        FrameGen's GPU frame interpolation (motion search + interpolation, HLSL compute)
  Interpolators.*     engine interface, blend baseline, engine support table
  VfgEngine.*         NVIDIA VFG (Video Effects SDK, loaded at run time, D3D11↔CUDA interop) — docs/VFG.md
  Hud.*               HUD / text stabiliser (port of the Mac's Metal kernels)
  Renderer.*          Presenter interface + D3D11 presenter (vblank-driven, present only on change)
  DlssG.*, DlssPresenter.*   NVIDIA DLSS Frame Generation through Streamline (docs/DLSS.md)
  Audio.*             WASAPI capture of the card's endpoint + delayed, drift-corrected render
  Dashboard.*         Direct2D dashboard + FPS counter (overlays composited by the presenter)
  Settings.*          Win32 Settings window (Video, Frame Generation, Display, Sound, Interface)
  Prefs.*             %APPDATA%\FrameGen\settings.ini (FG_PREFS=<file> overrides — use it in tests)
  Tools.cpp           --list-devices, --bench, --dump, --check-colors, --check-hud, --check-quality, --check-real, --stress
  Install.*           per-user install helpers; FrameGen.exe --uninstall [--quiet]
installer/Setup.cpp   the installer (files embedded as resources by CMake; package.ps1 → dist\)
tools/make_icon.py    renders src/framegen.ico (pure Python)
tools/strip.py        dev aid: side-by-side crops of PNGs
tools/get_streamline.ps1   fetches the Streamline SDK (optional)
```

## The frame generation engine (FlowEngine)

1. Feature pyramids of both real frames (Y + chroma, R10G10B10A2; luma alone can't tell a red ball
   from green grass) down to ≈ 1/32 scale at 1080p.
2. Symmetric block motion search at the in-between time t: for each 8×8 block find v with
   A(p − t·v) ≈ B(p + (1−t)·v). Coarsest level: full search ±8 px (= ±256 px at 1080p). Finer levels:
   candidates from the coarser level, zero and the previous pair, one/two propagation passes, then
   ±1 / ±0.5 px refinement. Candidates are evaluated in parallel (16 threads per block).
   Cost = colour difference + capped smoothness + a *saturating* length penalty (flat areas keep
   small vectors — without it flat-coloured objects vanished; unsaturated, it fought real 120 px pans).
   The frame's 4 dominant motions (a vote over the 1/32 field) are offered to every finer block as
   candidates. That recovers a camera pan for regions the coarse level averaged away, such as a
   horizon strip behind a still car.
3. Per pixel: the smoothly interpolated vector and the 5×5 nearest block vectors are scored by patch
   match. Every candidate that matches about as well as the best contributes, weighted by
   exp(−Δcost/σ) × an overlapped-block window (σ = 0.02). Neighbouring blocks with different vectors
   blend instead of meeting at a hard edge, which was the "boxy" look. Each candidate's colour uses
   the occlusion rule: on disagreement, use the frame that shows the pixel (motion-field divergence:
   converging = covered → earlier frame, diverging = uncovered → later).
Low latency: one field at t = ½ (half-res) reused for all phases. Smoothest: a field per phase,
refined to full resolution.

## Measurements (high-end NVIDIA RTX desktop GPU, 1080p)

| | Low latency | Smoothest | Blend |
|---|---|---|---|
| `--bench` 2× | 1.9 ms | 2.6 ms | 0.1 ms |
| `--bench` 4× | 4.7 ms | 7.4 ms | 0.1 ms |
| `--check-quality` action, 2× (PSNR) | 38.6 dB | 37.8 dB | 23.0 dB |
| `--check-quality` 120 px/frame pan, 2× | 29.6 dB | 28.9 dB | — |
| `--check-real` console gameplay: block grid (1.00 = none) | 1.014 | 1.020 | — |
| `--check-real` console gameplay: PSNR vs NVIDIA VFG | 32.0 dB | 32.3 dB | — |

NVIDIA VFG (Balanced): `--bench` 3.1 ms at 2×, 8.2 ms at 4×. `--check-quality`: action 40.4 dB,
pan 30.9 dB. On the gameplay pairs its block grid is 1.007. Before soft blending, the
dominant-motion candidates and the 5×5 neighbourhood, our engine scored grid 1.032 and 31.4 dB vs
VFG. Tried and rejected: an exhaustive search at the 1/16 level (−1.3 dB on the synthetic test)
and a 3×3 vector median on the final field (erases small objects).
| `--check-hud` UI panel error (off → stabiliser) | 2.15 → 1.68 | 2.18 → 1.71 | — |

Live, test pattern, 30 fps game on 60 Hz: 2× → 60 fps, delay ≈ 24 ms; Smoothest 4× → 120 fps,
delay ≈ 41 ms. 25 fps PAL on 50 Hz captured at 120 Hz: cadence jitter 3.33 → 0.38 ms.

## Live findings (things that broke on real hardware)

- **Presenting every refresh of a 280 Hz panel** made the GPU switch to the compositor 280×/s and
  doubled the cost of the motion search → present only when the picture changes, woken by
  `IDXGIOutput::WaitForVBlank`, and only into a free back buffer (frame-latency waitable — drawing
  into a buffer still queued for display stalls the whole GPU queue, which froze the window).
- **Waiting 0 ms for the waitable** skipped every other refresh when output = refresh (60 fps on a
  60 Hz display showed 30) → wait up to ~0.7 refresh.
- **Frame queue cap 24** was too small for 240 fps output → 128.
- **The capture thread blocked inside D3D while holding the device lock** (uploads up to 52 ms) and
  stalled generation, presentation and the UI → frames now come from system memory, repeat detection
  runs on the CPU (like the Mac), only *new* frames are uploaded, through a *separate* D3D11 device
  with shared fences (`Uploader`).
- The card path then still showed ~50 ms GPU transfer stalls in bursts, even on the separate
  device; the test pattern (no uploads) never did. Cause: the GPU linked at PCIe x2 through a riser.
  After reseating it (x8) the stalls were gone. Check the PCIe link width first if capture stutters.
- The step-down rule copied from the Mac (15 drops in 250 ms = too slow) paused generation on a
  one-off hiccup → now needs generation time > 80 % of the budget (or drops with > 50 %) for 1 s.
- Dumping PNGs on the pipeline thread (`--dump-live`) caused 0.5 s hitches → background thread.
- `--stress` (heavy GPU compute + VRAM pattern test) crashed this PC with a 0x116 bugcheck — don't
  run it there until the hardware is fixed.

## Test tools

```powershell
.\build\framegen-cli.exe --list-devices
.\build\framegen-cli.exe --bench [--width 3840 --height 2160]
.\build\framegen-cli.exe --check-quality        # FG_SCENE=action|fastpan|still|hud, FG_QUALITY_DUMP=<dir>
.\build\framegen-cli.exe --check-hud            # FG_SCENE, FG_NOISE=1, FG_HUD_DUMP=<dir>
.\build\framegen-cli.exe --check-real out\game      # engines on real --dump-live pairs (FG_REAL_OUT=<dir>)
.\build\framegen-cli.exe --check-colors
.\build\framegen-cli.exe --dump out\dump --scene fastpan
.\build\framegen-cli.exe --source synthetic --fps 120 --signal-hz 50 --game-fps 25 --log --quit-after 8 --windowed
.\build\framegen-cli.exe --source capture --log --dump-live out\live --quit-after 20 --windowed
```

`FG_PREFS=<file>` (separate settings), `FG_TRACE=1` (per-pair timings, lock waits),
`FG_PROFILE=1` (GPU time per pass in `--bench`), `--simulate-slow 12`, `--cycle-modes`,
`--ui-shot file.png`, `FG_SETTINGS_SHOT=<dir>` (PNG of each Settings tab),
`FG_ENGINES=fast,quality,vfg` (engines for `--bench` / `--check-quality` / `--check-real`), `FG_VFG_MODEL`,
`FG_SIGMA` (soft vector choice, 0 = hard), `FG_R1` (3×3 instead of 5×5 neighbourhood),
`.\build.ps1 -Target framegen-cli`, `--activate` (take focus;
tests only), `--check-dlss`, `FG_NO_DLSS=1` (skip Streamline), `FG_DLSS_NOGPU=1` (behave like a GPU without DLSS-G), `FG_DRED=1`, `FG_SL_DEV=1`.

## Release

`.\package.ps1` → `dist\FrameGen-Setup-<version>.exe` (version: `project()` in CMakeLists.txt).
The installer (`installer/Setup.cpp`) is a plain Win32 program; CMake writes `setup_payload.rc`
with FrameGen.exe, the licences, the README and — if present — the Streamline / DLSS-G DLLs
(`streamline\`) and `third_party\nvvfx\runtime\**` plus the Visual C++ runtime DLLs (from
`%VCToolsRedistDir%`) as RCDATA resources. It installs per
user to `%LOCALAPPDATA%\Programs\FrameGen` (`/S` = silent), creates shortcuts and the Windows
› Apps entry; `FrameGen.exe --uninstall` removes exactly the registered folder (and refuses
anywhere else). The app logs each run to `%LOCALAPPDATA%\FrameGen\FrameGen.log` (previous run:
`FrameGen.previous.log`) and writes a minidump there on a crash. CI (`.github/workflows/build.yml`)
builds the app and an installer without NVIDIA VFG. Test aids: `FG_VFG_NOGPU=1` (behave like a
GPU without VFG support), `FG_VFG_ANYGPU=1`.

## Mouse

Every dashboard control works with the mouse. Earlier builds drew the controls but never connected
their callbacks. Right-click anywhere opens a menu with frame generation, mode, HUD, compare, mute,
dashboard, full screen and Settings. `FG_CLICKLOG=1` logs clicks (`[click] x,y …`) for UI tests
that post mouse messages.

## Keys

H / Tab dashboard · F / F11 / double-click full screen · Esc leave full screen · M 2×/3×/4× ·
0 off · 2 Low latency · 3 Smoothest · V NVIDIA VFG · D NVIDIA DLSS FG · C compare · U HUD stabiliser ·
Ctrl+[ / Ctrl+] sound sync · Ctrl+M mute · Ctrl+, Settings.
