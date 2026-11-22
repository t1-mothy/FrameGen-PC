// DLSS Frame Generation presenter (Direct3D 12 + NVIDIA Streamline).
//
// Direct3D 11 still does all the image work: each real frame is composed (scaling, dashboard) into
// a window-sized texture shared with Direct3D 12, together with a motion-vector image built from
// FrameGen's own motion search (a capture card has no game motion vectors) and a flat depth.
// Direct3D 12 copies the picture into Streamline's proxy swap chain, tags the inputs, sets the
// Reflex/PCL markers and presents; DLSS-G then generates and paces the in-between frames.
// Real frames are presented at their (cadence-smoothed) content time plus an adaptive delay.
#pragma once
#include "Renderer.h"
#include "DlssG.h"

#ifdef FG_HAVE_STREAMLINE
class DlssPresenter : public Presenter {
public:
    using Presenter::Presenter;
    ~DlssPresenter() override;
    bool init(HWND hwnd, std::string& error) override;
    void start() override;
    void stop() override;
    const char* kind() const override { return "dlss"; }

private:
    struct Slot {
        ComPtr<ID3D12Resource> color12, mvec12;
        ComPtr<ID3D12Resource> mvecOwned;   // plain D3D12 copy of mvec12 — what gets tagged
        ComPtr<ID3D11Texture2D> color11, mvec11;
        ComPtr<ID3D11RenderTargetView> colorRtv, mvecRtv;
        uint64_t doneValue = 0;   // D3D12 fence value once the copy / tag of this slot is finished
    };
    void run();
    bool createTargets(int w, int h, std::string& error);
    void releaseTargets();
    void compose(Slot& s, const FramePtr& frame);
    bool presentSlot(Slot& s, bool reset);
    void setMode(bool on);
    int wantedGenerate() const;   // frames to generate, within this GPU's limit
    void waitGpu12();
    void dumpDred();

    DlssG::Runtime* rt_ = nullptr;
    ComPtr<IDXGISwapChain3> swap_;          // Streamline proxy
    ComPtr<ID3D12CommandAllocator> alloc_[3];
    uint64_t allocDone_[3] = {};
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> done12_, ready12_;  // done: D3D12 → D3D11, ready: D3D11 → D3D12
    ComPtr<ID3D11Fence> done11_, ready11_;
    uint64_t doneValue_ = 0, readyValue_ = 0;
    HANDLE fenceEvent_ = nullptr;
    std::vector<ComPtr<ID3D12Resource>> back_;
    ComPtr<ID3D12Resource> depth_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    Slot slots_[3];
    int slot_ = 0, bw_ = 0, bh_ = 0, allocIdx_ = 0;
    uint32_t frameIndex_ = 0;
    int optionsGenerate_ = -1;
    bool modeOn_ = false;
    std::thread thread_;
    std::atomic<bool> running_{false};
    FramePtr last_;
    double lastPresent_ = 0;
    ComPtr<ID3D11BlendState> premul_;
    ComPtr<ID3D11RasterizerState> rs_;
};
#endif
