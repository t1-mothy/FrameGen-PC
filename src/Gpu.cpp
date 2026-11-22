#include "Gpu.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <algorithm>

const char* kFullscreenVS = R"(
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO main(uint id : SV_VertexID) {
    VO o; float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
)";

// YUV (NV12) → RGBA8. rgb = M · (yuv − off)
static const char* kNV12ToRGBA = R"(
Texture2D<float> Y : register(t0);
Texture2D<float2> UV : register(t1);
RWTexture2D<unorm float4> O : register(u0);
SamplerState S : register(s0);
cbuffer C : register(b0) { float4 r0, r1, r2, off; float2 invSize; float2 pad; };
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; O.GetDimensions(w, h); if (id.x >= w || id.y >= h) return;
    float y = Y[id.xy];
    float2 c = UV.SampleLevel(S, (float2(id.xy) + 0.5) * invSize, 0);
    float3 v = float3(y, c) - off.xyz;
    O[id.xy] = float4(saturate(float3(dot(r0.xyz, v), dot(r1.xyz, v), dot(r2.xyz, v))), 1);
}
)";

// RGBA → NV12 planes (BT.709 limited range), rendered into the plane render targets.
static const char* kRGBAToNV12 = R"(
Texture2D<float4> T : register(t0);
SamplerState S : register(s0);
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float psY(VO i) : SV_Target { return 16.0/255 + (219.0/255) * luma(saturate(T.SampleLevel(S, i.uv, 0).rgb)); }
float2 psUV(VO i) : SV_Target {
    float3 c = saturate(T.SampleLevel(S, i.uv, 0).rgb);   // linear filter at the 2×2 centre = average
    float l = luma(c);
    return float2(128.0/255 + (224.0/255) * (c.b - l) / 1.8556, 128.0/255 + (224.0/255) * (c.r - l) / 1.5748);
}
)";

// Sparse luma samples for repeat detection: every 8th pixel starting at (5, 3).
static const char* kSparseLuma = R"(
Texture2D<float4> T : register(t0);
RWTexture2D<unorm float> O : register(u0);
cbuffer C : register(b0) { uint isRGB; uint3 pad; };
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; O.GetDimensions(w, h); if (id.x >= w || id.y >= h) return;
    float4 v = T.Load(int3(5 + id.x * 8, 3 + id.y * 8, 0));
    O[id.xy] = isRGB ? 16.0/255 + (219.0/255) * dot(v.rgb, float3(0.2126, 0.7152, 0.0722)) : v.r;
}
)";

Gpu& Gpu::get() { static Gpu g; return g; }

bool Gpu::init() {
    if (dev) return true;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { logf("[gpu] CreateDXGIFactory2 failed: %s", hr2str(hr).c_str()); return false; }
    ComPtr<IDXGIFactory6> f6;
    if (SUCCEEDED(factory.As(&f6))) f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (!adapter) factory->EnumAdapters1(0, &adapter);
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) {
        adapter->GetDesc1(&ad);
        adapterName = narrow(ad.Description);
        adapterLuid = ad.AdapterLuid;
        isNvidia = ad.VendorId == 0x10DE;
        vramMB = ad.DedicatedVideoMemory / (1024 * 1024);
    }
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c;
    hr = D3D11CreateDevice(adapter.Get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                           levels, 2, D3D11_SDK_VERSION, &d, nullptr, &c);
    if (FAILED(hr)) { logf("[gpu] D3D11CreateDevice failed: %s", hr2str(hr).c_str()); return false; }
    if (FAILED(d.As(&dev)) || FAILED(c.As(&ctx))) { logf("[gpu] Direct3D 11.4 not available (Windows 10 1703+ needed)"); return false; }
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);

    BOOL tearing = FALSE;
    factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
    tearingSupported = tearing;

    hr = MFCreateDXGIDeviceManager(&mfResetToken, &mfManager);
    if (SUCCEEDED(hr)) hr = mfManager->ResetDevice(dev.Get(), mfResetToken);
    if (FAILED(hr)) { logf("[gpu] MF device manager failed: %s", hr2str(hr).c_str()); mfManager.Reset(); }

    D2D1_FACTORY_OPTIONS fo{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)d2dFactory.GetAddressOf());
    ComPtr<IDXGIDevice> dxgiDev; dev.As(&dxgiDev);
    if (d2dFactory) d2dFactory->CreateDevice(dxgiDev.Get(), &d2dDevice);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dwrite.GetAddressOf());

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &linear_);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    dev->CreateSamplerState(&sd, &point_);
    for (auto& b : cbs_) {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 512; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&bd, nullptr, &b);
    }
    return true;
}

ComPtr<ID3DBlob> Gpu::compile(const char* src, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(src, strlen(src), entry, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        logf("[gpu] shader compile failed (%s %s): %s", entry, target, err ? (const char*)err->GetBufferPointer() : hr2str(hr).c_str());
        MessageBoxA(nullptr, err ? (const char*)err->GetBufferPointer() : "shader compile failed", "FrameGen — shader error", MB_ICONERROR);
        ExitProcess(3);
    }
    return blob;
}

ID3D11ComputeShader* Gpu::cs(const char* src, const char* entry) {
    auto l = lock();
    auto key = std::make_pair((const void*)src, std::string("cs:") + entry);
    auto it = shaders_.find(key);
    if (it != shaders_.end()) return static_cast<ID3D11ComputeShader*>(it->second.Get());
    auto blob = compile(src, entry, "cs_5_0");
    ComPtr<ID3D11ComputeShader> s;
    dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s);
    shaders_[key] = s;
    return s.Get();
}
ID3D11VertexShader* Gpu::vs(const char* src, const char* entry) {
    auto l = lock();
    auto key = std::make_pair((const void*)src, std::string("vs:") + entry);
    auto it = shaders_.find(key);
    if (it != shaders_.end()) return static_cast<ID3D11VertexShader*>(it->second.Get());
    auto blob = compile(src, entry, "vs_5_0");
    ComPtr<ID3D11VertexShader> s;
    dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s);
    shaders_[key] = s;
    return s.Get();
}
ID3D11PixelShader* Gpu::ps(const char* src, const char* entry) {
    auto l = lock();
    auto key = std::make_pair((const void*)src, std::string("ps:") + entry);
    auto it = shaders_.find(key);
    if (it != shaders_.end()) return static_cast<ID3D11PixelShader*>(it->second.Get());
    auto blob = compile(src, entry, "ps_5_0");
    ComPtr<ID3D11PixelShader> s;
    dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s);
    shaders_[key] = s;
    return s.Get();
}

ImagePtr Gpu::makeTexture(int w, int h, DXGI_FORMAT fmt, UINT bind) {
    auto img = std::make_shared<GpuImage>();
    img->w = w; img->h = h; img->fmt = fmt;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = bind;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &img->tex);
    if (FAILED(hr)) { logf("[gpu] CreateTexture2D %dx%d fmt %d failed: %s", w, h, int(fmt), hr2str(hr).c_str()); return nullptr; }
    if (fmt == DXGI_FORMAT_NV12) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R8_UNORM; dev->CreateShaderResourceView(img->tex.Get(), &sd, &img->srv[0]);
        sd.Format = DXGI_FORMAT_R8G8_UNORM; dev->CreateShaderResourceView(img->tex.Get(), &sd, &img->srv[1]);
        if (bind & D3D11_BIND_RENDER_TARGET) {
            D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rd.Format = DXGI_FORMAT_R8_UNORM; dev->CreateRenderTargetView(img->tex.Get(), &rd, &img->rtv[0]);
            rd.Format = DXGI_FORMAT_R8G8_UNORM; dev->CreateRenderTargetView(img->tex.Get(), &rd, &img->rtv[1]);
        }
    } else {
        if (bind & D3D11_BIND_SHADER_RESOURCE) dev->CreateShaderResourceView(img->tex.Get(), nullptr, &img->srv[0]);
        if (bind & D3D11_BIND_UNORDERED_ACCESS) dev->CreateUnorderedAccessView(img->tex.Get(), nullptr, &img->uav);
        if (bind & D3D11_BIND_RENDER_TARGET) dev->CreateRenderTargetView(img->tex.Get(), nullptr, &img->rtv[0]);
    }
    return img;
}

ImagePtr Gpu::makeImage(int w, int h, DXGI_FORMAT fmt) {
    UINT bind = D3D11_BIND_SHADER_RESOURCE;
    switch (fmt) {
    case DXGI_FORMAT_NV12: bind |= D3D11_BIND_RENDER_TARGET; break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: bind |= D3D11_BIND_RENDER_TARGET; break;
    default: bind |= D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET; break;
    }
    return makeTexture(w, h, fmt, bind);
}

ImagePtr ImagePool::acquire(int w, int h, DXGI_FORMAT fmt) {
    std::lock_guard<std::mutex> l(m_);
    for (auto& it : items_)
        if (it.use_count() == 1 && it->w == w && it->h == h && it->fmt == fmt) return it;
    // Forget free images of other sizes (resolution changed).
    items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const ImagePtr& p) {
        return p.use_count() == 1 && (p->w != w || p->h != h || p->fmt != fmt); }), items_.end());
    auto img = Gpu::get().makeImage(w, h, fmt);
    if (img) items_.push_back(img);
    return img;
}

void Gpu::setCB(UINT slot, const void* data, size_t size, bool pixelStage) {
    if (slot >= 4 || size > 512) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(cbs_[slot].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, size);
        ctx->Unmap(cbs_[slot].Get(), 0);
    }
    ID3D11Buffer* b = cbs_[slot].Get();
    if (pixelStage) { ctx->PSSetConstantBuffers(slot, 1, &b); ctx->VSSetConstantBuffers(slot, 1, &b); }
    else ctx->CSSetConstantBuffers(slot, 1, &b);
}

void Gpu::dispatch(ID3D11ComputeShader* shader, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                   std::initializer_list<ID3D11UnorderedAccessView*> uavs, const void* cb, size_t cbSize,
                   UINT tx, UINT ty, UINT gw, UINT gh) {
    ID3D11ShaderResourceView* s[16] = {};
    ID3D11UnorderedAccessView* u[8] = {};
    UINT ns = 0, nu = 0;
    for (auto* v : srvs) if (ns < 16) s[ns++] = v;
    for (auto* v : uavs) if (nu < 8) u[nu++] = v;
    ctx->CSSetShader(shader, nullptr, 0);
    if (ns) ctx->CSSetShaderResources(0, ns, s);
    if (nu) ctx->CSSetUnorderedAccessViews(0, nu, u, nullptr);
    if (cb) setCB(0, cb, cbSize);
    ID3D11SamplerState* samp[2] = {linear_.Get(), point_.Get()};
    ctx->CSSetSamplers(0, 2, samp);
    ctx->Dispatch((tx + gw - 1) / gw, (ty + gh - 1) / gh, 1);
    ID3D11ShaderResourceView* ns0[16] = {};
    ID3D11UnorderedAccessView* nu0[8] = {};
    if (ns) ctx->CSSetShaderResources(0, ns, ns0);
    if (nu) ctx->CSSetUnorderedAccessViews(0, nu, nu0, nullptr);
}

double Gpu::waitIdle() {
    double t0 = hostNow();
    // Fence + event: wakes exactly when the GPU reaches this point (no polling, no 1 ms sleeps).
    static thread_local HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 value;
    {
        auto l = lock();
        if (!fence_) dev->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (!fence_) return 0;
        value = ++fenceValue_;
        double ts = hostNow();
        ctx->Signal(fence_.Get(), value);
        ctx->Flush();
        if (getenv("FG_TRACE") && hostNow() - ts > 0.004) logf("[trace] Signal+Flush blocked %.1f ms on the CPU", (hostNow() - ts) * 1000);
    }
    if (getenv("FG_TRACE") && hostNow() - t0 > 0.004) logf("[trace] waitIdle: %.1f ms before the fence was even submitted", (hostNow() - t0) * 1000);
    if (fence_->GetCompletedValue() < value) {
        fence_->SetEventOnCompletion(value, ev);
        if (WaitForSingleObject(ev, 2000) == WAIT_TIMEOUT) logf("[gpu] waitIdle timed out");
    }
    return (hostNow() - t0) * 1000;
}

void Gpu::nv12ToRgba(const GpuImage& src, const GpuImage& dst, const YuvInfo& yuv) {
    struct { float r0[4], r1[4], r2[4], off[4]; float inv[2], pad[2]; } c{};
    float ys = yuv.fullRange ? 1.f : 255.f / 219.f, cs_ = yuv.fullRange ? 1.f : 255.f / 224.f;
    if (yuv.bt709) {
        float r0[4] = {ys, 0, 1.5748f * cs_, 0}, r1[4] = {ys, -0.1873f * cs_, -0.4681f * cs_, 0}, r2[4] = {ys, 1.8556f * cs_, 0, 0};
        memcpy(c.r0, r0, 16); memcpy(c.r1, r1, 16); memcpy(c.r2, r2, 16);
    } else {
        float r0[4] = {ys, 0, 1.402f * cs_, 0}, r1[4] = {ys, -0.344136f * cs_, -0.714136f * cs_, 0}, r2[4] = {ys, 1.772f * cs_, 0, 0};
        memcpy(c.r0, r0, 16); memcpy(c.r1, r1, 16); memcpy(c.r2, r2, 16);
    }
    c.off[0] = yuv.fullRange ? 0.f : 16.f / 255.f; c.off[1] = c.off[2] = 128.f / 255.f;
    c.inv[0] = 1.f / src.w; c.inv[1] = 1.f / src.h;
    auto l = lock();
    dispatch(cs(kNV12ToRGBA), {src.srv[0].Get(), src.srv[1].Get()}, {dst.uav.Get()}, &c, sizeof c, dst.w, dst.h);
}

void Gpu::rgbaToNv12(const GpuImage& src, const GpuImage& dst) {
    auto l = lock();
    static ComPtr<ID3D11RasterizerState> rs;
    if (!rs) { D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; dev->CreateRasterizerState(&rd, &rs); }
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs(kFullscreenVS), nullptr, 0);
    ctx->RSSetState(rs.Get());
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ID3D11ShaderResourceView* s = src.srv[0].Get();
    ctx->PSSetShaderResources(0, 1, &s);
    ID3D11SamplerState* samp = linear_.Get();
    ctx->PSSetSamplers(0, 1, &samp);
    for (int p = 0; p < 2; p++) {
        D3D11_VIEWPORT vp{0, 0, float(p ? dst.w / 2 : dst.w), float(p ? dst.h / 2 : dst.h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rtv = dst.rtv[p].Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->PSSetShader(ps(kRGBAToNV12, p ? "psUV" : "psY"), nullptr, 0);
        ctx->Draw(3, 0);
    }
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* n = nullptr;
    ctx->PSSetShaderResources(0, 1, &n);
}

bool Gpu::sparseLuma(const GpuImage& src, std::vector<uint8_t>& out, int& cols, int& rows) {
    static std::mutex m; std::lock_guard<std::mutex> g(m);
    cols = (src.w - 5 + 7) / 8; rows = (src.h - 3 + 7) / 8;
    if (cols <= 0 || rows <= 0) return false;
    {
        auto l = lock();
        if (!lumaTex_ || lumaCols_ != cols || lumaRows_ != rows) {
            D3D11_TEXTURE2D_DESC td{}; td.Width = cols; td.Height = rows; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            lumaTex_.Reset(); lumaStage_.Reset();
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &lumaTex_))) return false;
            td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateTexture2D(&td, nullptr, &lumaStage_))) return false;
            lumaCols_ = cols; lumaRows_ = rows;
        }
        ComPtr<ID3D11UnorderedAccessView> uav;
        dev->CreateUnorderedAccessView(lumaTex_.Get(), nullptr, &uav);
        struct { UINT isRGB, pad[3]; } c{src.isNV12() ? 0u : 1u, {}};
        dispatch(cs(kSparseLuma), {src.srv[0].Get()}, {uav.Get()}, &c, sizeof c, cols, rows);
        ctx->CopyResource(lumaStage_.Get(), lumaTex_.Get());
    }
    waitIdle();
    auto l = lock();
    D3D11_MAPPED_SUBRESOURCE m2;
    if (FAILED(ctx->Map(lumaStage_.Get(), 0, D3D11_MAP_READ, 0, &m2))) return false;
    out.resize(size_t(cols) * rows);
    for (int r = 0; r < rows; r++) memcpy(&out[size_t(r) * cols], (uint8_t*)m2.pData + size_t(r) * m2.RowPitch, cols);
    ctx->Unmap(lumaStage_.Get(), 0);
    return true;
}

bool Gpu::readRGBA(const GpuImage& img, std::vector<uint8_t>& out, const YuvInfo& yuv) {
    const GpuImage* src = &img;
    ImagePtr tmp;
    if (img.isNV12()) {
        tmp = makeImage(img.w, img.h, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!tmp) return false;
        nv12ToRgba(img, *tmp, yuv);
        src = tmp.get();
    }
    ComPtr<ID3D11Texture2D> stage;
    D3D11_TEXTURE2D_DESC td; src->tex->GetDesc(&td);
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &stage))) return false;
    { auto l = lock(); ctx->CopyResource(stage.Get(), src->tex.Get()); }
    waitIdle();
    auto l = lock();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize(size_t(src->w) * src->h * 4);
    bool bgra = src->fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
    for (int y = 0; y < src->h; y++) {
        const uint8_t* row = (const uint8_t*)m.pData + size_t(y) * m.RowPitch;
        uint8_t* o = &out[size_t(y) * src->w * 4];
        if (!bgra) memcpy(o, row, size_t(src->w) * 4);
        else for (int x = 0; x < src->w; x++) { o[4 * x] = row[4 * x + 2]; o[4 * x + 1] = row[4 * x + 1]; o[4 * x + 2] = row[4 * x]; o[4 * x + 3] = row[4 * x + 3]; }
    }
    ctx->Unmap(stage.Get(), 0);
    return true;
}

bool Gpu::readLuma(const GpuImage& img, std::vector<uint8_t>& out) {
    if (!img.isNV12()) {
        std::vector<uint8_t> rgba;
        if (!readRGBA(img, rgba)) return false;
        out.resize(size_t(img.w) * img.h);
        for (size_t i = 0; i < out.size(); i++)
            out[i] = uint8_t(std::clamp(16.0 + 219.0 * (0.2126 * rgba[4 * i] + 0.7152 * rgba[4 * i + 1] + 0.0722 * rgba[4 * i + 2]) / 255.0 + 0.5, 0.0, 255.0));
        return true;
    }
    ComPtr<ID3D11Texture2D> stage;
    D3D11_TEXTURE2D_DESC td; img.tex->GetDesc(&td);
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &stage))) return false;
    { auto l = lock(); ctx->CopyResource(stage.Get(), img.tex.Get()); }
    waitIdle();
    auto l = lock();
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    out.resize(size_t(img.w) * img.h);
    for (int y = 0; y < img.h; y++) memcpy(&out[size_t(y) * img.w], (uint8_t*)m.pData + size_t(y) * m.RowPitch, img.w);
    ctx->Unmap(stage.Get(), 0);
    return true;
}

bool Gpu::savePNG(const GpuImage& img, const std::string& path, const YuvInfo& yuv) {
    std::vector<uint8_t> rgba;
    if (!readRGBA(img, rgba, yuv)) return false;
    return savePNG(rgba.data(), img.w, img.h, path);
}

bool Gpu::savePNG(const uint8_t* px, int w, int h, const std::string& path, bool gray) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    if (FAILED(stream->InitializeFromFilename(widen(path).c_str(), GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> enc;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    enc->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(w, h);
    WICPixelFormatGUID pf = gray ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&pf);
    std::vector<uint8_t> buf;
    if (gray) {
        buf.assign(px, px + size_t(w) * h);
        frame->WritePixels(h, w, UINT(buf.size()), buf.data());
    } else {
        buf.resize(size_t(w) * h * 4);
        for (size_t i = 0; i < size_t(w) * h; i++) { buf[4 * i] = px[4 * i + 2]; buf[4 * i + 1] = px[4 * i + 1]; buf[4 * i + 2] = px[4 * i]; buf[4 * i + 3] = 255; }
        frame->WritePixels(h, w * 4, UINT(buf.size()), buf.data());
    }
    frame->Commit();
    return SUCCEEDED(enc->Commit());
}

ImagePtr Gpu::loadPNG(const std::string& path) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
    ComPtr<IWICBitmapDecoder> dec;
    if (HRESULT hr = wic->CreateDecoderFromFilename(widen(path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec); FAILED(hr)) {
        if (getenv("FG_TRACE")) logf("[png] %s: 0x%08lx", path.c_str(), hr);
        return nullptr;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return nullptr;
    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return nullptr;
    UINT w = 0, h = 0; conv->GetSize(&w, &h);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    if (HRESULT hr = conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()); FAILED(hr)) { logf("[png] %s: decode 0x%08lx", path.c_str(), hr); return nullptr; }
    auto img = makeImage(int(w), int(h), DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!img) { logf("[png] no image"); return nullptr; }
    auto l = lock();
    ctx->UpdateSubresource(img->tex.Get(), 0, nullptr, px.data(), w * 4, 0);
    return img;
}

// ---- GpuProfiler

GpuProfiler& GpuProfiler::get() { static GpuProfiler p; static bool init = (p.enabled = getenv("FG_PROFILE") != nullptr, true); (void)init; return p; }

void GpuProfiler::begin() {
    if (!enabled) return;
    auto& g = Gpu::get();
    auto l = g.lock();
    D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    g.dev->CreateQuery(&d, &disjoint_);
    d.Query = D3D11_QUERY_TIMESTAMP;
    g.dev->CreateQuery(&d, &start_);
    g.ctx->Begin(disjoint_.Get());
    g.ctx->End(start_.Get());
    marks_.clear();
    active_ = true;
}

void GpuProfiler::mark(const char* name) {
    if (!enabled || !active_) return;
    auto& g = Gpu::get();
    auto l = g.lock();
    Entry e{name, nullptr};
    D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP, 0};
    g.dev->CreateQuery(&d, &e.q);
    g.ctx->End(e.q.Get());
    marks_.push_back(e);
}

void GpuProfiler::end() {
    if (!enabled || !active_) return;
    auto& g = Gpu::get();
    { auto l = g.lock(); g.ctx->End(disjoint_.Get()); }
    active_ = false;
    g.waitIdle();   // results are ready after this — no spinning on the GPU lock
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    for (;;) { HRESULT hr; { auto l = g.lock(); hr = g.ctx->GetData(disjoint_.Get(), &dj, sizeof dj, 0); } if (hr == S_OK) break; if (FAILED(hr)) return; Sleep(0); }
    if (dj.Disjoint || !dj.Frequency) return;
    auto ts = [&](ID3D11Query* q) { UINT64 v = 0; for (;;) { HRESULT hr; { auto l = g.lock(); hr = g.ctx->GetData(q, &v, sizeof v, 0); } if (hr == S_OK || FAILED(hr)) break; Sleep(0); } return v; };
    UINT64 prev = ts(start_.Get());
    for (auto& m : marks_) {
        UINT64 v = ts(m.q.Get());
        double ms = double(v - prev) * 1000.0 / double(dj.Frequency);
        prev = v;
        auto it = std::find_if(totals_.begin(), totals_.end(), [&](auto& t) { return t.first == m.name; });
        if (it == totals_.end()) totals_.push_back({m.name, {ms, 1}}); else { it->second.first += ms; it->second.second++; }
    }
}

void GpuProfiler::report() {
    if (!enabled) return;
    std::string s = "    GPU:";
    for (auto& [n, v] : totals_) s += strf(" %s %.2f", n.c_str(), v.first / std::max(1, v.second));
    logf("%s (ms per call)", s.c_str());
    totals_.clear();
}

// ---- Uploader

bool Uploader::init() {
    std::lock_guard<std::mutex> g(m_);
    if (dev_) return true;
    auto& gpu = Gpu::get();
    ComPtr<ID3D11Device> d; ComPtr<ID3D11DeviceContext> c;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(gpu.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &d, nullptr, &c))) return false;
    ComPtr<ID3D11Device5> d5; ComPtr<ID3D11DeviceContext4> c4;
    if (FAILED(d.As(&d5)) || FAILED(c.As(&c4))) return false;
    // Fence signalled by the upload device and waited on by the main device — and the reverse.
    HANDLE h = nullptr;
    if (FAILED(d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&upOnUp_)))) return false;
    if (FAILED(upOnUp_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h))) return false;
    HRESULT hr = gpu.dev->OpenSharedFence(h, IID_PPV_ARGS(&upOnMain_)); CloseHandle(h);
    if (FAILED(hr)) return false;
    if (FAILED(gpu.dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&readOnMain_)))) return false;
    if (FAILED(readOnMain_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h))) return false;
    hr = d5->OpenSharedFence(h, IID_PPV_ARGS(&readOnUp_)); CloseHandle(h);
    if (FAILED(hr)) return false;
    dev_ = d5; ctx_ = c4;
    logf("[capture] uploads use their own Direct3D device");
    return true;
}

ImagePtr Uploader::acquire(int w, int h) {
    for (auto& it : ring_) if (it.use_count() == 1 && it->w == w && it->h == h) return it;
    ring_.erase(std::remove_if(ring_.begin(), ring_.end(), [&](const ImagePtr& p) { return p.use_count() == 1 && (p->w != w || p->h != h); }), ring_.end());
    if (ring_.size() >= 24) return nullptr;
    auto& gpu = Gpu::get();
    auto img = std::make_shared<GpuImage>();
    img->w = w; img->h = h; img->fmt = DXGI_FORMAT_NV12;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(gpu.dev->CreateTexture2D(&td, nullptr, &img->tex))) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8_UNORM; gpu.dev->CreateShaderResourceView(img->tex.Get(), &sd, &img->srv[0]);
    sd.Format = DXGI_FORMAT_R8G8_UNORM; gpu.dev->CreateShaderResourceView(img->tex.Get(), &sd, &img->srv[1]);
    ComPtr<IDXGIResource1> res; HANDLE sh = nullptr;
    if (FAILED(img->tex.As(&res)) || FAILED(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sh))) return nullptr;
    HRESULT hr = dev_->OpenSharedResource1(sh, IID_PPV_ARGS(&img->upTex)); CloseHandle(sh);
    if (FAILED(hr)) return nullptr;
    ring_.push_back(img);
    return img;
}

ImagePtr Uploader::upload(const uint8_t* y, const uint8_t* uv, int pitch, int w, int h, uint64_t& fenceValue) {
    std::lock_guard<std::mutex> g(m_);
    if (!dev_) return nullptr;
    auto img = acquire(w, h);
    if (!img) return nullptr;
    if (!stage_[0] || stageW_ != w || stageH_ != h) {
        D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        for (auto& st : stage_) { st.Reset(); dev_->CreateTexture2D(&td, nullptr, &st); }
        stageW_ = w; stageH_ = h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D* stage = nullptr;
    for (int k = 0; k < 4 && !stage; k++) {
        int i = (stageNext_ + k) % 4;
        if (stage_[i] && SUCCEEDED(ctx_->Map(stage_[i].Get(), 0, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) { stage = stage_[i].Get(); stageNext_ = (i + 1) % 4; }
    }
    // All four still in flight: wait (only this capture thread waits — the main device is untouched).
    if (!stage && stage_[stageNext_] && SUCCEEDED(ctx_->Map(stage_[stageNext_].Get(), 0, D3D11_MAP_WRITE, 0, &m))) { stage = stage_[stageNext_].Get(); stageNext_ = (stageNext_ + 1) % 4; }
    if (!stage) return nullptr;
    for (int r = 0; r < h; r++) memcpy((uint8_t*)m.pData + size_t(r) * m.RowPitch, y + size_t(r) * pitch, w);
    uint8_t* uvDst = (uint8_t*)m.pData + size_t(m.RowPitch) * h;
    for (int r = 0; r < h / 2; r++) memcpy(uvDst + size_t(r) * m.RowPitch, uv + size_t(r) * pitch, w);
    ctx_->Unmap(stage, 0);
    ctx_->Wait(readOnUp_.Get(), img->readFence.load());   // the main device has finished reading this texture
    ctx_->CopyResource(img->upTex.Get(), stage);
    fenceValue = ++upValue_;
    ctx_->Signal(upOnUp_.Get(), fenceValue);
    ctx_->Flush();
    return img;
}

void Uploader::waitOnMain(uint64_t value) { if (upOnMain_ && value) Gpu::get().ctx->Wait(upOnMain_.Get(), value); }
void Uploader::markRead(GpuImage& img) {
    if (!readOnMain_ || !img.upTex) return;
    uint64_t v = ++readValue_;
    Gpu::get().ctx->Signal(readOnMain_.Get(), v);
    img.readFence = v;
}
