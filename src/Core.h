// Core types shared by the whole app: clock, logging, frames, statistics, command-line options.
#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// ---- Clock: seconds on the QueryPerformanceCounter timeline (same clock as DWM / MF device timestamps).
double hostNow();
double qpcToSeconds(int64_t qpc);
int64_t secondsToQpc(double s);
void preciseSleepUntil(double t);   // high-resolution wait (timer + short spin)

// ---- Logging / strings
void logf(const char* fmt, ...);
std::string strf(const char* fmt, ...);
std::wstring strfw(const wchar_t* fmt, ...);
std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
std::string hr2str(HRESULT hr);
/// True if `path` has a valid Authenticode signature whose signer is `company` (e.g. L"NVIDIA Corporation").
bool signedBy(const std::wstring& path, const wchar_t* company);
bool icontains(const std::string& hay, const std::string& needle);

// ---- GPU images (see Gpu.h). Frames hold a shared reference; pools recycle images nobody holds.
struct GpuImage;
using ImagePtr = std::shared_ptr<GpuImage>;

/// A frame travelling through the pipeline. `time` is the frame's *content* time on the host
/// clock (hostNow), in seconds.
struct VideoFrame {
    ImagePtr image;          // NV12 from the capture card / test pattern; RGBA8 after the pipeline
    double time = 0;
    bool isGenerated = false;
    double readyTime = 0;    // host time at which this frame became available to the presenter
    double motion = 0;       // mean luma change since the previous real frame (0–255)
    bool forWarmup = false;  // synthetic frame used only to warm up an engine
    bool isRepeatPassThrough = false;   // passed on by the repeat safety net (unchanged picture)
    /// Sparse luma samples (every 8th pixel from (5, 3)) taken on the CPU by the capture card
    /// source — repeat detection then needs no GPU readback. Empty = sample on the GPU.
    std::shared_ptr<std::vector<uint8_t>> sparseLuma;
    uint64_t uploadFence = 0;
    int lumaCols = 0;           // columns of sparseLuma (rows = size / cols)
    bool admitted = false;      // already passed repeat detection (before upload)
    /// DLSS-G mode: estimated motion from the previous real frame (block field, RG16F, values in
    /// field pixels; × mvecScale = video pixels; pointing from this frame to where it came from
    /// is −field). Null = no motion known (first frame, scene cut).
    ImagePtr mvec;
    float mvecScale = 1;   // capture upload device fence value to wait for before reading `image`
    int width() const;
    int height() const;
};
using FramePtr = std::shared_ptr<VideoFrame>;

enum class EngineKind { Off, Blend, Fast, Quality, DlssG, Vfg, Fsr };
const char* engineId(EngineKind k);          // "off", "blend", "fast", "quality"
const char* engineLabel(EngineKind k);       // human-readable
const char* engineShortLabel(EngineKind k);  // for menus / the dashboard
std::optional<EngineKind> engineFromId(const std::string& s);

/// Thread-safe rolling statistics for the dashboard and --log.
class Stats {
public:
    void recordSource(double t);
    void recordPresent(double t);
    void recordInput(double t);
    void recordRefresh(double t);
    void recordForced();
    void recordRepeat();
    void resetInterp();
    void recordInterp(double ms);
    void recordSourceDrop();
    double takeMaxGapMs();

    struct Snapshot { double inputFPS, sourceFPS, outputFPS, refreshHz, interpAvgMs, interpMaxMs, delayMs; int dropped; };
    Snapshot snapshot();

    std::atomic<double> pipelineDelayMs{0};
    std::atomic<int> repeats{0}, forced{0};
    void setText(std::string& field, const std::string& v) { std::lock_guard<std::mutex> l(m_); field = v; }
    std::string get(const std::string& field) { std::lock_guard<std::mutex> l(m_); return field; }
    std::string sourceSize, sourceName, interpolatorName, lastError;   // guarded by m_ (use setText/get)
    void setError(const std::string& e) { setText(lastError, e); }
    void clearError() { setText(lastError, ""); }
    std::string error() { return get(lastError); }

private:
    static void push(std::deque<double>& a, double v, size_t cap) { a.push_back(v); while (a.size() > cap) a.pop_front(); }
    static double rate(const std::deque<double>& a);
    std::mutex m_;
    std::deque<double> source_, present_, refresh_, input_, interp_;
    int dropped_ = 0;
    double maxGap_ = 0;
};

/// Command-line options (override saved preferences for this run).
struct Options {
    std::string source = "auto";       // auto | capture | synthetic
    std::string device;                // index or (part of) a name
    int width = 0, height = 0;         // capture / test pattern size; 0 = automatic
    double fps = 0;                    // capture rate / test pattern rate; 0 = automatic
    int repeat = 1;                    // test pattern: send each frame N times
    std::optional<EngineKind> engine;  // --interp
    std::optional<int> mult;           // --mult
    bool audio = true;
    bool listDevices = false, bench = false, checkColors = false, checkHud = false, checkQuality = false;
    bool afterGpuReset = false;   // started again by FrameGen after a GPU reset
    std::string dumpDir, realDir, liveDumpDir, snapPath, logFile, uiShot;
    double quitAfter = 0;
    double stress = 0;   // --stress <seconds>
    bool checkDlss = false;
    bool activate = false;
    bool experimentalDlss = false;   // --experimental-dlss (no longer needed; accepted for old command lines)   // --activate: take keyboard focus even when launched from a terminal (tests)
    double maxHz = 0;
    bool dedupe = true, edgeFix = true, hudLock = true;
    std::string scene = "action";      // action | still | static | hud | stopgo
    bool log = false, testTone = false, cycleModes = false, noise = false;
    std::optional<float> volume;
    double simulateSlowMs = 0;
    double gameFPS = 0, signalHz = 0;
    bool windowed = false, fullscreen = false;
    bool sourceGiven = false;

    static Options parse(int argc, wchar_t** argv);
};

extern Options g_opt;

// Global toggles (mirroring the Mac app).
extern std::atomic<bool> g_hudLock;        // HUD & text stabiliser
extern std::atomic<bool> g_repairEdges;    // border repair after generation
extern std::atomic<double> g_simulatedSlowMs;
