#include "DlssPresenter.h"

#ifdef FG_HAVE_STREAMLINE
#include <avrt.h>
#include <algorithm>
#include <cmath>

// Direct3D 11 composition shaders (same picture as the normal presenter).
static const char* kCompose = R"(
cbuffer C : register(b0) { float4 rect; float alpha; float3 pad; float2 k; float2 uvScale; };   // rect = NDC left, top, right, bottom
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vsQuad(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    VO o; o.pos = float4(lerp(rect.xy, rect.zw, t), 0, 1); o.uv = t; return o;
}
Texture2D T : register(t0);
Texture2D<float2> F : register(t1);
SamplerState S : register(s0);
float4 psImage(VO i) : SV_Target { return float4(T.Sample(S, i.uv).rgb, 1); }
float4 psOverlay(VO i) : SV_Target { return T.Sample(S, i.uv) * alpha; }
// Motion vectors in back-buffer pixels, pointing to where the pixel was in the previous real frame.
float2 psMvec(VO i) : SV_Target { return -F.SampleLevel(S, i.uv * uvScale, 0) * k; }
// UI overlays don't move: zero motion under them.
float2 psMvecZero(VO i) : SV_Target { if (T.Sample(S, i.uv).a < 0.02) discard; return 0; }
)";
struct ComposeCB { float rect[4]; float alpha, pad[3]; float k[2]; float uvScale[2]; };

static void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER rb{}; rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r; rb.Transition.StateBefore = a; rb.Transition.StateAfter = b; rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &rb);
}

DlssPresenter::~DlssPresenter() {
    stop();
    if (rt_) { setMode(false); waitGpu12(); }
    releaseTargets();
    back_.clear();
    swap_.Reset();
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

void DlssPresenter::waitGpu12() {
    if (!rt_ || !done12_) return;
    uint64_t v = ++doneValue_;
    rt_->queue->Signal(done12_.Get(), v);
    if (done12_->GetCompletedValue() < v) { done12_->SetEventOnCompletion(v, fenceEvent_); WaitForSingleObject(fenceEvent_, 2000); }
}

bool DlssPresenter::init(HWND hwnd, std::string& error) {
    hwnd_ = hwnd;
    const auto& sup = DlssG::support();
    if (!sup.supported) { error = "DLSS Frame Generation isn't available: " + sup.reason; return false; }
    rt_ = DlssG::runtime();
    if (!rt_ || !rt_->ensureDevice(error)) return false;
    auto& gpu = Gpu::get();
    ID3D12Device* dev = rt_->device.Get();
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Fences shared between the two APIs.
    HANDLE h = nullptr;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&done12_))) || FAILED(dev->CreateSharedHandle(done12_.Get(), nullptr, GENERIC_ALL, nullptr, &h)) ||
        FAILED(gpu.dev->OpenSharedFence(h, IID_PPV_ARGS(&done11_)))) { error = "could not share a fence between Direct3D 11 and 12"; return false; }
    CloseHandle(h);
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ready12_))) || FAILED(dev->CreateSharedHandle(ready12_.Get(), nullptr, GENERIC_ALL, nullptr, &h)) ||
        FAILED(gpu.dev->OpenSharedFence(h, IID_PPV_ARGS(&ready11_)))) { error = "could not share a fence between Direct3D 11 and 12"; return false; }
    CloseHandle(h);
    for (auto& a : alloc_) dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0].Get(), nullptr, IID_PPV_ARGS(&list_));
    list_->Close();
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_));

    // Swap chain through Streamline's proxy factory (DLSS-G takes over presentation).
    RECT rc; GetClientRect(hwnd, &rc);
    bw_ = std::max<int>(64, rc.right - rc.left); bh_ = std::max<int>(64, rc.bottom - rc.top);
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) { error = "DXGI unavailable"; return false; }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGIFactory2* proxyFactory = factory.Get(); proxyFactory->AddRef();
    if (rt_->upgrade((void**)&proxyFactory) != sl::Result::eOk) { proxyFactory->Release(); error = "Streamline couldn't attach to DXGI"; return false; }
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bw_; sd.Height = bh_; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = gpu.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = proxyFactory->CreateSwapChainForHwnd(rt_->queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    proxyFactory->Release();
    if (FAILED(hr)) { error = "could not create the DLSS swap chain (" + hr2str(hr) + ")"; return false; }
    sc1.As(&swap_);
    if (!createTargets(bw_, bh_, error)) return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    gpu.dev->CreateBlendState(&bd, &premul_);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    gpu.dev->CreateRasterizerState(&rd, &rs_);
    for (const char* e : {"psImage", "psOverlay", "psMvec", "psMvecZero"}) gpu.ps(kCompose, e);
    gpu.vs(kCompose, "vsQuad");
    logf("[dlss] DLSS Frame Generation presenter ready (%dx%d)", bw_, bh_);
    return true;
}

bool DlssPresenter::createTargets(int w, int h, std::string& error) {
    auto& gpu = Gpu::get();
    ID3D12Device* dev = rt_->device.Get();
    back_.resize(3);
    for (UINT i = 0; i < 3; i++) if (FAILED(swap_->GetBuffer(i, IID_PPV_ARGS(&back_[i])))) { error = "swap chain buffers unavailable"; return false; }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto texDesc = [&](DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1;
        d.MipLevels = 1; d.Format = f; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = flags; return d;
    };
    auto shared = [&](DXGI_FORMAT f, ComPtr<ID3D12Resource>& r12, ComPtr<ID3D11Texture2D>& r11, ComPtr<ID3D11RenderTargetView>& rtv) {
        auto d = texDesc(f, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r12)))) return false;
        HANDLE sh = nullptr;
        if (FAILED(dev->CreateSharedHandle(r12.Get(), nullptr, GENERIC_ALL, nullptr, &sh))) return false;
        HRESULT hr = gpu.dev->OpenSharedResource1(sh, IID_PPV_ARGS(&r11)); CloseHandle(sh);
        return SUCCEEDED(hr) && SUCCEEDED(gpu.dev->CreateRenderTargetView(r11.Get(), nullptr, &rtv));
    };
    int si = 0;
    for (auto& s : slots_) {
        if (!shared(DXGI_FORMAT_R8G8B8A8_UNORM, s.color12, s.color11, s.colorRtv) || !shared(DXGI_FORMAT_R16G16_FLOAT, s.mvec12, s.mvec11, s.mvecRtv)) {
            error = "could not share textures between Direct3D 11 and 12"; return false;
        }
        auto md = texDesc(DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE);
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &md, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.mvecOwned)))) {
            error = "could not create the motion vector buffer"; return false;
        }
        s.mvecOwned->SetName((L"FG mvec tagged " + std::to_wstring(si)).c_str());
        s.color12->SetName((L"FG color slot " + std::to_wstring(si)).c_str());
        s.mvec12->SetName((L"FG mvec slot " + std::to_wstring(si)).c_str());
        si++;
        s.doneValue = 0;
    }
    for (size_t i = 0; i < back_.size(); i++) back_[i]->SetName((L"FG backbuffer " + std::to_wstring(i)).c_str());
    // Flat depth: a capture card has no depth, so the whole picture is one plane.
    D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_R32_FLOAT; cv.Color[0] = 0.5f;
    auto dd = texDesc(DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&depth_)))) { error = "could not create the depth buffer"; return false; }
    depth_->SetName(L"FG depth");
    D3D12_CPU_DESCRIPTOR_HANDLE rtvh = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(depth_.Get(), nullptr, rtvh);
    waitGpu12();
    alloc_[0]->Reset();
    list_->Reset(alloc_[0].Get(), nullptr);
    list_->ClearRenderTargetView(rtvh, cv.Color, 0, nullptr);
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    rt_->queue->ExecuteCommandLists(1, lists);
    waitGpu12();
    bw_ = w; bh_ = h;
    return true;
}

void DlssPresenter::releaseTargets() {
    for (auto& s : slots_) s = Slot{};
    depth_.Reset();
    back_.clear();
}

int DlssPresenter::wantedGenerate() const {
    int maxGen = DlssG::maxGenerateKnown() > 0 ? DlssG::maxGenerateKnown() : DlssG::support().maxFramesToGenerate;
    return std::clamp(generate.load(), 1, std::max(1, maxGen));
}

void DlssPresenter::setMode(bool on) {
    if (!rt_ || !rt_->dlssgSetOptions) return;
    sl::DLSSGOptions o{};
    o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = uint32_t(wantedGenerate());
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    o.numBackBuffers = 3;
    o.colorWidth = bw_; o.colorHeight = bh_; o.mvecDepthWidth = bw_; o.mvecDepthHeight = bh_;
    o.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM; o.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT; o.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
    sl::Result r = rt_->dlssgSetOptions(sl::ViewportHandle(0), o);
    if (r != sl::Result::eOk) logf("[dlss] slDLSSGSetOptions: %s", DlssG::resultText(r).c_str());
    modeOn_ = on;
    optionsGenerate_ = int(o.numFramesToGenerate);
}

void DlssPresenter::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    SetThreadDescription(thread_.native_handle(), L"FrameGen.present.dlss");
}

void DlssPresenter::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// Direct3D 11: compose picture + motion vectors into the slot's shared textures.
void DlssPresenter::compose(Slot& s, const FramePtr& frame) {
    auto& gpu = Gpu::get();
    Overlay ov[2];
    { std::lock_guard<std::mutex> l(ovLock_); ov[0] = overlays_[0]; ov[1] = overlays_[1]; }
    auto l = gpu.lock();
    gpu.ctx->Wait(done11_.Get(), s.doneValue);   // Direct3D 12 has finished with this slot
    ID3D11RenderTargetView* rtvs[1] = {s.colorRtv.Get()};
    float black[4] = {0, 0, 0, 1}, zero[4] = {0, 0, 0, 0};
    gpu.ctx->ClearRenderTargetView(s.colorRtv.Get(), black);
    gpu.ctx->ClearRenderTargetView(s.mvecRtv.Get(), zero);
    D3D11_VIEWPORT vp{0, 0, float(bw_), float(bh_), 0, 1};
    gpu.ctx->RSSetViewports(1, &vp);
    gpu.ctx->RSSetState(rs_.Get());
    gpu.ctx->IASetInputLayout(nullptr);
    gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    gpu.ctx->VSSetShader(gpu.vs(kCompose, "vsQuad"), nullptr, 0);
    ID3D11SamplerState* samp = gpu.linearClamp();
    gpu.ctx->PSSetSamplers(0, 1, &samp);
    if (frame && frame->image && !frame->image->isNV12()) {
        const GpuImage& img = *frame->image;
        float va = float(bw_) / bh_, fa = float(img.w) / img.h, sx = 1, sy = 1;
        switch (scaling.load()) {
        case 2: break;
        case 1: if (va > fa) sy = va / fa; else sx = fa / va; break;
        default: if (va > fa) sx = fa / va; else sy = va / fa; break;
        }
        ComposeCB c{{-sx, sy, sx, -sy}, 1, {}, {0, 0}, {1, 1}};
        // Picture
        gpu.ctx->OMSetRenderTargets(1, rtvs, nullptr);
        gpu.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        gpu.setCB(0, &c, sizeof c, true);
        ID3D11ShaderResourceView* srv = img.srv[0].Get();
        gpu.ctx->PSSetShaderResources(0, 1, &srv);
        gpu.ctx->PSSetShader(gpu.ps(kCompose, "psImage"), nullptr, 0);
        gpu.ctx->Draw(4, 0);
        // Motion vectors (window pixels): field units × field scale × displayed size / video size.
        if (frame->mvec) {
            D3D11_TEXTURE2D_DESC fd; frame->mvec->tex->GetDesc(&fd);
            c.k[0] = frame->mvecScale * (sx * bw_) / img.w;
            c.k[1] = frame->mvecScale * (sy * bh_) / img.h;
            c.uvScale[0] = float(img.w) / (fd.Width * 8 * frame->mvecScale);
            c.uvScale[1] = float(img.h) / (fd.Height * 8 * frame->mvecScale);
            gpu.setCB(0, &c, sizeof c, true);
            ID3D11RenderTargetView* mv = s.mvecRtv.Get();
            gpu.ctx->OMSetRenderTargets(1, &mv, nullptr);
            ID3D11ShaderResourceView* fs[2] = {nullptr, frame->mvec->srv[0].Get()};
            gpu.ctx->PSSetShaderResources(0, 2, fs);
            gpu.ctx->PSSetShader(gpu.ps(kCompose, "psMvec"), nullptr, 0);
            gpu.ctx->Draw(4, 0);
        }
    }
    // Dashboard / FPS counter on top of the picture, with zero motion underneath.
    for (auto& o : ov) {
        if (!o.visible || !o.image) continue;
        float x0 = 2.f * o.x / bw_ - 1, y0 = 1 - 2.f * o.y / bh_, x1 = 2.f * (o.x + o.image->w) / bw_ - 1, y1 = 1 - 2.f * (o.y + o.image->h) / bh_;
        ComposeCB c{{x0, y0, x1, y1}, o.alpha, {}, {0, 0}, {1, 1}};
        gpu.setCB(0, &c, sizeof c, true);
        ID3D11ShaderResourceView* srv[2] = {o.image->srv[0].Get(), nullptr};
        gpu.ctx->PSSetShaderResources(0, 2, srv);
        gpu.ctx->OMSetRenderTargets(1, rtvs, nullptr);
        gpu.ctx->OMSetBlendState(premul_.Get(), nullptr, 0xffffffff);
        gpu.ctx->PSSetShader(gpu.ps(kCompose, "psOverlay"), nullptr, 0);
        gpu.ctx->Draw(4, 0);
        ID3D11RenderTargetView* mv = s.mvecRtv.Get();
        gpu.ctx->OMSetRenderTargets(1, &mv, nullptr);
        gpu.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        gpu.ctx->PSSetShader(gpu.ps(kCompose, "psMvecZero"), nullptr, 0);
        gpu.ctx->Draw(4, 0);
    }
    ID3D11ShaderResourceView* n[2] = {};
    gpu.ctx->PSSetShaderResources(0, 2, n);
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    gpu.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    gpu.ctx->Signal(ready11_.Get(), ++readyValue_);
    gpu.ctx->Flush();
}

// Direct3D 12: copy into the back buffer, tag inputs, constants, markers, present.
bool DlssPresenter::presentSlot(Slot& s, bool reset) {
    sl::FrameToken* token = nullptr;
    uint32_t fi = frameIndex_++;
    if (rt_->newFrameToken(token, &fi) != sl::Result::eOk || !token) return false;
    rt_->pclMarker(sl::PCLMarker::eSimulationStart, *token);
    rt_->pclMarker(sl::PCLMarker::eSimulationEnd, *token);
    rt_->pclMarker(sl::PCLMarker::eRenderSubmitStart, *token);

    sl::Constants c{};
    sl::float4x4 id{};
    for (int r = 0; r < 4; r++) id.row[r] = sl::float4(r == 0 ? 1.f : 0.f, r == 1 ? 1.f : 0.f, r == 2 ? 1.f : 0.f, r == 3 ? 1.f : 0.f);
    c.cameraViewToClip = id; c.clipToCameraView = id; c.clipToLensClip = id; c.clipToPrevClip = id; c.prevClipToClip = id;
    c.jitterOffset = {0, 0};
    c.mvecScale = {1.f / bw_, 1.f / bh_};   // vectors are in back-buffer pixels
    c.cameraPinholeOffset = {0, 0};
    c.cameraPos = {0, 0, 0}; c.cameraUp = {0, 1, 0}; c.cameraRight = {1, 0, 0}; c.cameraFwd = {0, 0, 1};
    c.cameraNear = 0.1f; c.cameraFar = 1000.f; c.cameraFOV = 1.0f; c.cameraAspectRatio = float(bw_) / bh_;
    c.motionVectorsInvalidValue = 0;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    rt_->setConstants(c, *token, sl::ViewportHandle(0));
    if (wantedGenerate() != optionsGenerate_ || !modeOn_) setMode(true);

    int ai = allocIdx_; allocIdx_ = (allocIdx_ + 1) % 3;
    if (done12_->GetCompletedValue() < allocDone_[ai]) { done12_->SetEventOnCompletion(allocDone_[ai], fenceEvent_); WaitForSingleObject(fenceEvent_, 1000); }
    alloc_[ai]->Reset();
    list_->Reset(alloc_[ai].Get(), nullptr);
    UINT bi = swap_->GetCurrentBackBufferIndex();
    ID3D12Resource* bb = back_[bi % back_.size()].Get();
    barrier(list_.Get(), bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list_->CopyResource(bb, s.color12.Get());
    barrier(list_.Get(), bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    // Motion vectors: copy the shared texture into a normal D3D12 texture and tag that one
    // (Streamline's clone of a cross-API shared resource needed an alignment override).
    list_->CopyResource(s.mvecOwned.Get(), s.mvec12.Get());
    barrier(list_.Get(), s.mvecOwned.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Extent ext{0, 0, uint32_t(bw_), uint32_t(bh_)};
    sl::Resource depth(sl::ResourceType::eTex2d, depth_.Get(), nullptr, nullptr, uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET));
    sl::Resource mvec(sl::ResourceType::eTex2d, s.mvecOwned.Get(), nullptr, nullptr, uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    sl::ResourceTag tags[] = {sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ext),
                              sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &ext)};
    rt_->setTag(*token, sl::ViewportHandle(0), tags, 2, (sl::CommandBuffer*)list_.Get());
    barrier(list_.Get(), s.mvecOwned.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list_->Close();
    rt_->queue->Wait(ready12_.Get(), readyValue_);   // Direct3D 11 has finished composing this slot
    ID3D12CommandList* lists[] = {list_.Get()};
    rt_->queue->ExecuteCommandLists(1, lists);
    uint64_t v = ++doneValue_;
    rt_->queue->Signal(done12_.Get(), v);
    allocDone_[ai] = v;
    s.doneValue = v;
    rt_->pclMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
    rt_->pclMarker(sl::PCLMarker::ePresentStart, *token);
    HRESULT hr = swap_->Present(0, 0);
    rt_->pclMarker(sl::PCLMarker::ePresentEnd, *token);
    if (FAILED(hr) || rt_->device->GetDeviceRemovedReason() != S_OK) {
        logf("[dlss] Present failed: %s, device removed reason %s", hr2str(hr).c_str(), hr2str(rt_->device->GetDeviceRemovedReason()).c_str());
        dumpDred();
        return false;
    }
    return true;
}

void DlssPresenter::dumpDred() {
    static bool done = false;
    if (done) return;
    done = true;
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(rt_->device.As(&dred))) { logf("[dlss] DRED unavailable (run with FG_DRED=1)"); return; }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf))) {
        logf("[dred] page fault at GPU VA 0x%llx", (unsigned long long)pf.PageFaultVA);
        for (auto* n = pf.pHeadExistingAllocationNode; n; n = n->pNext) logf("[dred]   existing allocation: %ls", n->ObjectNameW ? n->ObjectNameW : L"(unnamed)");
        for (auto* n = pf.pHeadRecentFreedAllocationNode; n; n = n->pNext) logf("[dred]   recently freed: %ls", n->ObjectNameW ? n->ObjectNameW : L"(unnamed)");
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
        for (auto* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            UINT doneOps = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (doneOps >= n->BreadcrumbCount) continue;   // finished command lists aren't interesting
            logf("[dred] unfinished list %ls on queue %ls: %u of %u ops done", n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"?",
                 n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"?", doneOps, n->BreadcrumbCount);
            for (UINT i = doneOps; i < n->BreadcrumbCount && i < doneOps + 6; i++) logf("[dred]     op %u: %d", i, int(n->pCommandHistory[i]));
        }
    }
}

void DlssPresenter::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Games", &task);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    double lastStateCheck = 0, lastReal = 0;
    bool needReset = true;
    while (running_) {
        if (resizePending_) {
            resizePending_ = false;
            int w = std::max(64, pendW_.load()), h = std::max(64, pendH_.load());
            if (w != bw_ || h != bh_) {
                setMode(false);
                waitGpu12();
                { auto l = Gpu::get().lock(); Gpu::get().ctx->Flush(); }
                releaseTargets();
                HRESULT hr = swap_->ResizeBuffers(3, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, Gpu::get().tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                std::string err;
                if (FAILED(hr) || !createTargets(w, h, err)) { logf("[dlss] resize failed: %s %s", hr2str(hr).c_str(), err.c_str()); Sleep(50); continue; }
                needReset = true;
            }
        }
        // Next real frame (in DLSS mode the pipeline produces only real frames).
        double d = delay;
        double wanted = pipeline_.requiredDelay() + extraDelay;
        d = wanted > d ? wanted : d + (wanted - d) * 0.05;
        delay = d;
        stats_.pipelineDelayMs = d * 1000;
        FramePtr next = pipeline_.output.popAfter(last_ ? last_->time : 0);
        double now = hostNow();
        if (!next) {
            // Nothing new (pause, menu, still picture): keep the window alive with the last frame.
            if (last_ && now - lastPresent_ > 0.1) {
                Slot& s = slots_[slot_]; slot_ = (slot_ + 1) % 3;
                auto still = std::make_shared<VideoFrame>(*last_); still->mvec.reset();
                compose(s, still);
                presentSlot(s, true);
                lastPresent_ = hostNow();
            }
            Sleep(1);
            continue;
        }
        double due = next->time + d;
        if (due - now > 0.25) due = now + 0.25;
        preciseSleepUntil(due);
        if (!running_) break;
        Slot& s = slots_[slot_]; slot_ = (slot_ + 1) % 3;
        bool reset = needReset || !next->mvec || (last_ && next->time - last_->time > 0.2);
        compose(s, next);
        if (presentSlot(s, reset)) needReset = false;
        double t = hostNow();
        // Output rate: DLSS-G reports how many frames it actually showed per real frame.
        int n = 1;
        sl::DLSSGState st{};
        if (rt_->dlssgGetState(sl::ViewportHandle(0), st, nullptr) == sl::Result::eOk) {
            n = std::max(1, int(st.numFramesActuallyPresented));
            DlssG::setMaxGenerateKnown(int(st.numFramesToGenerateMax));
            static double lastLog = 0;
            if (g_opt.log && t - lastLog > 1) { lastLog = t; logf("[dlss] state: status %u, presented %u per real frame, max generate %u, vsync support %d, mode %s gen %d",
                unsigned(st.status), st.numFramesActuallyPresented, st.numFramesToGenerateMax, int(st.bIsVsyncSupportAvailable), modeOn_ ? "on" : "off", optionsGenerate_); }
            if (st.status != sl::DLSSGStatus::eOk && t - lastStateCheck > 1) {
                lastStateCheck = t;
                std::lock_guard<std::mutex> l(ovLock_);
                error_ = strf("DLSS Frame Generation reported a problem (status %u).", unsigned(st.status));
                logf("[dlss] %s", error_.c_str());
            }
        }
        double span = lastReal > 0 ? std::min(0.1, t - lastReal) : 0.0;
        for (int i = 0; i < n; i++) { double ts = t - span * double(n - 1 - i) / n; stats_.recordPresent(ts); stats_.recordRefresh(ts); }
        lastReal = t;
        last_ = next;
        lastPresent_ = t;
    }
    if (av) AvRevertMmThreadCharacteristics(av);
    CoUninitialize();
}
#endif
