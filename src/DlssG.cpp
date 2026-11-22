#include "DlssG.h"
#include "Gpu.h"

#ifdef FG_HAVE_STREAMLINE
#include <sl_security.h>
#endif

namespace DlssG {

std::wstring streamlineDir() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe; d = d.substr(0, d.find_last_of(L"\\/"));
    // FG_SL_DEV=1: NVIDIA's development Streamline/DLSS-G builds (input validation, verbose logs).
    return d + (getenv("FG_SL_DEV") ? L"\\streamline_dev" : L"\\streamline");
}

#ifdef FG_HAVE_STREAMLINE
std::string resultText(sl::Result r) {
    switch (r) {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorDriverOutOfDate: return "the NVIDIA driver is too old — update it";
    case sl::Result::eErrorOSOutOfDate: return "Windows is too old (needs Windows 10 2004 or later)";
    case sl::Result::eErrorOSDisabledHWS: return "Hardware-accelerated GPU scheduling is off (Windows Settings › System › Display › Graphics › Change default graphics settings), then restart";
    case sl::Result::eErrorNoSupportedAdapterFound:
    case sl::Result::eErrorAdapterNotSupported: return "this graphics card doesn't support DLSS Frame Generation (needs an NVIDIA RTX 40 or 50 series)";
    case sl::Result::eErrorFeatureMissing: return "the DLSS-G plugin is missing from the streamline folder";
    default: return strf("Streamline error %d", int(r));
    }
}

static Runtime g_rt;
static bool g_rtReady = false;

static void slLog(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eError || g_opt.log) logf("[streamline] %s", msg);
}

template <typename T> static bool sym(HMODULE m, T*& out, const char* name) { out = (T*)GetProcAddress(m, name); return out != nullptr; }

static bool loadRuntime(Runtime& a, bool verbose, std::string& why) {
    std::wstring dir = streamlineDir(), path = dir + L"\\sl.interposer.dll";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) { why = "DLSS Frame Generation files aren't installed (streamline folder missing)"; return false; }
    // Only load NVIDIA-signed Streamline (security requirement of the SDK).
    if (!getenv("FG_SL_DEV") && !sl::security::verifyEmbeddedSignature(path.c_str())) { why = "sl.interposer.dll is not signed by NVIDIA — refusing to load it"; return false; }
    a.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!a.dll) { why = "could not load sl.interposer.dll"; return false; }
    bool ok = sym(a.dll, a.init, "slInit") && sym(a.dll, a.shutdown, "slShutdown") && sym(a.dll, a.isSupported, "slIsFeatureSupported") &&
              sym(a.dll, a.setDevice, "slSetD3DDevice") && sym(a.dll, a.getFeatureFunction, "slGetFeatureFunction") &&
              sym(a.dll, a.getRequirements, "slGetFeatureRequirements") && sym(a.dll, a.upgrade, "slUpgradeInterface") &&
              sym(a.dll, a.getNative, "slGetNativeInterface") && sym(a.dll, a.setTag, "slSetTagForFrame") &&
              sym(a.dll, a.setConstants, "slSetConstants") && sym(a.dll, a.newFrameToken, "slGetNewFrameToken") &&
              sym(a.dll, a.setFeatureLoaded, "slSetFeatureLoaded") && sym(a.dll, a.freeResources, "slFreeResources");
    if (!ok) { why = "sl.interposer.dll is missing functions"; return false; }
    static std::wstring dirKeep = dir;
    static const wchar_t* paths[] = {dirKeep.c_str()};
    static sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = verbose ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths; pref.numPathsToPlugins = 1;
    pref.logMessageCallback = slLog;
    // Manual hooking: only our D3D12 objects go through Streamline; no over-the-air downloads.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features; pref.numFeaturesToLoad = 3;
    pref.engine = sl::EngineType::eCustom; pref.engineVersion = FG_VERSION; pref.projectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result r = a.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) { why = "Streamline didn't start: " + resultText(r); return false; }
    return true;
}

bool Runtime::ensureDevice(std::string& error) {
    if (queue) return true;
    if (getenv("FG_DRED")) {   // diagnostics: record what the GPU was doing if the device is lost
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            logf("[dlss] DRED diagnostics on");
        }
    }
    if (FAILED(D3D12CreateDevice(Gpu::get().adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) { error = "Direct3D 12 isn't available"; return false; }
    proxyDevice = device;
    if (upgrade((void**)proxyDevice.GetAddressOf()) != sl::Result::eOk) { error = "Streamline couldn't attach to the Direct3D 12 device"; return false; }
    sl::Result r = setDevice(device.Get());
    if (r != sl::Result::eOk) { error = "Streamline: " + resultText(r); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(proxyDevice->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { error = "could not create a Direct3D 12 queue"; return false; }
    void* fn = nullptr;
    if (getFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fn) == sl::Result::eOk) dlssgSetOptions = (PFun_slDLSSGSetOptions*)fn;
    if (getFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fn) == sl::Result::eOk) dlssgGetState = (PFun_slDLSSGGetState*)fn;
    if (getFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fn) == sl::Result::eOk) pclMarker = (PFun_slPCLSetMarker*)fn;
    if (getFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fn) == sl::Result::eOk) reflexSetOptions = (PFun_slReflexSetOptions*)fn;
    if (!dlssgSetOptions || !dlssgGetState || !pclMarker) { error = "the DLSS-G / PCL plugins didn't load"; return false; }
    if (reflexSetOptions) { sl::ReflexOptions ro{}; ro.mode = sl::ReflexMode::eLowLatency; reflexSetOptions(ro); }
    return true;
}

Runtime* runtime() { return g_rtReady ? &g_rt : nullptr; }
#endif

static std::string g_preInitError;
static bool g_preInitDone = false;
void preInit() {
#ifdef FG_HAVE_STREAMLINE
    if (g_preInitDone) return;
    g_preInitDone = true;
    if (loadRuntime(g_rt, getenv("FG_SL_DEV") != nullptr, g_preInitError)) g_rtReady = true;
#endif
}

const Support& support() {
    static Support s = [] {
        Support x;
#ifdef FG_HAVE_STREAMLINE
        preInit();
        if (!g_rtReady) { x.reason = g_preInitError; return x; }
        x.loaded = true;
        sl::AdapterInfo ai{};
        LUID luid = Gpu::get().adapterLuid;
        ai.deviceLUID = (uint8_t*)&luid; ai.deviceLUIDSizeInBytes = sizeof(LUID);
        sl::Result r = g_rt.isSupported(sl::kFeatureDLSS_G, ai);
        if (r != sl::Result::eOk) { x.reason = resultText(r); return x; }
        x.supported = true;
        x.maxFramesToGenerate = 3;   // refined when the D3D12 device exists (RTX 50: up to 5)
#else
        x.reason = "this build was made without the Streamline SDK";
#endif
        return x;
    }();
    return s;
}

int runCheck() {
#ifdef FG_HAVE_STREAMLINE
    logf("DLSS Frame Generation check (Streamline from %s)", narrow(streamlineDir()).c_str());
#endif
    Support s = support();
#ifdef FG_HAVE_STREAMLINE
    if (s.supported) {
        std::string err;
        if (g_rt.ensureDevice(err)) {
            sl::DLSSGState st{};
            if (g_rt.dlssgGetState(sl::ViewportHandle(0), st, nullptr) == sl::Result::eOk) s.maxFramesToGenerate = int(st.numFramesToGenerateMax);
        } else logf("  D3D12: %s", err.c_str());
        sl::FeatureRequirements req{};
        if (g_rt.getRequirements(sl::kFeatureDLSS_G, req) == sl::Result::eOk)
            logf("  driver %u.%u (needs %u.%u), Windows %u.%u.%u, hardware GPU scheduling required: %s",
                 req.driverVersionDetected.major, req.driverVersionDetected.minor, req.driverVersionRequired.major, req.driverVersionRequired.minor,
                 req.osVersionDetected.major, req.osVersionDetected.minor, req.osVersionDetected.build,
                 (req.flags & sl::FeatureRequirementFlags::eHardwareSchedulingRequired) ? "yes" : "no");
    }
#endif
    logf("  Streamline loaded: %s", s.loaded ? "yes" : "no");
    logf("  DLSS-G supported: %s%s", s.supported ? "yes" : "no", s.supported ? strf(" — up to %d generated frames per real frame (%dx)", s.maxFramesToGenerate, s.maxFramesToGenerate + 1).c_str() : "");
    if (!s.reason.empty()) logf("  reason: %s", s.reason.c_str());
    return s.supported ? 0 : 1;
}
}
