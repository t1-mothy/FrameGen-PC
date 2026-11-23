// Presentation. Two implementations share the Presenter interface:
//  * Renderer — Direct3D 11 flip-model swap chain on a dedicated high-priority thread (Lesson 11),
//    adaptive delay (Lesson 7 / §3.4), compare mode, Fit/Fill/Stretch and the UI overlays;
//  * DlssPresenter (DlssPresenter.h) — Direct3D 12 + NVIDIA Streamline, where DLSS Frame
//    Generation creates and paces the in-between frames.
#pragma once
#include "Pipeline.h"
#include <thread>

class Presenter {
public:
    Presenter(FramePipeline& pipeline, Stats& stats) : pipeline_(pipeline), stats_(stats) {}
    virtual ~Presenter() = default;
    virtual bool init(HWND hwnd, std::string& error) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual const char* kind() const = 0;
    void resize(int w, int h) { pendW_ = w; pendH_ = h; resizePending_ = true; }

    std::atomic<double> delay{0.06};        // smoothed presentation delay behind content time (s)
    std::atomic<double> extraDelay{0.002};  // safety margin
    std::atomic<bool> compare{false};
    std::atomic<double> maxHz{0};           // 0 = display maximum
    std::atomic<int> scaling{0};            // 0 fit, 1 fill, 2 stretch
    std::atomic<bool> vrr{false};           // present each frame when due (G-SYNC / FreeSync) instead of every refresh
    std::atomic<int> undrawable{0};
    std::atomic<int> notReady{0};     // refreshes skipped because the swap chain wasn't ready
    std::atomic<int> passedOver{0};   // frames never shown (the presenter fell behind)
    std::atomic<int> shownLate{0};    // frames shown more than 1.5 refreshes after they were due
    std::atomic<int> loops{0};        // presenter wake-ups (≈ refresh rate when keeping up)
    std::atomic<bool> timed{false};   // presenting on a timer (refresh signal unreliable)
    std::atomic<int> generate{1};           // DLSS presenter: frames to generate per real frame
    double displayHz();                     // refresh rate of the window's monitor
    std::vector<int> displayRates();        // refresh rates the monitor offers at its current resolution

    struct Overlay { ImagePtr image; int x = 0, y = 0; float alpha = 1; bool visible = false; };
    void setOverlay(int slot, const Overlay& o) { std::lock_guard<std::mutex> l(ovLock_); overlays_[slot] = o; ovVersion_++; }
    void requestShot(const std::string& path) { std::lock_guard<std::mutex> l(ovLock_); shotPath_ = path; }
    std::string lastError() { std::lock_guard<std::mutex> l(ovLock_); return error_; }

protected:
    FramePipeline& pipeline_;
    Stats& stats_;
    HWND hwnd_ = nullptr;
    std::atomic<bool> resizePending_{false};
    std::atomic<int> pendW_{0}, pendH_{0};
    std::mutex ovLock_;
    Overlay overlays_[2];
    std::string shotPath_, error_;
    std::atomic<int> ovVersion_{0};
    double cachedHz_ = 0, cachedHzAt_ = -10;
    std::vector<int> ratesCache_;
    std::wstring ratesMonitor_;
};

class Renderer : public Presenter {
public:
    using Presenter::Presenter;
    ~Renderer() override { stop(); }
    bool init(HWND hwnd, std::string& error) override;
    void start() override;
    void stop() override;
    const char* kind() const override { return "d3d11"; }

private:
    UINT frameLatency_ = 1;
    // Refresh-signal health (WaitForVBlank). Some setups (G-SYNC, a sleeping display, driver
    // quirks) deliver it irregularly or not at all: then present on a steady timer instead.
    double lastWake_ = 0, vblankWindowAt_ = 0;
    int vblankCount_ = 0, badSeconds_ = 0;
    bool timedMode_ = false;
    double behindSince_ = 0;
    void run();
    void drawFrame(const GpuImage& img, int dw, int dh, const D3D11_RECT* scissor);
    bool resizeBuffers();
    ComPtr<IDXGISwapChain2> swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    HANDLE waitable_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int bw_ = 0, bh_ = 0;
    ComPtr<ID3D11BlendState> premul_;
    ComPtr<ID3D11RasterizerState> rs_, rsScissor_;
    FramePtr lastShown_, lastDrawable_;
    double prof_[4] = {}, profAt_ = 0;
    ComPtr<IDXGIOutput> output_;
    double outputAt_ = -10;
    int lastOvVersion_ = -1, lastScaling_ = -1;
    bool lastCompare_ = false, justResized_ = true;
};
