#include "Interpolators.h"
#include "FlowEngine.h"
#include "Hud.h"
#include "DlssG.h"
#include "Prefs.h"
#include "VfgEngine.h"
#include "FsrEngine.h"

static const char* kBlend = R"(
Texture2D<float4> A : register(t0);
Texture2D<float4> B : register(t1);
RWTexture2D<unorm float4> O : register(u0);
cbuffer C : register(b0) { float t; float3 pad; };
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; O.GetDimensions(w, h); if (id.x >= w || id.y >= h) return;
    O[id.xy] = lerp(A[id.xy], B[id.xy], t);
}
)";

/// Linear cross-fade — baseline for comparison only (ghosting, not real motion).
class BlendInterpolator : public Interpolator {
public:
    std::string name() override { return "Blend"; }
    bool interpolate(const VideoFrame& prev, const VideoFrame& next, const std::vector<float>& phases,
                     std::vector<ImagePtr>& out, std::string& error) override {
        auto& gpu = Gpu::get();
        if (!prev.image || !next.image || prev.image->w != next.image->w || prev.image->h != next.image->h) { error = "blend: size mismatch"; return false; }
        auto l = gpu.lock();
        for (float t : phases) {
            auto o = pool_.acquire(next.image->w, next.image->h, DXGI_FORMAT_R8G8B8A8_UNORM);
            if (!o) { error = "blend: out of video memory"; return false; }
            struct { float t, pad[3]; } c{t, {}};
            gpu.dispatch(gpu.cs(kBlend), {prev.image->srv[0].Get(), next.image->srv[0].Get()}, {o->uav.Get()}, &c, sizeof c, o->w, o->h);
            out.push_back(o);
        }
        return true;
    }
private:
    ImagePool pool_;
};

std::unique_ptr<Interpolator> makeInterpolator(EngineKind kind, std::string& error) {
    switch (kind) {
    case EngineKind::Off: return nullptr;
    case EngineKind::Blend: return std::make_unique<BlendInterpolator>();
    case EngineKind::Fast: return std::make_unique<FlowEngine>(FlowEngine::Quality::Fast);
    case EngineKind::Quality: return std::make_unique<FlowEngine>(FlowEngine::Quality::High);
    case EngineKind::DlssG: return nullptr;   // DLSS-G generates frames in its presenter
    case EngineKind::Vfg: {
        if (!Vfg::support().supported) { error = Vfg::support().reason; return nullptr; }
        std::string m = Prefs::get().str("vfgModel", "medium");
        if (const char* e = getenv("FG_VFG_MODEL")) m = e;
        return std::make_unique<VfgEngine>(m == "low" ? VfgEngine::Model::Low : m == "high" ? VfgEngine::Model::High : VfgEngine::Model::Medium);
    }
    case EngineKind::Fsr:
        if (!Fsr::support().supported) { error = Fsr::support().reason; return nullptr; }
        return std::make_unique<FsrEngine>();
    }
    error = "unknown engine";
    return nullptr;
}

namespace EngineSupport {
// Both built-in engines run on any Direct3D 11 GPU; NVIDIA VFG / DLSS Frame Generation and AMD FSR
// need their runtimes and a capable GPU.
bool isSupported(EngineKind k) {
    if (k == EngineKind::DlssG) return DlssG::support().supported;
    if (k == EngineKind::Vfg) return Vfg::support().supported;
    if (k == EngineKind::Fsr) return Fsr::support().supported;
    return k == EngineKind::Off || Gpu::get().dev != nullptr;
}
bool any() { return Gpu::get().dev != nullptr; }
std::optional<EngineKind> best(EngineKind preferred) {
    if (preferred != EngineKind::Off && isSupported(preferred)) return preferred;
    if (isSupported(EngineKind::Fast)) return EngineKind::Fast;
    return std::nullopt;
}
std::string unsupportedReason() { return "Frame generation needs a Direct3D 11 graphics card."; }
std::string describe() {
    const auto& d = DlssG::support();
    std::string dl = d.supported ? strf("yes (up to %d×)", d.maxFramesToGenerate + 1) : ("no — " + d.reason);
    auto& g = Gpu::get();
    const auto& v = Vfg::support();
    std::string vfg = v.supported ? "yes (" + narrow(v.dir) + ")" : "no — " + v.reason;
    const auto& f = Fsr::support();
    std::string fsr = f.supported ? "yes, 2× (" + narrow(f.dir) + ")" : "no — " + f.reason;
    return strf("GPU: %s (%zu MB)%s\nLow latency engine (GPU flow): %s\nSmoothest engine (GPU flow, quality): %s\nTearing / VRR presentation: %s\nDLSS Frame Generation (NVIDIA): %s\nNVIDIA VFG (AI frame generation): %s\nAMD FSR Frame Generation: %s",
                g.adapterName.c_str(), g.vramMB, g.isNvidia ? " — NVIDIA" : "",
                isSupported(EngineKind::Fast) ? "yes" : "no", isSupported(EngineKind::Quality) ? "yes" : "no", g.tearingSupported ? "supported" : "not supported", dl.c_str(), vfg.c_str(), fsr.c_str());
}
}
