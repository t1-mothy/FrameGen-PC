#include "FsrEngine.h"
#include "FlowEngine.h"
#include "Hud.h"
#include <d3d12.h>
#include <filesystem>
#include <mutex>

#include "ffx_api.h"
#include "ffx_framegeneration.h"
#include "dx12/ffx_api_dx12.h"

namespace {
constexpr int kRing = 3;   // command lists in flight

struct Api {
    bool ok = false;
    std::string error;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

const wchar_t* kLoader = L"amd_fidelityfx_loader_dx12.dll";
const wchar_t* kFrameGen = L"amd_fidelityfx_framegeneration_dx12.dll";

std::wstring runtimeDir() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path().wstring() + L"\\amd-fsr";
}

/// Loads and verifies AMD's DLLs once per process (they stay loaded).
const Api& api() {
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        std::wstring dir = runtimeDir();
        for (const wchar_t* n : {kFrameGen, kLoader}) {
            std::wstring p = dir + L"\\" + n;
            if (!std::filesystem::exists(p)) { a.error = "AMD FSR's files are missing (" + narrow(n) + "). Reinstall FrameGen. Frame generation is disabled, please select another option."; return; }
            if (!getenv("FG_FSR_UNSIGNED") && !signedBy(p, L"Advanced Micro Devices")) { a.error = narrow(n) + " is not signed by AMD — refusing to load it."; return; }
        }
        // The frame generation provider first, by full path: the loader then finds it already loaded.
        auto load = [&](const wchar_t* n) { return LoadLibraryExW((dir + L"\\" + n).c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); };
        HMODULE fg = load(kFrameGen);
        HMODULE loader = load(kLoader);
        if (!fg || !loader) { a.error = strf("Couldn't load AMD FSR (error %lu).", GetLastError()); return; }
        a.create = (PfnFfxCreateContext)GetProcAddress(loader, "ffxCreateContext");
        a.destroy = (PfnFfxDestroyContext)GetProcAddress(loader, "ffxDestroyContext");
        a.configure = (PfnFfxConfigure)GetProcAddress(loader, "ffxConfigure");
        a.query = (PfnFfxQuery)GetProcAddress(loader, "ffxQuery");
        a.dispatch = (PfnFfxDispatch)GetProcAddress(loader, "ffxDispatch");
        a.ok = a.create && a.destroy && a.configure && a.dispatch;
        if (!a.ok) a.error = "AMD FSR's loader is missing functions.";
    });
    return a;
}

std::string ffxText(ffxReturnCode_t r) {
    switch (r) {
    case FFX_API_RETURN_OK: return "ok";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown description type";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "Direct3D 12 error";
    case FFX_API_RETURN_NO_PROVIDER: return "no provider (unsupported GPU or driver)";
    case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
    case FFX_API_RETURN_ERROR_PARAMETER: return "invalid parameter";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "the AMD DLL is too old";
    default: return strf("error %u", r);
    }
}

void ffxMessage(uint32_t type, const wchar_t* message) {
    logf("[fsr] %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", narrow(message ? message : L"").c_str());
}

void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER rb{}; rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r; rb.Transition.StateBefore = a; rb.Transition.StateAfter = b; rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    l->ResourceBarrier(1, &rb);
}

// Motion vectors for FSR, in video pixels, pointing from each pixel to where it was in the previous
// real frame (−field, see VideoFrame::mvec).
const char* kMvec = R"(
Texture2D<float2> F : register(t0);
RWTexture2D<float2> O : register(u0);
SamplerState S : register(s0);
cbuffer C : register(b0) { float2 k; float2 uvScale; float2 invSize; float2 pad; };
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; O.GetDimensions(w, h); if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) * invSize;
    O[id.xy] = -F.SampleLevel(S, uv * uvScale, 0) * k;
}
)";
}

namespace Fsr {
const char* kGpuUnsupported = "Your GPU does not support AMD FSR frame generation (it needs DirectX 12 with Shader Model 6.2). Frame generation is disabled, please select another option.";

const Support& support() {
    static Support s;
    static std::once_flag once;
    std::call_once(once, [] {
        s.dir = runtimeDir();
        auto& gpu = Gpu::get();
        if (getenv("FG_FSR_NOGPU")) { s.reason = kGpuUnsupported; return; }   // test aid: behave like an unsupported GPU
        if (!std::filesystem::exists(s.dir + L"\\" + kFrameGen) || !std::filesystem::exists(s.dir + L"\\" + kLoader)) {
            s.reason = "AMD FSR's files are missing (reinstall FrameGen). Frame generation is disabled, please select another option.";
            return;
        }
        // Direct3D 12 on the same graphics card (checked without creating a device).
        if (!gpu.adapter || FAILED(D3D12CreateDevice(gpu.adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) { s.reason = kGpuUnsupported; return; }
        s.supported = true;
    });
    return s;
}
}

struct FsrEngine::State {
    int w = 0, h = 0;
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc[kRing];
    ComPtr<ID3D12GraphicsCommandList> list[kRing];
    uint64_t allocDone[kRing] = {};
    int ring = 0;
    ComPtr<ID3D12Fence> fence12;     // shared: D3D11 signals "inputs ready", D3D12 signals "output ready"
    ComPtr<ID3D11Fence> fence11;
    uint64_t fenceValue = 0;
    HANDLE event = nullptr;
    struct Shared { ComPtr<ID3D12Resource> r12; ComPtr<ID3D11Texture2D> t11; ComPtr<ID3D11UnorderedAccessView> uav; };
    Shared color[2], mvec[2], out;   // colour / motion alternate per frame (FSR still reads the previous one)
    int slot = 0;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ffxContext ctx = nullptr;
    uint64_t frameID = 0;
    ImagePtr lastFed;                // the real frame FSR saw last, as the pipeline gave it (held, so the pointer stays unique)
    double lastTime = 0;

    /// Signals the shared fence from Direct3D 12 and waits on the CPU (no Gpu::lock held).
    void idle() {
        if (!queue || !fence12) return;
        uint64_t v = ++fenceValue;
        queue->Signal(fence12.Get(), v);
        if (fence12->GetCompletedValue() < v) { fence12->SetEventOnCompletion(v, event); WaitForSingleObject(event, 2000); }
    }
};

// Motion vectors from Smoothest's search (8×8 blocks at full resolution), with no penalty on vector
// length: measured on console gameplay (docs/DEVELOPMENT.md) this halves FSR's error against VFG.
// A per-pixel choice of vector (as FrameGen's own interpolation does) scored slightly worse: FSR
// wants smooth, game-like vectors.
FsrEngine::FsrEngine() : flow_(std::make_unique<FlowEngine>(FlowEngine::Quality::High)) { flow_->setLengthPenalty(0); }
FsrEngine::~FsrEngine() { release(); }

void FsrEngine::release() {
    if (!s_) return;
    s_->idle();
    if (s_->ctx) api().destroy(&s_->ctx, nullptr);
    if (s_->event) CloseHandle(s_->event);
    delete s_;
    s_ = nullptr;
}

bool FsrEngine::ensure(int w, int h, std::string& error) {
    if (s_ && s_->w == w && s_->h == h) return true;
    release();
    const auto& a = api();
    if (!a.ok) { error = a.error; return false; }
    auto& gpu = Gpu::get();
    double t0 = hostNow();
    s_ = new State;
    State& s = *s_;
    s.w = w; s.h = h;
    auto fail = [&](const std::string& why) { error = why; release(); return false; };
    if (FAILED(D3D12CreateDevice(gpu.adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&s.dev)))) return fail(Fsr::kGpuUnsupported);
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_2};
    if (FAILED(s.dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_2) return fail(Fsr::kGpuUnsupported);
    s.dev->SetName(L"FrameGen FSR device");
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(s.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&s.queue)))) return fail("AMD FSR: couldn't create a Direct3D 12 queue.");
    for (int i = 0; i < kRing; i++) {
        if (FAILED(s.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc[i]))) ||
            FAILED(s.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc[i].Get(), nullptr, IID_PPV_ARGS(&s.list[i])))) return fail("AMD FSR: couldn't create Direct3D 12 command lists.");
        s.list[i]->Close();
    }
    s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE sh = nullptr;
    if (FAILED(s.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s.fence12))) || FAILED(s.dev->CreateSharedHandle(s.fence12.Get(), nullptr, GENERIC_ALL, nullptr, &sh)) ||
        FAILED(gpu.dev->OpenSharedFence(sh, IID_PPV_ARGS(&s.fence11)))) { if (sh) CloseHandle(sh); return fail("AMD FSR: couldn't share a fence between Direct3D 11 and 12."); }
    CloseHandle(sh);

    // Textures shared with Direct3D 11.
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto texDesc = [&](DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = UINT64(w); d.Height = UINT(h); d.DepthOrArraySize = 1;
        d.MipLevels = 1; d.Format = f; d.SampleDesc.Count = 1; d.Flags = flags; return d;
    };
    std::string sharedError;
    auto shared = [&](DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags, State::Shared& r, bool uav11, const wchar_t* name) {
        auto d = texDesc(f, flags | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
        auto failed = [&](const char* step, HRESULT hr) { sharedError = strf("%s for %s: %s", step, narrow(name).c_str(), hr2str(hr).c_str()); return false; };
        HRESULT hr = s.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r.r12));
        if (FAILED(hr)) return failed("CreateCommittedResource", hr);
        r.r12->SetName(name);
        HANDLE h = nullptr;
        if (FAILED(hr = s.dev->CreateSharedHandle(r.r12.Get(), nullptr, GENERIC_ALL, nullptr, &h))) return failed("CreateSharedHandle", hr);
        hr = gpu.dev->OpenSharedResource1(h, IID_PPV_ARGS(&r.t11)); CloseHandle(h);
        if (FAILED(hr)) return failed("OpenSharedResource1", hr);
        if (uav11 && FAILED(hr = gpu.dev->CreateUnorderedAccessView(r.t11.Get(), nullptr, &r.uav))) return failed("CreateUnorderedAccessView", hr);
        return true;
    };
    bool ok = true;
    for (int i = 0; i < 2; i++) {
        ok = ok && shared(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, s.color[i], false, L"FrameGen FSR colour");
        // (Direct3D 11 only opens shared textures that can also be render targets.)
        ok = ok && shared(DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, s.mvec[i], true, L"FrameGen FSR motion");
    }
    ok = ok && shared(DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, s.out, false, L"FrameGen FSR output");
    if (!ok) { logf("[fsr] %s", sharedError.c_str()); return fail("AMD FSR: couldn't share textures between Direct3D 11 and 12 (out of video memory?)."); }

    // Flat depth: a capture card has no depth, so the whole picture is one plane.
    D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_R32_FLOAT; cv.Color[0] = 0.5f;
    auto dd = texDesc(DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if (FAILED(s.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&s.depth)))) return fail("AMD FSR: out of video memory.");
    s.depth->SetName(L"FrameGen FSR depth");
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    s.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s.rtvHeap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    s.dev->CreateRenderTargetView(s.depth.Get(), nullptr, rtv);
    s.alloc[0]->Reset();
    s.list[0]->Reset(s.alloc[0].Get(), nullptr);
    s.list[0]->ClearRenderTargetView(rtv, cv.Color, 0, nullptr);
    barrier(s.list[0].Get(), s.depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.list[0]->Close();
    ID3D12CommandList* lists[] = {s.list[0].Get()};
    s.queue->ExecuteCommandLists(1, lists);
    s.idle();

    // The FSR frame generation context.
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = s.dev.Get();
    ffxCreateContextDescFrameGenerationVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.header.pNext = &backend.header;
    version.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.header.pNext = &version.header;
    cd.flags = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    cd.displaySize = {uint32_t(w), uint32_t(h)};
    cd.maxRenderSize = {uint32_t(w), uint32_t(h)};
    cd.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    ffxReturnCode_t r = a.create(&s.ctx, &cd.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        s.ctx = nullptr;
        return fail(r == FFX_API_RETURN_NO_PROVIDER ? std::string(Fsr::kGpuUnsupported) : "AMD FSR couldn't start: " + ffxText(r) + ". Frame generation is disabled, please select another option.");
    }
    ffxConfigureDescGlobalDebug dbg{};
    dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    dbg.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
    dbg.fpMessage = ffxMessage;
    dbg.debugLevel = getenv("FG_TRACE") ? FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE : FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
    a.configure(&s.ctx, &dbg.header);
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    std::string provider = a.query && a.query(&s.ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName ? pv.versionName : "?";
    logf("[fsr] AMD FSR frame generation %s ready at %dx%d in %.0f ms", provider.c_str(), w, h, (hostNow() - t0) * 1000);
    return true;
}

bool FsrEngine::feed(const ImagePtr& color, const ImagePtr& field, float fieldScale, bool reset, double dtMs, std::string& error) {
    auto& gpu = Gpu::get();
    const auto& a = api();
    State& s = *s_;
    int slot = s.slot; s.slot ^= 1;
    // Direct3D 11: the frame and its motion vectors into the shared textures, then "ready".
    uint64_t inputsReady;
    {
        auto l = gpu.lock();
        gpu.ctx->CopyResource(s.color[slot].t11.Get(), color->tex.Get());
        if (field) {
            D3D11_TEXTURE2D_DESC fd; field->tex->GetDesc(&fd);
            static const float mvTest = getenv("FG_FSR_MV") ? float(atof(getenv("FG_FSR_MV"))) : 1.f;   // test aid: scale / sign of the motion vectors
            struct { float k[2], uvScale[2], invSize[2], pad[2]; } c{
                {fieldScale * mvTest, fieldScale * mvTest},
                {float(s.w) / (fd.Width * 8 * fieldScale), float(s.h) / (fd.Height * 8 * fieldScale)},
                {1.f / s.w, 1.f / s.h}, {}};
            gpu.dispatch(gpu.cs(kMvec), {field->srv[0].Get()}, {s.mvec[slot].uav.Get()}, &c, sizeof c, UINT(s.w), UINT(s.h));
        } else {
            const float zero[4] = {};
            gpu.ctx->ClearUnorderedAccessViewFloat(s.mvec[slot].uav.Get(), zero);
        }
        inputsReady = ++s.fenceValue;
        gpu.ctx->Signal(s.fence11.Get(), inputsReady);
        gpu.ctx->Flush();
    }
    // Direct3D 12: FSR's two passes (prepare with the motion vectors, then generate).
    int i = s.ring; s.ring = (s.ring + 1) % kRing;
    if (s.fence12->GetCompletedValue() < s.allocDone[i]) { s.fence12->SetEventOnCompletion(s.allocDone[i], s.event); WaitForSingleObject(s.event, 2000); }
    s.alloc[i]->Reset();
    ID3D12GraphicsCommandList* list = s.list[i].Get();
    list->Reset(s.alloc[i].Get(), nullptr);
    barrier(list, s.color[slot].r12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(list, s.mvec[slot].r12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(list, s.out.r12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uint64_t id = ++s.frameID;
    FfxApiRect2D rect{0, 0, s.w, s.h};

    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.frameGenerationEnabled = true;
    cfg.allowAsyncWorkloads = false;
    cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;   // no FSR swap chain: FrameGen presents the frames itself
    cfg.onlyPresentGenerated = false;
    cfg.generationRect = rect;
    cfg.frameID = id;   // must match the dispatch's, or FSR treats the frame as a reset
    const char* step = "configure";
    ffxReturnCode_t r = a.configure(&s.ctx, &cfg.header);

    ffxDispatchDescFrameGenerationPrepareV2 prep{};
    prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prep.frameID = id;
    prep.flags = 0;
    prep.commandList = list;
    prep.renderSize = {uint32_t(s.w), uint32_t(s.h)};
    prep.jitterOffset = {0, 0};
    prep.motionVectorScale = {1, 1};   // already in pixels
    prep.frameTimeDelta = float(dtMs);
    prep.reset = reset;
    prep.cameraNear = 0.1f; prep.cameraFar = 100.f;
    prep.cameraFovAngleVertical = 1.0f;
    prep.viewSpaceToMetersFactor = 1.f;
    prep.depth = ffxApiGetResourceDX12(s.depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prep.motionVectors = ffxApiGetResourceDX12(s.mvec[slot].r12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prep.cameraUp[1] = 1; prep.cameraRight[0] = 1; prep.cameraForward[2] = 1;
    if (r == FFX_API_RETURN_OK) { step = "prepare"; r = a.dispatch(&s.ctx, &prep.header); }

    ffxDispatchDescFrameGeneration gen{};
    gen.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    gen.commandList = list;
    gen.presentColor = ffxApiGetResourceDX12(s.color[slot].r12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    gen.outputs[0] = ffxApiGetResourceDX12(s.out.r12.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    gen.numGeneratedFrames = 1;
    gen.reset = reset;
    gen.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    gen.minMaxLuminance[0] = 0; gen.minMaxLuminance[1] = 1000;
    gen.generationRect = rect;
    gen.frameID = id;
    if (r == FFX_API_RETURN_OK) { step = "generate"; r = a.dispatch(&s.ctx, &gen.header); }

    barrier(list, s.color[slot].r12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(list, s.mvec[slot].r12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    barrier(list, s.out.r12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    HRESULT hr = list->Close();
    // Always submit (with the waits and signals), so the fence keeps counting up even after an error.
    s.queue->Wait(s.fence12.Get(), inputsReady);
    if (SUCCEEDED(hr)) { ID3D12CommandList* lists[] = {list}; s.queue->ExecuteCommandLists(1, lists); }
    uint64_t outputReady = ++s.fenceValue;
    s.queue->Signal(s.fence12.Get(), outputReady);
    s.allocDone[i] = outputReady;
    {
        auto l = gpu.lock();
        gpu.ctx->Wait(s.fence11.Get(), outputReady);   // later Direct3D 11 work waits for FSR (on the GPU)
    }
    if (r != FFX_API_RETURN_OK) { error = strf("AMD FSR (%s): %s", step, ffxText(r).c_str()); return false; }
    if (FAILED(hr)) { error = "AMD FSR: " + hr2str(hr); return false; }
    if (Gpu::isDeviceLost(s.dev->GetDeviceRemovedReason())) { error = "AMD FSR: the GPU was reset."; return false; }
    return true;
}

bool FsrEngine::interpolate(const VideoFrame& prev, const VideoFrame& next, const std::vector<float>& phases,
                            std::vector<ImagePtr>& out, std::string& error) {
    if (!fatal_.empty()) { error = fatal_; return false; }
    auto& gpu = Gpu::get();
    if (gpu.deviceLost) { error = "The graphics driver reset the GPU — restarting FrameGen."; return false; }
    if (!Fsr::support().supported) { fatal_ = error = Fsr::support().reason; return false; }
    ImagePtr A = prev.image, B = next.image;
    if (!A || !B || A->w != B->w || A->h != B->h || A->fmt != DXGI_FORMAT_R8G8B8A8_UNORM || B->fmt != DXGI_FORMAT_R8G8B8A8_UNORM) {
        error = "AMD FSR: frames must be RGBA and the same size"; return false;
    }
    if (phases.size() != 1) { error = "AMD FSR frame generation makes 2× only."; return false; }
    if (!ensure(B->w, B->h, error)) { fatal_ = error; return false; }
    bool hud = g_hudLock && !next.forWarmup;
    ImagePtr fa = A, fb = B;
    if (hud) std::tie(fa, fb) = HudStabilizer::get().prepare(prev.image, next.image, next.motion);
    double dtMs = std::max(1.0, (next.time - prev.time) * 1000);
    // FSR remembers the last frame it was given. If that isn't `prev` (first pair, after a gap or
    // a scene cut), give it `prev` first, from scratch.
    if (s_->lastFed != A) {
        if (!feed(fa, nullptr, 1, true, dtMs, error)) return false;
    }
    float scale = 1;
    ImagePtr field = flow_->motionField(prev, next, scale);
    bool ok = feed(fb, field, scale, false, dtMs, error);
    s_->lastFed = ok ? B : nullptr;
    if (!ok) return false;
    auto o = outPool_.acquire(B->w, B->h, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!o) { error = "AMD FSR: out of video memory"; return false; }
    {
        auto l = gpu.lock();
        gpu.ctx->CopyResource(o->tex.Get(), s_->out.t11.Get());
        if (wantRaw) {
            rawCopy = gpu.makeImage(o->w, o->h, DXGI_FORMAT_R8G8B8A8_UNORM);
            if (rawCopy) gpu.ctx->CopyResource(rawCopy->tex.Get(), o->tex.Get());
        }
    }
    out.push_back(o);
    if (hud) HudStabilizer::get().composite(out, prev.image, next.image, phases, next.motion);
    return true;
}
