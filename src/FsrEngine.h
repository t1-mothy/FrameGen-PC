// AMD FSR Frame Generation (AMD FidelityFX SDK: amd_fidelityfx_loader_dx12.dll +
// amd_fidelityfx_framegeneration_dx12.dll, AMD-signed, MIT-style licence). Runs on any
// DirectX 12 graphics card with Shader Model 6.2 — AMD, NVIDIA (incl. RTX 20/30) and Intel.
//
// FSR makes one frame halfway between two real ones, so this engine is 2× only. It's built for
// games, which hand it motion vectors and depth; a capture card has neither, so the motion
// vectors come from FrameGen's own motion search (FlowEngine::motionField, Smoothest's search
// without its "flat areas stay still" bias) and depth is one flat plane. FrameGen renders with Direct3D 11, FSR needs Direct3D 12: frames move between the two
// through shared textures and a shared fence (GPU-side waits only, nothing waits on the CPU while
// holding Gpu::lock()).
//
// The DLLs are loaded at run time from <exe>\amd-fsr (third_party\fidelityfx\bin when building,
// see docs/DEVELOPMENT.md) and must carry AMD's Authenticode signature.
#pragma once
#include "Interpolators.h"

class FlowEngine;

namespace Fsr {
struct Support {
    bool supported = false;   // the DLLs are there and the GPU has Direct3D 12
    std::string reason;       // why not, in plain English
    std::wstring dir;
};
extern const char* kGpuUnsupported;
const Support& support();    // cached; cheap (no DLL is loaded)
}

class FsrEngine : public Interpolator {
public:
    FsrEngine();
    ~FsrEngine() override;
    std::string name() override { return "AMD FSR"; }
    bool interpolate(const VideoFrame& prev, const VideoFrame& next, const std::vector<float>& phases,
                     std::vector<ImagePtr>& out, std::string& error) override;

private:
    struct State;
    bool ensure(int w, int h, std::string& error);
    /// Hands FSR the next real frame; with `generate`, FSR's frame halfway from the previous one
    /// ends up in the shared output texture.
    bool feed(const ImagePtr& color, const ImagePtr& field, float fieldScale, bool reset, double dtMs, std::string& error);
    void release();
    State* s_ = nullptr;
    std::unique_ptr<FlowEngine> flow_;   // motion vectors for FSR
    std::string fatal_;                  // an error that won't fix itself (shown once, then Off)
    ImagePool outPool_;
};
