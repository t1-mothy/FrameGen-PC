# NVIDIA VFG (AI video frame generation)

FrameGen can use **NVIDIA Video Frame Generation (VFG)**, the AI frame interpolation filter in
NVIDIA's Video Effects SDK (Maxine), as its frame generation engine. On real gameplay it gives a
much cleaner picture than FrameGen's own motion search on fast camera movement: no block
artefacts, and thin objects like poles survive. It needs an **NVIDIA RTX 40 or 50 series** card
(Ada / Blackwell) and a current driver.

Choose it in **Mode** on the dashboard or in Settings › Frame Generation, from the right-click
menu, or press **V**. On a PC that can run it, it's the default until you pick another mode. On a
GPU that can't (not RTX 40/50, or not NVIDIA), picking it says *"Your GPU does not support NVIDIA
VFG. Frame generation is disabled, please select another option."* and generation stays off until
another mode is chosen.

## The runtime

The installer ships NVIDIA's runtime with FrameGen (in `nvidia-vfx\`, with NVIDIA's licence texts
in `nvidia-vfx\licenses` and the Visual C++ runtime DLLs NVIDIA's DLLs need). That's the usual
practice for apps using the SDK: NVIDIA's licence allows distributing it as part of an application,
not on its own, under terms at least as protective as NVIDIA's (see THIRD_PARTY_NOTICES.txt). NVIDIA
doesn't offer it as an anonymous download (NGC requires a free NVIDIA Developer sign-in), so the
app can't fetch it by itself. The repository doesn't contain it. To build an installer with VFG,
the release builder downloads the SDK from NGC (accepting NVIDIA's licence) and puts the files in
`third_party\nvvfx\runtime`.

FrameGen loads these files at run time:
`NVVideoEffects.dll`, `NVCVImage.dll`, `cudart64_12.dll` and `nvVFXVideoFrameGeneration.dll`
(either beside the others, or in the SDK's `features\nvvfxvideoframegeneration\bin` layout).

1. From NVIDIA NGC, download the **Video Effects SDK Core** for Windows
   (catalog.ngc.nvidia.com › maxine › vfx_sdk_core) and extract it.
2. In its `features` folder, install the Video Frame Generation feature:
   `.\install_feature.ps1 -features nvvfxvideoframegeneration`.
3. FrameGen looks in, in this order:
   - the folder chosen in **Settings › Frame Generation › NVIDIA VFG › Choose folder…**;
   - `nvidia-vfx\` next to `FrameGen.exe`;
   - `%NV_VIDEO_EFFECTS_PATH%` / `%NVVFX_SDK_PATH%` (and their `bin\`);
   - `%ProgramFiles%\NVIDIA Corporation\NVIDIA Video Effects`.

Every DLL must carry NVIDIA's Authenticode signature, or FrameGen refuses to load it.
The DLLs also need the Microsoft Visual C++ 2015–2022 runtime (normally already installed).

For development, anything in `third_party\nvvfx\runtime\` is copied to `build\nvidia-vfx\` by
the build (git-ignored, like `third_party\` as a whole), and `package.ps1` puts it in the installer.

## Settings

**VFG quality** (Settings › Frame Generation) picks NVIDIA's model:

| | 2× | 3× | 4× | 
|---|---|---|---|
| Fast (Low model) | 1.5 ms | 2.4 ms | 3.4 ms |
| Balanced (Medium, default) | 3.1 ms | 5.3 ms | 8.2 ms |
| Best quality (High) | 10.7 ms | 20.7 ms | 31.1 ms |

GPU time per real frame at 1080p on a high-end RTX 50 desktop GPU (`framegen-cli --bench`). Best quality at 4×
nearly fills a 30 fps game's 33 ms budget. If generation can't keep up, the automatic step-down
lowers the multiplier, then switches to Low latency.

Live (30 fps console game through a capture card, Balanced): 2× → 60 fps, ≈ 35 ms delay; 4× → 120 fps,
≈ 59 ms delay; no dropped frames.

## How it works

`src/VfgEngine.cpp` is an `Interpolator` like FrameGen's own engines, so the pipeline (repeat
detection, cadence, scheduling, HUD stabiliser, presenter) is unchanged:

- The SDK's C API is declared locally (struct layout and selector strings from NVIDIA's
  MIT-licensed headers, github.com/NVIDIA-Maxine/Maxine-VFX-SDK; copies in `third_party\nvvfx`).
  The DLLs are found, signature-checked and loaded once, on first use.
- Effect `"VideoFrameGeneration"`: `InputWidth` / `InputHeight` / `Mode` / CUDA stream before
  `NvVFX_Load`. Then `FrameMultiplier` = 0 (explicit timestep mode) and per generated frame
  `Timestep` = phase, `SrcImage0` = previous frame, `SrcImage1` = next frame, `DstImage0` = output.
  Automatic shot-change detection stays on.
- Frames stay on the GPU. The two real frames are copied into D3D11 textures registered with
  CUDA (`NvCVImage_InitFromD3D11Texture`), mapped onto the effect's CUDA stream and transferred
  into the effect's CUDA buffers. Each result goes back through a registered output texture.
  Mapping and unmapping order CUDA work after earlier D3D11 work, and D3D11 work after CUDA work.
  The D3D11 calls hold `Gpu::lock()`; CUDA work runs without it.
- Load takes ≈ 0.2 s (in the background while real frames keep showing). A load failure is
  reported once and the engine stays off instead of retrying every frame.

## Comparing engines on real gameplay

`framegen-cli --check-real <dir>` runs the engines on frame pairs saved by `--dump-live`
(`liveN_a_before.png` / `liveN_d_after.png`) and reports blockiness (luma steps on the 8 px block
grid; 1.00 = like the input) and each engine's PSNR against VFG's output. `FG_REAL_OUT=<dir>`
saves the results, `FG_ENGINES=fast,quality,vfg` picks engines, `FG_VFG_MODEL=low|medium|high`.
