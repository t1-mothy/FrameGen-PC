#include "VfgEngine.h"
#include "Hud.h"
#include "Prefs.h"
#include <softpub.h>
#include <wintrust.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <regex>

// ---- The SDK's C API, declared here (layout and values from NVIDIA's MIT-licensed headers:
// nvCVImage.h / nvVideoEffects.h in github.com/NVIDIA-Maxine/Maxine-VFX-SDK).
namespace {
struct NvImg {
    unsigned width = 0, height = 0;
    int pitch = 0;
    int pixelFormat = 0, componentType = 0;
    unsigned char pixelBytes = 0, componentBytes = 0, numComponents = 0, planar = 0, gpuMem = 0, colorspace = 0, reserved[2]{};
    void* pixels = nullptr;
    void* deletePtr = nullptr;
    void (*deleteProc)(void*) = nullptr;
    unsigned long long bufferBytes = 0;
};
static_assert(sizeof(NvImg) == 64, "NvCVImage layout");
using Status = int;
using Handle = void*;
using CUstream = struct CUstream_st*;
constexpr int kRGBA = 6, kU8 = 1;
constexpr unsigned kChunky = 0, kGpuMem = 1;
// Selectors (strings the filter answers to).
constexpr const char* kEffect = "VideoFrameGeneration";
constexpr const char* kIn0 = "SrcImage0";
constexpr const char* kIn1 = "SrcImage1";
constexpr const char* kOut = "DstImage0";
constexpr const char* kModelDir = "ModelDir";
constexpr const char* kStream = "CudaStream";
constexpr const char* kWidth = "InputWidth";
constexpr const char* kHeight = "InputHeight";
constexpr const char* kMode = "Mode";
constexpr const char* kMultiplier = "FrameMultiplier";
constexpr const char* kTimestep = "Timestep";
constexpr const char* kAutoShot = "AutomaticShotChangeDetectionEnabled";

struct Api {
    bool ok = false;
    std::string error;
    Status(__cdecl* CreateEffect)(const char*, Handle*) = nullptr;
    void(__cdecl* DestroyEffect)(Handle) = nullptr;
    Status(__cdecl* SetU32)(Handle, const char*, unsigned) = nullptr;
    Status(__cdecl* SetF32)(Handle, const char*, float) = nullptr;
    Status(__cdecl* SetString)(Handle, const char*, const char*) = nullptr;
    Status(__cdecl* SetCudaStream)(Handle, const char*, CUstream) = nullptr;
    Status(__cdecl* SetImage)(Handle, const char*, NvImg*) = nullptr;
    Status(__cdecl* Load)(Handle) = nullptr;
    Status(__cdecl* Run)(Handle, int) = nullptr;
    Status(__cdecl* StreamCreate)(CUstream*) = nullptr;
    Status(__cdecl* StreamSync)(CUstream) = nullptr;
    void(__cdecl* StreamDestroy)(CUstream) = nullptr;
    Status(__cdecl* GetVersion)(unsigned*) = nullptr;
    Status(__cdecl* ImgAlloc)(NvImg*, unsigned, unsigned, int, int, unsigned, unsigned, unsigned) = nullptr;
    void(__cdecl* ImgDealloc)(NvImg*) = nullptr;
    Status(__cdecl* ImgFromD3D11)(NvImg*, ID3D11Texture2D*) = nullptr;
    Status(__cdecl* ImgMap)(NvImg*, CUstream) = nullptr;
    Status(__cdecl* ImgUnmap)(NvImg*, CUstream) = nullptr;
    Status(__cdecl* ImgTransfer)(const NvImg*, NvImg*, float, CUstream, NvImg*) = nullptr;
    const char*(__cdecl* ErrorString)(Status) = nullptr;
    std::string err(Status s) const { const char* m = ErrorString ? ErrorString(s) : nullptr; return strf("%s (%d)", m ? m : "error", s); }
};

const wchar_t* kDlls[] = {L"NVVideoEffects.dll", L"NVCVImage.dll", L"cudart64_12.dll", L"nvVFXVideoFrameGeneration.dll"};

/// True if `path` has a valid Authenticode signature from NVIDIA Corporation.
bool signedByNvidia(const std::wstring& path) {
    WINTRUST_FILE_INFO fi{sizeof fi};
    fi.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{sizeof wd};
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    LONG r = WinVerifyTrust(nullptr, &action, &wd);
    bool nvidia = false;
    if (r == ERROR_SUCCESS) {
        if (auto* prov = WTHelperProvDataFromStateData(wd.hWVTStateData))
            if (auto* signer = WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0); signer && signer->csCertChain && signer->pasCertChain) {
                wchar_t name[256] = {};
                CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
                nvidia = wcscmp(name, L"NVIDIA Corporation") == 0;
            }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    return nvidia;
}

std::wstring exeDir() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).parent_path().wstring();
}

std::vector<std::wstring> candidateDirs() {
    std::vector<std::wstring> d;
    std::string pref = Prefs::get().str("vfgRuntime", "");
    if (!pref.empty()) d.push_back(widen(pref));
    d.push_back(exeDir() + L"\\nvidia-vfx");
    for (const wchar_t* var : {L"NV_VIDEO_EFFECTS_PATH", L"NVVFX_SDK_PATH"}) {
        wchar_t v[MAX_PATH]; DWORD n = GetEnvironmentVariableW(var, v, MAX_PATH);
        if (n > 0 && n < MAX_PATH) { d.push_back(v); d.push_back(std::wstring(v) + L"\\bin"); }
    }
    wchar_t pf[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) d.push_back(std::wstring(pf) + L"\\NVIDIA Corporation\\NVIDIA Video Effects");
    return d;
}

/// Finds the VFG feature DLL in `dir` itself or in the SDK's features\<name>\bin layout.
std::wstring featurePath(const std::wstring& dir) {
    for (auto p : {dir + L"\\nvVFXVideoFrameGeneration.dll", dir + L"\\features\\nvvfxvideoframegeneration\\bin\\nvVFXVideoFrameGeneration.dll",
                   dir + L"\\features\\nvVFXVideoFrameGeneration\\bin\\nvVFXVideoFrameGeneration.dll"})
        if (std::filesystem::exists(p)) return p;
    return {};
}

Vfg::Support g_support;
bool g_supportValid = false;
std::mutex g_supportLock;

/// Loads and verifies the runtime once per process (DLLs stay loaded).
Api& api() {
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        const auto& sup = Vfg::support();
        if (!sup.supported) { a.error = sup.reason; return; }
        const std::wstring& dir = sup.dir;
        std::wstring feature = featurePath(dir);
        // Verify every DLL before loading any of them (also checks the files are complete).
        for (auto* n : kDlls) {
            std::wstring p = std::wstring(n) == L"nvVFXVideoFrameGeneration.dll" ? feature : dir + L"\\" + n;
            if (!std::filesystem::exists(p)) { a.error = "The NVIDIA VFG runtime is incomplete (" + narrow(n) + " is missing)."; return; }
            if (!getenv("FG_VFG_UNSIGNED") && !signedByNvidia(p)) { a.error = narrow(n) + " is not signed by NVIDIA — refusing to load it."; return; }
        }
        // Dependencies first (by full path) so NVVideoEffects finds them already loaded.
        auto load = [&](const std::wstring& p) { return LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); };
        HMODULE cudart = load(dir + L"\\cudart64_12.dll");
        HMODULE cv = load(dir + L"\\NVCVImage.dll");
        HMODULE feat = load(feature);
        HMODULE fx = load(dir + L"\\NVVideoEffects.dll");
        if (!cudart || !cv || !fx || !feat) { a.error = strf("Couldn't load the NVIDIA VFG runtime (error %lu). Is the NVIDIA driver up to date?", GetLastError()); return; }
        auto get = [&](auto& fn, HMODULE m, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(m, name)); return fn != nullptr; };
        bool ok = get(a.CreateEffect, fx, "NvVFX_CreateEffect") && get(a.DestroyEffect, fx, "NvVFX_DestroyEffect") &&
                  get(a.SetU32, fx, "NvVFX_SetU32") && get(a.SetF32, fx, "NvVFX_SetF32") && get(a.SetString, fx, "NvVFX_SetString") &&
                  get(a.SetCudaStream, fx, "NvVFX_SetCudaStream") && get(a.SetImage, fx, "NvVFX_SetImage") && get(a.Load, fx, "NvVFX_Load") &&
                  get(a.Run, fx, "NvVFX_Run") && get(a.StreamCreate, fx, "NvVFX_CudaStreamCreate") && get(a.StreamDestroy, fx, "NvVFX_CudaStreamDestroy") &&
                  get(a.ImgAlloc, cv, "NvCVImage_Alloc") && get(a.ImgDealloc, cv, "NvCVImage_Dealloc") &&
                  get(a.ImgFromD3D11, cv, "NvCVImage_InitFromD3D11Texture") && get(a.ImgMap, cv, "NvCVImage_MapResource") &&
                  get(a.ImgUnmap, cv, "NvCVImage_UnmapResource") && get(a.ImgTransfer, cv, "NvCVImage_Transfer");
        get(a.GetVersion, fx, "NvVFX_GetVersion");
        get(a.StreamSync, fx, "NvVFX_CudaStreamSynchronize");
        get(a.ErrorString, cv, "NvCV_GetErrorStringFromCode");
        if (!ok) { a.error = "This NVIDIA VFG runtime is a different version FrameGen doesn't understand."; return; }
        unsigned v = 0;
        if (a.GetVersion && a.GetVersion(&v) == 0) logf("[vfg] NVIDIA Video Effects %u.%u.%u.%u from %s", v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255, narrow(dir).c_str());
        a.ok = true;
    });
    return a;
}
}

namespace Vfg {
const char* kGpuUnsupported = "Your GPU does not support NVIDIA VFG. Frame generation is disabled, please select another option.";

/// VFG runs on Ada (RTX 40) and Blackwell (RTX 50) GPUs and newer.
static bool gpuCanRunVfg() {
    auto& gpu = Gpu::get();
    if (!gpu.dev || !gpu.isNvidia) return false;
    if (getenv("FG_VFG_NOGPU")) return false;   // test aid: behave like an unsupported GPU
    if (getenv("FG_VFG_ANYGPU")) return true;
    static const std::regex re(R"(RTX\s*[4-9]0[5-9]0|Ada Generation|Blackwell)", std::regex::icase);
    return std::regex_search(gpu.adapterName, re);
}

bool isRuntimeFolder(const std::wstring& dir) {
    return std::filesystem::exists(dir + L"\\NVVideoEffects.dll") && std::filesystem::exists(dir + L"\\NVCVImage.dll") && !featurePath(dir).empty();
}
const Support& support() {
    std::lock_guard<std::mutex> l(g_supportLock);
    if (g_supportValid) return g_support;
    Support s;
    for (auto& d : candidateDirs()) if (isRuntimeFolder(d)) { s.dir = d; break; }
    s.gpuOk = gpuCanRunVfg();
    if (!s.gpuOk) s.reason = kGpuUnsupported;
    else if (s.dir.empty()) s.reason = "NVIDIA VFG's files are missing (reinstall FrameGen, or choose the VFG folder in Settings › Frame Generation). Frame generation is disabled, please select another option.";
    else s.supported = true;
    g_support = s; g_supportValid = true;
    return g_support;
}
void refresh() { std::lock_guard<std::mutex> l(g_supportLock); g_supportValid = false; }
}

// ---- Engine

struct VfgEngine::State {
    Handle fx = nullptr;
    CUstream stream = nullptr;
    int w = 0, h = 0;
    NvImg buf0, buf1, dst, tmp;          // CUDA buffers the filter reads / writes
    ComPtr<ID3D11Texture2D> inTex[2];    // D3D11 side of the interop
    NvImg inImg[2];
    std::vector<ComPtr<ID3D11Texture2D>> outTex;
    std::vector<NvImg> outImg;
};

VfgEngine::VfgEngine(Model m) : model_(m) {}
VfgEngine::~VfgEngine() { release(); }

void VfgEngine::release() {
    if (!s_) return;
    auto& a = api();
    if (a.ok) {
        // Let queued CUDA work finish (without holding the GPU lock), then unregister the shared
        // Direct3D textures under the lock so no Direct3D work runs alongside — switching modes
        // while VFG ran could otherwise end in a GPU reset.
        if (s_->stream && a.StreamSync) a.StreamSync(s_->stream);
        if (s_->fx) a.DestroyEffect(s_->fx);
        for (NvImg* im : {&s_->buf0, &s_->buf1, &s_->dst, &s_->tmp}) a.ImgDealloc(im);
        {
            auto l = Gpu::get().lock();
            for (NvImg* im : {&s_->inImg[0], &s_->inImg[1]}) a.ImgDealloc(im);
            for (auto& im : s_->outImg) a.ImgDealloc(&im);
        }
        if (s_->stream) a.StreamDestroy(s_->stream);
    }
    delete s_;
    s_ = nullptr;
}

bool VfgEngine::ensure(int w, int h, size_t outputs, std::string& error) {
    auto& a = api();
    if (!a.ok) { error = a.error; return false; }
    auto& gpu = Gpu::get();
    if (!s_ || s_->w != w || s_->h != h) {
        release();
        s_ = new State;
        s_->w = w; s_->h = h;
        double t0 = hostNow();
        Status st;
        auto fail = [&](const char* what) {
            if (st == -17 /* NVCV_ERR_UNSUPPORTEDGPU */) error = Vfg::kGpuUnsupported;
            else if (st == -19 /* NVCV_ERR_UNSUPPORTEDDRIVER */) error = "Your NVIDIA driver is too old for NVIDIA VFG — update it at nvidia.com/drivers. Frame generation is disabled, please select another option.";
            else error = strf("NVIDIA VFG: %s failed: %s. Frame generation is disabled, please select another option.", what, a.err(st).c_str());
            release(); return false;
        };
        if ((st = a.CreateEffect(kEffect, &s_->fx)) != 0) return fail("starting the effect");
        a.SetString(s_->fx, kModelDir, narrow(Vfg::support().dir).c_str());
        if ((st = a.StreamCreate(&s_->stream)) != 0) return fail("creating a CUDA stream");
        a.SetCudaStream(s_->fx, kStream, s_->stream);
        a.SetU32(s_->fx, kWidth, unsigned(w));
        a.SetU32(s_->fx, kHeight, unsigned(h));
        a.SetU32(s_->fx, kMode, unsigned(model_));
        a.SetU32(s_->fx, kAutoShot, 1);
        if ((st = a.Load(s_->fx)) != 0) return fail("loading the AI model");
        for (NvImg* im : {&s_->buf0, &s_->buf1, &s_->dst})
            if ((st = a.ImgAlloc(im, unsigned(w), unsigned(h), kRGBA, kU8, kChunky, kGpuMem, 32)) != 0) return fail("allocating video memory");
        if ((st = a.SetImage(s_->fx, kOut, &s_->dst)) != 0) return fail("setting the output");
        a.SetU32(s_->fx, kMultiplier, 0);   // explicit timestep per run
        for (int i = 0; i < 2; i++) {
            auto img = gpu.makeTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
            if (!img) { error = "NVIDIA VFG: out of video memory"; release(); return false; }
            s_->inTex[i] = img->tex;
            auto l = gpu.lock();
            if ((st = a.ImgFromD3D11(&s_->inImg[i], s_->inTex[i].Get())) != 0) return fail("sharing frames with CUDA");
        }
        logf("[vfg] ready at %dx%d (model %d) in %.0f ms", w, h, int(model_), (hostNow() - t0) * 1000);
    }
    while (s_->outImg.size() < outputs) {
        auto img = gpu.makeTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        if (!img) { error = "NVIDIA VFG: out of video memory"; return false; }
        s_->outTex.push_back(img->tex);
        s_->outImg.emplace_back();
        auto l = gpu.lock();
        if (Status st = a.ImgFromD3D11(&s_->outImg.back(), img->tex.Get()); st != 0) {
            error = "NVIDIA VFG: sharing frames with CUDA failed: " + a.err(st);
            s_->outImg.pop_back(); s_->outTex.pop_back();
            return false;
        }
    }
    return true;
}

bool VfgEngine::interpolate(const VideoFrame& prev, const VideoFrame& next, const std::vector<float>& phases,
                            std::vector<ImagePtr>& out, std::string& error) {
    if (!fatal_.empty()) { error = fatal_; return false; }
    auto& gpu = Gpu::get();
    if (gpu.deviceLost) { error = "The graphics driver reset the GPU — restarting FrameGen."; return false; }
    ImagePtr A = prev.image, B = next.image;
    if (!A || !B || A->w != B->w || A->h != B->h || A->fmt != DXGI_FORMAT_R8G8B8A8_UNORM || B->fmt != DXGI_FORMAT_R8G8B8A8_UNORM) {
        error = "NVIDIA VFG: frames must be RGBA and the same size"; return false;
    }
    if (!ensure(B->w, B->h, phases.size(), error)) {
        if (!s_) fatal_ = error;   // the model couldn't load: don't retry every frame
        return false;
    }
    auto& a = api();
    bool hud = g_hudLock && !next.forWarmup;
    if (hud) std::tie(A, B) = HudStabilizer::get().prepare(prev.image, next.image, next.motion);
    Status st = 0;
    auto cuda = [&](Status s, const char* what) { if (s != 0 && st == 0) { st = s; error = strf("NVIDIA VFG: %s failed: %s", what, a.err(s).c_str()); } return s == 0; };
    static double lockHeld = 0, lockMax = 0, lockAt = 0; static int lockN = 0;
    auto held = [&](double t0) { double d = hostNow() - t0; lockHeld += d; lockMax = std::max(lockMax, d); lockN++; };
    {
        double t0 = hostNow();
        struct H { std::function<void()> f; ~H() { f(); } } h{[&] { held(t0); }};
        auto l = gpu.lock();
        gpu.ctx->CopyResource(s_->inTex[0].Get(), A->tex.Get());
        gpu.ctx->CopyResource(s_->inTex[1].Get(), B->tex.Get());
        // Mapping orders CUDA after the D3D11 work submitted so far.
        cuda(a.ImgMap(&s_->inImg[0], s_->stream), "map") && cuda(a.ImgMap(&s_->inImg[1], s_->stream), "map");
    }
    if (st == 0) {
        cuda(a.ImgTransfer(&s_->inImg[0], &s_->buf0, 1.f, s_->stream, &s_->tmp), "copy in") &&
            cuda(a.ImgTransfer(&s_->inImg[1], &s_->buf1, 1.f, s_->stream, &s_->tmp), "copy in");
        { double t0 = hostNow(); auto l = gpu.lock(); a.ImgUnmap(&s_->inImg[0], s_->stream); a.ImgUnmap(&s_->inImg[1], s_->stream); held(t0); }
    }
    if (st == 0) {
        a.SetImage(s_->fx, kIn0, &s_->buf0);
        a.SetImage(s_->fx, kIn1, &s_->buf1);
        for (size_t i = 0; i < phases.size() && st == 0; i++) {
            a.SetF32(s_->fx, kTimestep, std::clamp(phases[i], 0.01f, 0.99f));
            if (!cuda(a.Run(s_->fx, 1), "generating")) break;
            { double t0 = hostNow(); auto l = gpu.lock(); bool ok = cuda(a.ImgMap(&s_->outImg[i], s_->stream), "map"); held(t0); if (!ok) break; }
            cuda(a.ImgTransfer(&s_->dst, &s_->outImg[i], 1.f, s_->stream, &s_->tmp), "copy out");
            { double t0 = hostNow(); auto l = gpu.lock(); a.ImgUnmap(&s_->outImg[i], s_->stream); held(t0); }   // D3D11 work after this waits for CUDA
        }
    }
    if (getenv("FG_TRACE") && hostNow() - lockAt > 1) { logf("[vfg] interop holds the GPU lock: avg %.2f ms, max %.2f ms (%d holds)", lockHeld / std::max(lockN, 1) * 1000, lockMax * 1000, lockN); lockHeld = lockMax = 0; lockN = 0; lockAt = hostNow(); }
    if (st != 0) return false;
    {
        auto l = gpu.lock();
        for (size_t i = 0; i < phases.size(); i++) {
            auto o = outPool_.acquire(B->w, B->h, DXGI_FORMAT_R8G8B8A8_UNORM);
            if (!o) { error = "NVIDIA VFG: out of video memory"; return false; }
            gpu.ctx->CopyResource(o->tex.Get(), s_->outTex[i].Get());
            out.push_back(o);
        }
        if (wantRaw && !out.empty()) {
            rawCopy = gpu.makeImage(out[0]->w, out[0]->h, DXGI_FORMAT_R8G8B8A8_UNORM);
            if (rawCopy) gpu.ctx->CopyResource(rawCopy->tex.Get(), out[0]->tex.Get());
        }
    }
    if (hud) HudStabilizer::get().composite(out, prev.image, next.image, phases, next.motion);
    return true;
}
