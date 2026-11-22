// Test pattern (no hardware needed): a scene that stresses frame generation — a fast pan, a
// bouncing ball, a spinning bar and a static HUD — plus a simulation of the whole signal chain
// (game fps → HDMI signal Hz → capture card Hz) and of capture-card compression noise.
#pragma once
#include "Capture.h"

class SyntheticSource : public FrameSource {
public:
    SyntheticSource(double fps, int width, int height, int repeatCount = 1);
    ~SyntheticSource() override { stop(); }
    std::string name() override { return name_; }
    bool start(std::string& error) override;
    void stop() override;

    /// Draws content frame `index` at `fps` content rate (deterministic: also used by the test tools).
    /// Returns an NV12 image (BT.709 limited range, like a capture card).
    ImagePtr render(int index, double fps = 0);
    /// Same picture as RGBA8 (no noise) — ground truth for the quality tools.
    ImagePtr renderRGBA(int index, double fps = 0);

    std::string scene = "action";   // action | still | static | hud | stopgo
    bool noise = false;
    double gameFPS = 0, signalHz = 0;
    double startTime = 0;
    /// Where the "hud" scene's static text panel is (top-left origin, pixels).
    static D2D1_RECT_F hudPanel(float W, float H);

private:
    ImagePtr draw(int index, double fps, bool withNoise, bool toNV12);
    void run();
    double fps_; int w_, h_, repeat_;
    std::string name_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    ImagePool bgraPool_, rgbaPool_, nv12Pool_;
    ComPtr<ID2D1DeviceContext> dc_;
    ComPtr<IDWriteTextFormat> big_, small_;
    std::mutex drawLock_;
};
