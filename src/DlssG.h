// NVIDIA DLSS Frame Generation through Streamline (optional engine, RTX 40/50).
//
// Streamline is loaded at runtime from the "streamline" folder next to FrameGen.exe, so the app
// works without it and DLSS-G is simply not offered. Streamline is MIT licensed; the DLSS-G /
// NGX binaries are NVIDIA's (see third_party/streamline/*.license.txt). Get them with
// tools/get_streamline.ps1 — they are not part of this repository.
#pragma once
#include "Core.h"

#ifdef FG_HAVE_STREAMLINE
#include <d3d12.h>
#include <dxgi1_6.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#endif

namespace DlssG {
struct Support {
    bool loaded = false;       // Streamline DLLs found, NVIDIA-signed and initialised
    bool supported = false;    // DLSS-G usable on this GPU / OS / driver
    bool gpuUnsupported = false;   // the graphics card itself can't run it (not RTX 40 / 50)
    int maxFramesToGenerate = 0;
    std::string reason;        // why not, in plain English
};
/// Checks (once) whether DLSS Frame Generation can run here. Initialises Streamline on success
/// (kept for the rest of the run).
const Support& support();
/// Starts the support check on a background thread (it takes ~1 s); support() waits for it.
void checkInBackground();
/// Generated frames per real frame this GPU's DLSS-G allows (RTX 40: 1, RTX 50: up to 5),
/// known once DLSS-G has run; 0 = not known yet.
int maxGenerateKnown();
void setMaxGenerateKnown(int n);
/// Starts Streamline. Must run before any DXGI / Direct3D call in the process (SDK requirement).
void preInit();
void startPreInit();   // the same, on a background thread (preInit() / support() wait for it)
/// --check-dlss: prints a detailed report.
int runCheck();
std::wstring streamlineDir();

#ifdef FG_HAVE_STREAMLINE
/// Streamline entry points and the Direct3D 12 device it's bound to (one per process).
struct Runtime {
    HMODULE dll = nullptr;
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slIsFeatureSupported* isSupported = nullptr;
    PFun_slSetD3DDevice* setDevice = nullptr;
    PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
    PFun_slGetFeatureRequirements* getRequirements = nullptr;
    PFun_slUpgradeInterface* upgrade = nullptr;
    PFun_slGetNativeInterface* getNative = nullptr;
    PFun_slSetTagForFrame* setTag = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slGetNewFrameToken* newFrameToken = nullptr;
    PFun_slSetFeatureLoaded* setFeatureLoaded = nullptr;
    PFun_slFreeResources* freeResources = nullptr;
    PFun_slDLSSGSetOptions* dlssgSetOptions = nullptr;
    PFun_slDLSSGGetState* dlssgGetState = nullptr;
    PFun_slPCLSetMarker* pclMarker = nullptr;
    PFun_slReflexSetOptions* reflexSetOptions = nullptr;
    // Direct3D 12 (created on first use, on the same adapter as the D3D11 device)
    ComPtr<ID3D12Device> device;        // native
    ComPtr<ID3D12Device> proxyDevice;   // Streamline proxy (only for CreateCommandQueue)
    ComPtr<ID3D12CommandQueue> queue;   // created through the proxy
    bool ensureDevice(std::string& error);
};
Runtime* runtime();   // null unless support().loaded
std::string resultText(sl::Result r);
#endif
}
