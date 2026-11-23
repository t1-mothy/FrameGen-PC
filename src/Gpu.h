// Direct3D 11 device shared by capture (Media Foundation), the pipeline, the presenter and the UI.
//
// Threading: the immediate context is protected by Media Foundation-compatible multithread
// protection, and every *sequence* of calls (bind state → dispatch/draw → unbind) from our own
// code is wrapped in Gpu::lock(), so threads never interleave inside one sequence. Never wait
// for the GPU while holding the lock (use waitFor()).
#pragma once
#include "Core.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <map>

struct GpuImage {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv[2];   // NV12: [0] = Y (R8), [1] = CbCr (R8G8). RGBA/BGRA: [0]
    ComPtr<ID3D11UnorderedAccessView> uav;     // RGBA8 / float formats only
    ComPtr<ID3D11RenderTargetView> rtv[2];     // NV12: per plane. RGBA/BGRA: [0]
    int w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool isNV12() const { return fmt == DXGI_FORMAT_NV12; }
    // Shared with the capture upload device (see Uploader).
    ComPtr<ID3D11Texture2D> upTex;               // the same texture opened on the upload device
    std::atomic<uint64_t> readFence{0};          // main-device fence value after its last read
};

/// Colour matrix of incoming YUV video.
struct YuvInfo { bool bt709 = true; bool fullRange = false; };

/// Recycles GPU images: an image is free again once no frame references it.
class ImagePool {
public:
    ImagePtr acquire(int w, int h, DXGI_FORMAT fmt);
    void clear() { std::lock_guard<std::mutex> l(m_); items_.clear(); }
private:
    std::mutex m_;
    std::vector<ImagePtr> items_;
};

class Gpu {
public:
    static Gpu& get();
    bool init();   // false if no usable D3D11 device

    ComPtr<ID3D11Device5> dev;
    ComPtr<ID3D11DeviceContext4> ctx;
    ComPtr<IDXGIFactory5> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IMFDXGIDeviceManager> mfManager;
    UINT mfResetToken = 0;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<IDWriteFactory> dwrite;
    std::string adapterName;
    LUID adapterLuid{};
    bool isNvidia = false;
    size_t vramMB = 0;
    bool tearingSupported = false;

    std::unique_lock<std::recursive_mutex> lock() { return std::unique_lock<std::recursive_mutex>(mtx_); }

    /// The graphics driver reset or removed the device (TDR, driver update/crash). Logged once
    /// with Windows' reason; the app then restarts itself (App::tick).
    static bool isDeviceLost(HRESULT hr) { return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG; }
    void reportDeviceLost(const char* where, HRESULT hr);
    std::atomic<bool> deviceLost{false};

    // Images
    ImagePtr makeImage(int w, int h, DXGI_FORMAT fmt);
    ImagePtr makeTexture(int w, int h, DXGI_FORMAT fmt, UINT bind);   // generic helper (no pool)

    // Shaders (compiled at runtime from HLSL source; cached by entry + source pointer)
    ID3D11ComputeShader* cs(const char* src, const char* entry = "main");
    ID3D11VertexShader* vs(const char* src, const char* entry = "main");
    ID3D11PixelShader* ps(const char* src, const char* entry = "main");

    /// Binds SRVs t0.., UAVs u0.., constant buffer b0, dispatches ceil(w/16) × ceil(h/16) groups
    /// (16×16 threads), then unbinds. Caller must hold lock().
    void dispatch(ID3D11ComputeShader* shader, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                  std::initializer_list<ID3D11UnorderedAccessView*> uavs, const void* cb, size_t cbSize,
                  UINT threadsX, UINT threadsY, UINT groupW = 16, UINT groupH = 16);
    void setCB(UINT slot, const void* data, size_t size, bool pixelStage = false);
    ID3D11SamplerState* linearClamp() { return linear_.Get(); }
    ID3D11SamplerState* pointClamp() { return point_.Get(); }

    /// Issues an event query (with flush) and waits for the GPU to reach it, without holding
    /// the lock while waiting. Returns ms waited.
    double waitIdle();

    // Colour conversion
    void nv12ToRgba(const GpuImage& src, const GpuImage& dst, const YuvInfo& yuv);
    void rgbaToNv12(const GpuImage& src, const GpuImage& dst);   // BT.709 limited range (test pattern)

    /// Sparse luma samples (every 8th pixel from (5, 3)) of an NV12 or RGBA image, read back to
    /// the CPU — the input of repeat detection. Waits for the GPU.
    bool sparseLuma(const GpuImage& src, std::vector<uint8_t>& out, int& cols, int& rows);

    /// CPU copy of an RGBA8/BGRA8 image (tightly packed RGBA). NV12 images are converted first.
    bool readRGBA(const GpuImage& img, std::vector<uint8_t>& out, const YuvInfo& yuv = {});
    /// CPU copy of an NV12 image's luma plane.
    bool readLuma(const GpuImage& img, std::vector<uint8_t>& out);
    bool savePNG(const GpuImage& img, const std::string& path, const YuvInfo& yuv = {});
    static bool savePNG(const uint8_t* rgba, int w, int h, const std::string& path, bool gray = false);
    ImagePtr loadPNG(const std::string& path);   // RGBA8 image (tests)

private:
    std::recursive_mutex mtx_;
    ComPtr<ID3D11SamplerState> linear_, point_;
    ComPtr<ID3D11Buffer> cbs_[4];
    std::map<std::pair<const void*, std::string>, ComPtr<ID3D11DeviceChild>> shaders_;
    ComPtr<ID3DBlob> compile(const char* src, const char* entry, const char* target);
    ComPtr<ID3D11Texture2D> lumaTex_, lumaStage_;
    int lumaCols_ = 0, lumaRows_ = 0;
    ComPtr<ID3D11Fence> fence_;
    UINT64 fenceValue_ = 0;
};

/// Uploads capture-card frames (system memory) through a *separate* D3D11 device, so a busy GPU
/// can never make the capture thread hold the main device's lock: copies happen on the upload
/// device, the main device waits for them on the GPU (shared fence), and the upload device waits
/// on the GPU for the main device to finish reading a texture before reusing it.
class Uploader {
public:
    bool init();
    /// Copies an NV12 frame (Y plane then CbCr plane, `pitch` bytes per row) into a shared
    /// texture. Returns the image and the upload fence value the main device must wait for.
    ImagePtr upload(const uint8_t* y, const uint8_t* uv, int pitch, int w, int h, uint64_t& fenceValue);
    /// Main device (caller holds Gpu::lock): wait on the GPU for an upload.
    void waitOnMain(uint64_t value);
    /// Main device (caller holds Gpu::lock): mark that `img` has been read up to this point.
    void markRead(GpuImage& img);
    bool ready() const { return dev_ != nullptr; }
private:
    ImagePtr acquire(int w, int h);
    ComPtr<ID3D11Device5> dev_;
    ComPtr<ID3D11DeviceContext4> ctx_;
    ComPtr<ID3D11Fence> upOnUp_, upOnMain_;       // signalled by the upload device
    ComPtr<ID3D11Fence> readOnMain_, readOnUp_;   // signalled by the main device
    uint64_t upValue_ = 0;
    std::atomic<uint64_t> readValue_{0};
    std::vector<ImagePtr> ring_;
    ComPtr<ID3D11Texture2D> stage_[4];
    int stageNext_ = 0, stageW_ = 0, stageH_ = 0;
    std::mutex m_;
};

/// GPU timestamp profiler for the test tools (FG_PROFILE=1): time between consecutive marks.
class GpuProfiler {
public:
    static GpuProfiler& get();
    bool enabled = false;
    void begin();                 // call before a measured block (takes the GPU lock itself)
    void mark(const char* name);  // time since the previous mark is attributed to `name`
    void end();                   // waits for the results and accumulates them
    void report();                // prints averages and resets
private:
    struct Entry { std::string name; ComPtr<ID3D11Query> q; };
    ComPtr<ID3D11Query> disjoint_, start_;
    std::vector<Entry> marks_;
    std::vector<std::pair<std::string, std::pair<double, int>>> totals_;
    bool active_ = false;
};

/// Full-screen triangle vertex shader (SV_VertexID 0..2), outputs uv in [0,1].
extern const char* kFullscreenVS;
