# FrameGen for Windows (NVIDIA RTX) — plan

## Status (22 Nov 2026)

| # | Milestone | Status |
|---|---|---|
| M0 | Project setup (CMake + build.ps1, VS Build Tools, no external deps) | ✅ done — CI not set up yet |
| M1 | Passthrough: capture → present, card auto-pick, format choice, stats | ✅ done — a typical UVC card auto-picks 1080p120 NV12; colours verified |
| M2 | Test pattern + signal simulation + test tools | ✅ done — `--source synthetic --fps --signal-hz --game-fps --scene`, `--bench`, `--dump`, `--check-colors`, `--check-hud`, `--check-quality` (new), `--dump-live`, `--log`, `--simulate-slow` |
| M3 | Repeat detection, cadence, adaptive delay | ✅ done — 30 fps on 60 Hz → 30 unique; 25/50 Hz jitter 3.33 → 0.38 ms |
| M4 | Frame generation engine + engine swap without freezing | ✅ done — own GPU motion-flow engine (Low latency ≈ 1 ms, Smoothest 4× ≈ 4 ms at 1080p on a high-end RTX GPU); background swap |
| M5 | Audio passthrough | ✅ implemented (WASAPI, ring buffer steered to the video delay, output picker) — sync not yet measured with a real headset |
| M6 | Dashboard + Settings + prefs + auto-adjust + display cap | ✅ done — same layout/wording as Mac 1.2.0 |
| M7 | HUD stabiliser + `--check-hud` | ✅ ported — UI error 2.15 → 1.68 (our engine already wobbles less than Apple's) |
| M8 | DLSS-G via Streamline; FSR 3 fallback | ✅ NVIDIA VFG (Video Effects SDK) integrated and selectable — see docs/VFG.md. 🧪 DLSS-G integrated, **experimental** (GPU reset after a few seconds — see docs/DLSS.md). FSR 3 / NVIDIA FRUC not started |
| M9 | Installer, signing notes, release | ⏳ not started |

Engine choice differs from the original plan: NVIDIA's Optical Flow FRUC library needs a
developer-account download, so v1 ships FrameGen's own engine (D3D11 compute: hierarchical
symmetric block motion search at the in-between time + per-pixel vector selection + occlusion-aware
blending). It runs on any DX11 GPU and is measured against ground truth (`--check-quality`):
PSNR ≈ 38 dB vs 23 dB for a cross-fade on the action test scene, ≈ 29 dB on a 120 px/frame pan.

Open issues: DLSS-G GPU reset (docs/DLSS.md). See "Live findings" in DEVELOPMENT.md.

---

*The original hand-off plan follows (unchanged).*

Goal: the same simple experience as the Mac app — plug in a capture card, open the app, get 2×–4×
frame generation with sound, a tiny dashboard (H) and a Settings window for the rest — on Windows
PCs with NVIDIA RTX GPUs, using the GPU's extra power for better quality, 4K and 4×.

This is a separate project from the Mac app. The Mac version (Swift, Metal, VideoToolbox) lives in
its own repository and is a reference only: no code is shared. This document is the hand-off from it: what to build, in which order, with which SDKs, and
every lesson learned the hard way on real hardware. Read the "Lessons" section before writing
pipeline code.

## 1. What carries over and what doesn't

The Swift/Metal code can't be reused, but every algorithm ports 1:1 — and those were the hard part.

| Mac file | Responsibility | Windows equivalent |
|---|---|---|
| Sources.swift (CaptureSource) | UVC capture, format/rate choice, audio input | Media Foundation Source Reader → D3D11 NV12 textures (§3.1) |
| Sources.swift (SyntheticSource) | test pattern + signal-chain simulation | port as-is (Direct2D/CPU drawing) — essential for testing |
| Pipeline.swift | repeat detection, cadence, scheduling, model swap without freezing | port logic as-is (C++) |
| Interpolators.swift | frame generation engines + support detection | NVIDIA Optical Flow FRUC / DLSS-G / FSR3 / RIFE (§2) |
| MetalContext.swift | display shaders, edge repair, HUD stabiliser | HLSL compute/pixel shaders (logic as-is) |
| Renderer.swift | presentation thread, adaptive delay, compare mode | D3D11/12 flip-model swap chain + waitable object (§3.4) |
| Audio.swift | delayed, drift-corrected audio passthrough | WASAPI shared-mode render client (same ring buffer logic) |
| Dashboard.swift, Settings.swift, main.swift | UI, prefs, auto-adjust, menus | WinUI 3 (or Qt) — copy layout, wording and defaults |

## 2. Frame generation on NVIDIA — options

DLSS Frame Generation is designed for games: it needs motion vectors and depth from the game
engine. A capture card only gives finished pictures, so motion must be estimated from the video.

| Option | GPUs | Notes |
|---|---|---|
| A. NVIDIA Optical Flow SDK — FRUC library (NvOFFRUC) | RTX 20+ | Purpose-built video frame interpolation from two frames. Free SDK (developer account); check DLL redistribution terms. |
| B. DLSS Frame Generation via Streamline, fed with optical-flow motion vectors + flat depth | DLSS-G 2×: RTX 40+; MFG 3×/4×: RTX 50 | Highest potential quality, but a D3D12 present path, Reflex markers, and more licensing to check. The HUD stabiliser mask can supply DLSS-G's "HUD-less" input. |
| C. AMD FidelityFX FSR 3 frame generation | Any DX12 GPU | MIT-licensed; cross-vendor fallback. |
| D. RIFE via TensorRT (or ONNX Runtime + DirectML) | RTX 20+ | Open models; fallback / experimentation. |

Recommended path: A for v1, then B as an "Ultra" mode on RTX 40/50, with C as the fallback. Keep
the engine-support table and the automatic fallback. Optional: RTX Video Super Resolution to
upscale 1080p → 4K after frame generation.

**Licensing:** This project is MIT. Never copy code, shaders or configs from GPL/AGPL projects
(other capture-card frame generation tools included); studying their behaviour and documentation
is fine. Check each NVIDIA/AMD SDK's redistribution terms before shipping DLLs.

## 3. Architecture

```
Capture card ─MF Source Reader─► NV12 D3D11 texture + QPC timestamp
   │ audio (WASAPI capture)                    │
   ▼                                           ▼
ring buffer ─► WASAPI render         repeat detection + cadence (24×15 tile diffs)
                                               │ unique game frames
                                     HUD stabiliser: hide UI (compute shader)
                                               │
                                     frame generation engine
                                               │
                                     paste UI back
                                               ▼
                    present thread: flip-model swap chain, adaptive delay, VRR aware
```

- **3.1 Capture** — enumerate formats per device; requested size (default 1920×1080), fastest rate
  ≤ 120 Hz in Auto, prefer NV12/native YUV, avoid high-speed modes; use the device's exact frame
  interval; timestamps on the QPC timeline; the card's audio endpoint; hot-plug.
- **3.2 Repeat detection & cadence** — port exactly (Lessons 1–4).
- **3.3 Frame generation** — swap engines on a background thread (Lesson 8).
- **3.4 Presentation** — flip-model, waitable object, dedicated high-priority thread (Lesson 11),
  tearing + VRR (Lesson 12), adaptive delay (rise fast, fall slowly; ignore warm-up), optional Reflex.
- **3.5 UI** — copy the Mac UI 1:1; everything automatic by default; keep the display awake.
- **3.6 Packaging** — Inno Setup or MSIX; document SmartScreen; camera/mic privacy messages.

## 4. Milestones

M0 setup · M1 passthrough · M2 test pattern + tools · M3 repeats/cadence/delay · M4 engine A +
swap · M5 audio · M6 dashboard/settings/auto-adjust · M7 HUD stabiliser · M8 DLSS-G + FSR 3 ·
M9 installer and release. Port the Mac test tools early (M2) — they found every serious bug.

## 5. Lessons learned on the Mac (read before coding)

1. Consoles send a fixed 60 Hz signal; a 30 fps game repeats every frame. Drop repeats, keep the
   first copy's timestamp.
2. Repeat detection must be tile-based (24×15 tiles of sparse luma; repeats ≤ 0.58, quietest real
   frames ≥ 1.24 → threshold 0.9; plus a global mean < 0.6). Compare against the last accepted frame.
3. Safety net: never hold a frame longer than ~1.6 game intervals (45–110 ms); don't interpolate
   between repeats passed by the safety net.
4. Capture at the card's fastest rate ≤ 120 Hz in Auto; snap unique-frame timestamps to the learned
   cadence (trimmed mean; pull deviations < 45 % by 15 %; reset on larger jumps).
5. Cards report odd exact rates — use the device's own frame duration; verify the negotiated type.
6. Pixel formats bite — check each engine's formats, convert, never present an undrawable frame;
   verify colours with a ground-truth check.
7. Model warm-up: warm up off the live path; exclude the first frame after a size change from stats.
8. Swap engines in the background, pass real frames through meanwhile; never run two sessions at once.
9. Edge artifacts: check output borders.
10. HUD / text wobble: stillness-based UI detection with leaky counters (+1/−6, cap 40), ±2 px AA,
    solid interiors; hide UI from the model, paste it back; gate by motion. Don't retry the rejected
    approaches (generic per-pixel lock; cross-fade band).
11. Present from a dedicated thread; cache device enumeration.
12. Frame pacing: prefer VRR; otherwise suggest a refresh rate that's a whole multiple.
13. Slow GPUs: step down 4× → 3× → 2× → other engine → pause; grace period after changes; one-click
    lower resolution; cap the multiplier to the display refresh.
14. UI details: a hidden dashboard must not catch clicks; don't steal focus from a terminal; clear
    permission messages; remember settings.
15. Audio: steer a ring buffer's fill level to the video delay minus output latency; ±0.4 %
    resampling; jump if > 120 ms behind.

## 6. Reference numbers (Mac)

M5 Max, 1080p, 30 → 60 fps: Low latency ~10 ms/frame, Smoothest ~15 ms; delay ≈ 45–60 ms.
