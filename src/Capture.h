// Frame sources: the capture card (Media Foundation) and the interface shared with the test pattern.
#pragma once
#include "Core.h"
#include "Gpu.h"
#include <mfidl.h>
#include <mfreadwrite.h>
#include <thread>

class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual std::string name() = 0;
    /// Called on the source's own thread for every frame (NV12 image, host-clock time).
    std::function<void(FramePtr)> onFrame;
    /// Optional: decides before the GPU upload whether a frame is new (repeat detection on the CPU
    /// samples). Repeats are then never uploaded — 3 of 4 frames for a 30 fps game captured at 120 Hz.
    std::function<bool(VideoFrame&)> admit;
    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;
    YuvInfo yuv;   // colour matrix of the delivered NV12 frames
};

struct CaptureFormat {
    int w = 0, h = 0;
    GUID subtype{};
    std::string fourcc;
    UINT32 fpsNum = 0, fpsDen = 1;
    double fps() const { return fpsDen ? double(fpsNum) / fpsDen : 0; }
    DWORD index = 0;   // media type index on the device
};

struct CaptureDevice {
    std::string name;
    std::wstring symlink;
    std::vector<CaptureFormat> formats;
};

class CaptureSource : public FrameSource {
public:
    CaptureSource(std::string deviceQuery, int width, int height, double fps);
    ~CaptureSource() override { stop(); if (exited_ && !leaked_) CloseHandle(exited_); }
    std::string name() override { std::lock_guard<std::mutex> l(m_); return name_; }
    bool start(std::string& error) override;
    void stop() override;

    /// Video capture devices with their formats (cached — enumerating is slow; refresh after hot-plug).
    static std::vector<CaptureDevice> devices(bool refresh = false);
    static std::string describeDevices();
    /// Picks the card's format (see Lessons 4–5): requested size (default 1920 × 1080), in Auto the
    /// fastest rate ≤ 120 Hz, native uncompressed YUV preferred, high-speed modes avoided.
    static int pickFormat(const CaptureDevice& d, int wantW, int wantH, double wantFPS);
    static constexpr double kAutoMaxRate = 120.0;

    double activeRate = 0;
    int activeW = 0, activeH = 0;
    std::string activeFourcc;
    bool isAutoRate() const { return wantFPS_ <= 0; }
    std::string deviceName;
    bool permissionDenied = false;
    Stats* stats = nullptr;

private:
    void run();
    bool deliver(IMFSample* sample, LONGLONG ts);
    std::string query_;
    int wantW_, wantH_;
    double wantFPS_;
    std::mutex m_;
    std::string name_ = "Capture";
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    ImagePool pool_;
    ComPtr<ID3D11Texture2D> stage_[4];   // system-memory upload ring
    int stageNext_ = 0, stageW_ = 0, stageH_ = 0;
    bool useD3D_ = false;
public:
    static Uploader uploader_;   // separate D3D11 device for card uploads
private:
    bool uploaderFailed_ = false;
    HANDLE exited_ = nullptr;   // set when the capture thread has exited
    bool leaked_ = false;
    double offset_ = 0; bool haveOffset_ = false; int offsetFrames_ = 0;
    int64_t frames_ = 0;
};
