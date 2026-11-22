// NVIDIA Video Frame Generation (VFG): the AI frame interpolation filter of NVIDIA's Video
// Effects SDK (Maxine). Ada / Blackwell (RTX 40 / 50) GPUs.
//
// The SDK runtime (NVVideoEffects.dll, NVCVImage.dll, cudart64_12.dll and the
// nvVFXVideoFrameGeneration feature) is NVIDIA's and is not part of FrameGen. It's loaded at run
// time from, in order: the folder chosen in Settings (pref "vfgRuntime"), <exe>\nvidia-vfx, the
// NVIDIA Video Effects SDK install (NV_VIDEO_EFFECTS_PATH / NVVFX_SDK_PATH, or
// %ProgramFiles%\NVIDIA Corporation\NVIDIA Video Effects). Every DLL must carry NVIDIA's
// Authenticode signature. See docs/VFG.md.
//
// Frames move between Direct3D 11 and CUDA through the SDK's own D3D11 interop
// (NvCVImage_InitFromD3D11Texture / MapResource), so they never leave the GPU.
#pragma once
#include "Interpolators.h"

namespace Vfg {
struct Support {
    bool supported = false;   // a capable GPU and the runtime found
    bool gpuOk = false;       // NVIDIA RTX 40 / 50 (Ada / Blackwell) or newer
    std::string reason;       // why not, in plain English (shown when the user picks VFG)
    std::wstring dir;         // runtime folder
};
extern const char* kGpuUnsupported;
const Support& support();                 // cached; cheap (no DLL is loaded)
void refresh();                           // forget the cached result (folder changed in Settings)
bool isRuntimeFolder(const std::wstring& dir);
}

class VfgEngine : public Interpolator {
public:
    enum class Model { Low = 0, Medium = 1, High = 2 };
    explicit VfgEngine(Model m);
    ~VfgEngine() override;
    std::string name() override { return "NVIDIA VFG"; }
    bool interpolate(const VideoFrame& prev, const VideoFrame& next, const std::vector<float>& phases,
                     std::vector<ImagePtr>& out, std::string& error) override;

private:
    struct State;
    bool ensure(int w, int h, size_t outputs, std::string& error);
    void release();
    Model model_;
    State* s_ = nullptr;
    std::string fatal_;   // a load error that won't fix itself (shown once, then Off)
    ImagePool outPool_;
};
