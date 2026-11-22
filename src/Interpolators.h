// Frame generation engines and which ones this PC can run.
#pragma once
#include "Core.h"
#include "Gpu.h"

class Interpolator {
public:
    virtual ~Interpolator() = default;
    virtual std::string name() = 0;
    /// Produces one RGBA8 image per phase (each strictly between 0 and 1) between `prev` and
    /// `next` (RGBA8 frames). Called on the pipeline thread; the GPU work may still be running
    /// on return (the pipeline waits for it).
    virtual bool interpolate(const VideoFrame& prev, const VideoFrame& next, const std::vector<float>& phases,
                             std::vector<ImagePtr>& out, std::string& error) = 0;
    /// Live-test aid: when set, the next call keeps a copy of the raw engine output (before UI
    /// stabilisation) in rawCopy.
    bool wantRaw = false;
    ImagePtr rawCopy;
};

std::unique_ptr<Interpolator> makeInterpolator(EngineKind kind, std::string& error);

/// Engine availability (the Windows counterpart of the Mac app's EngineSupport table).
namespace EngineSupport {
bool isSupported(EngineKind k);
bool any();
std::optional<EngineKind> best(EngineKind preferred);
std::string unsupportedReason();
std::string describe();   // for --list-devices / --bench
}
