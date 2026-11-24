// FrameGen's own GPU frame interpolation (Direct3D 11 compute shaders, any DX11 GPU).
//
// A capture card only gives finished pictures, so motion is estimated from the video:
//   1. luma pyramids of both real frames (full, ½, ¼, ⅛, 1/16 resolution);
//   2. hierarchical *symmetric* block motion search at the in-between time t — for each 8×8
//      block find v so that A(p − t·v) matches B(p + (1−t)·v) — coarse to fine, with candidates
//      from the coarser level, the neighbours (propagation) and the previous frame pair, plus
//      sub-pixel refinement. Searching at time t gives a motion field with no holes;
//   3. per pixel, pick the best of the nearby block vectors (sharp motion boundaries), fetch both
//      real frames along that vector and blend; where the two disagree (occlusion), favour the
//      frame closer in time instead of showing a double image.
// "Low latency" estimates one motion field (at t = ½) and reuses it for every phase; "Smoothest"
// searches separately for each phase and refines down to full resolution.
#pragma once
#include "Interpolators.h"

class FlowEngine : public Interpolator {
public:
    enum class Quality { Fast, High };
    explicit FlowEngine(Quality q) : q_(q) {}
    std::string name() override { return q_ == Quality::Fast ? "GPU flow (low latency)" : "GPU flow (smoothest)"; }
    bool interpolate(const VideoFrame& prev, const VideoFrame& next, const std::vector<float>& phases,
                     std::vector<ImagePtr>& out, std::string& error) override;
    /// Motion from `prev` to `next`, located at `next`'s pixels (input for DLSS Frame Generation):
    /// a block field (RG16F, values in field pixels; multiply by `scale` for video pixels).
    ImagePtr motionField(const VideoFrame& prev, const VideoFrame& next, float& scale);
    /// Penalty on vector length per full-res pixel (default 0.002): keeps flat areas (plain sky)
    /// still, which suits FrameGen's own interpolation. Engines that take motion vectors as
    /// "true" motion (AMD FSR) need 0: there a still sky next to panning buildings tears the edges.
    void setLengthPenalty(float mu) { if (!getenv("FG_MU")) muFull_ = mu; }

private:
    struct Level {
        int w = 0, h = 0, bw = 0, bh = 0;
        ImagePtr lumaA, lumaB;     // R16F
        ImagePtr field[2];         // R16G16F block vectors (level pixels), ping-pong
        ImagePtr history;          // final field of the previous pair (temporal candidate)
        bool hasHistory = false;
    };
    bool ensure(int w, int h);
    void buildPyramid(const GpuImage& rgba, bool isA);
    int estimate(float t, int finest);   // returns index of the field holding the result at `finest`
    Quality q_;
    float muFull_ = getenv("FG_MU") ? float(atof(getenv("FG_MU"))) : 0.002f;   // vector length penalty per full-res pixel
    float muCapFull_ = getenv("FG_MUCAP") ? float(atof(getenv("FG_MUCAP"))) : 48.f;   // full-res pixels
    int w_ = 0, h_ = 0;
    float sigma_ = getenv("FG_SIGMA") ? float(atof(getenv("FG_SIGMA"))) : 0.02f;   // soft vector choice (0 = hard)
    std::vector<Level> levels_;
    ImagePtr glob_;   // 4 dominant motions (R32G32F, full-res pixels)
    int fullLevels_ = getenv("FG_FULLLEVELS") ? atoi(getenv("FG_FULLLEVELS")) : 0;   // extra exhaustive levels below the coarsest
    ImagePool outPool_, fieldPool_;
};
