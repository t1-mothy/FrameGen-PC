# CLAUDE.md — context for Claude Code

## Project
FrameGen for Windows: real-time frame generation for console capture cards on Windows PCs
(C++20, Direct3D 11 compute, Media Foundation, WASAPI, Direct2D). The product's point is
**simplicity**: automatic defaults, a small dashboard (H), a Settings window (Ctrl+,) for the rest,
plain-English messages. Same UX as the Mac app (Swift/Metal), which is a *reference only* — no code
is shared (file names in docs refer to the Mac app's `Sources/FrameGen/`).

Read `docs/PLAN.md` (goals, milestones, Lessons learned on the Mac — read before touching the
pipeline) and `docs/DEVELOPMENT.md` (architecture, measurements, test tools).

## Build & test
- `.\build.ps1` → `build\FrameGen.exe` (app) and `build\framegen-cli.exe` (same program as a
  console app: use it for tools and `--log`). Needs Visual Studio 2022+ / Build Tools with C++.
- Shaders are HLSL strings compiled at runtime (`D3DCompile`) — no offline shader compiler.
- Verify with measurements, not by eye: `framegen-cli --bench`, `--check-quality` (ground truth),
  `--check-hud`, `--check-colors`, `--list-devices`, `--source synthetic --fps 60 --repeat 2 --log
  --quit-after 8 --windowed`, real card: `--source capture --log --dump-live out\live`.
  `FG_PREFS=<file>` keeps tests from changing the user's settings; `FG_TRACE=1`, `FG_PROFILE=1`
  for per-frame timings / GPU passes.
- NVIDIA DLSS Frame Generation needs the Streamline SDK in `third_party\streamline`
  (`tools\get_streamline.ps1`); Streamline starts in the background at launch. See `docs/DLSS.md`.
- NVIDIA VFG (AI frame generation, `src/VfgEngine.*`) loads NVIDIA's Video Effects runtime at run
  time; for development put the four DLLs in `third_party\nvvfx\runtime` (copied to
  `build\nvidia-vfx`). See `docs/VFG.md`. Compare engines on real frames with `--check-real`.

## Rules
- License is MIT. 
  NVIDIA SDK binaries are not committed (third_party/ is git-ignored); check their licences
  before shipping them.
- Launching from a terminal must not steal keyboard focus (`--activate` exists only for tests,
  e.g. DLSS-G, which pauses when the window isn't focused).
- The GPU work is shared by several threads: every bind→dispatch→unbind sequence holds
  `Gpu::lock()`, never wait for the GPU while holding it, and never let a thread block inside D3D
  while holding it (that's what made the card path stutter — see DEVELOPMENT.md, "Live findings").

## Typical test setup
Console (HDCP off) → USB (UVC) capture card (NV12, 1080p120) → PC with an NVIDIA RTX 40/50 GPU and
a high-refresh display (a refresh rate that's a multiple of 60 gives even pacing for 60/120 fps).
If the card path stutters, check the GPU's PCIe link width first (a riser running at x2 caused
~50 ms upload stalls during development).
