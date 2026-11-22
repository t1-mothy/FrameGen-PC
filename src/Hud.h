// HUD / text stabiliser (Lesson 10). Frame generation drags static on-screen UI along with the
// moving world behind it, so text "wobbles". UI is detected as edges that have stayed still for
// over half a second (noise-tolerant leaky counters), hidden from the engine (filled with the
// average of nearby *changing* pixels) and pasted back exactly afterwards. Port of the Mac
// app's Metal kernels (same thresholds, measured against ground truth).
#pragma once
#include "Gpu.h"

class HudStabilizer {
public:
    static HudStabilizer& get();
    void reset();   // forget what was still (new source, resolution change)

    /// Step 1 (before generation): updates the stillness map from the two real frames and, while
    /// the scene moves, returns copies with on-screen UI hidden. Returns the originals otherwise.
    std::pair<ImagePtr, ImagePtr> prepare(const ImagePtr& prev, const ImagePtr& next, double motion);
    /// Step 2 (after generation): replaces each output with a copy that has the exact UI from the
    /// real frames pasted back. Leaves `outs` unchanged when the picture is (nearly) still.
    void composite(std::vector<ImagePtr>& outs, const ImagePtr& prev, const ImagePtr& next, const std::vector<float>& phases, double motion);

    /// The wobble only happens while the world moves behind the UI (mean luma difference, 0–255).
    static float lockStrength(double motion);

    struct Snapshot { int w = 0, h = 0; std::vector<uint8_t> data; };
    void requestSnapshot() { wantSnapshot_ = true; }
    Snapshot takeSnapshot();

    static constexpr float kLockThreshold = 0.03f;    // colour difference below which a pixel is unchanged
    static constexpr float kOverlayEdge = 0.12f;      // luma gradient for a pixel to count as text / icon edge
    static constexpr float kOverlayMinFrames = 18;    // real frames an edge must stay still (≈ 0.6 s at 30 fps)

private:
    void ensure(int w, int h);
    std::mutex m_;
    ImagePtr persist_;   // R32_UINT: bits 0–7 still count, 8–15 still-edge count, 16 = moving now
    ImagePtr blocks_;    // R8: 8×8 blocks containing UI edges
    ImagePool pool_;
    std::atomic<bool> wantSnapshot_{false};
    Snapshot snap_;
};
