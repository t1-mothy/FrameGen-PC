#include "Core.h"
#include "Gpu.h"
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cwctype>
#include <softpub.h>
#include <wintrust.h>

Options g_opt;
std::atomic<bool> g_hudLock{true};
std::atomic<bool> g_repairEdges{true};
std::atomic<double> g_simulatedSlowMs{0};

static double qpcFreq() {
    static double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return double(q.QuadPart); }();
    return f;
}
double hostNow() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return double(c.QuadPart) / qpcFreq(); }
double qpcToSeconds(int64_t qpc) { return double(qpc) / qpcFreq(); }
int64_t secondsToQpc(double s) { return int64_t(s * qpcFreq()); }

void preciseSleepUntil(double t) {
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        double left = t - hostNow();
        if (left <= 0) return;
        if (left > 0.0015 && timer) {
            LARGE_INTEGER due; due.QuadPart = -LONGLONG((left - 0.001) * 1e7);
            SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
            WaitForSingleObject(timer, 50);
        } else if (left > 0.0002) {
            Sleep(0);
        } else {
            YieldProcessor();
        }
    }
}

static std::mutex g_logLock;
static FILE* g_logFile = nullptr;
void logf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(nullptr, 0, fmt, ap); va_end(ap);
    std::string s(size_t(std::max(n, 0)) + 1, char(0));
    vsnprintf(s.data(), s.size(), fmt, ap2); va_end(ap2);
    const char* buf = s.c_str();
    std::lock_guard<std::mutex> l(g_logLock);
    fputs(buf, stdout);
    if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', stdout);
    fflush(stdout);
    if (!g_logFile && !g_opt.logFile.empty()) g_logFile = _wfopen(widen(g_opt.logFile).c_str(), L"a");
    if (g_logFile) { fputs(buf, g_logFile); if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', g_logFile); fflush(g_logFile); }
    OutputDebugStringA(buf);
}

std::wstring strfw(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap; va_start(ap, fmt); _vsnwprintf_s(buf, _TRUNCATE, fmt, ap); va_end(ap);
    return buf;
}

std::string strf(const char* fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    return buf;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string hr2str(HRESULT hr) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, hr, 0, (LPWSTR)&msg, 0, nullptr);
    std::string s = strf("0x%08X", unsigned(hr));
    if (msg) { std::string m = narrow(msg); LocalFree(msg); while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back(); s += " " + m; }
    return s;
}
bool icontains(const std::string& hay, const std::string& needle) {
    auto low = [](std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); }); return s; };
    return low(hay).find(low(needle)) != std::string::npos;
}

int VideoFrame::width() const { return image ? image->w : 0; }
int VideoFrame::height() const { return image ? image->h : 0; }

const char* engineId(EngineKind k) {
    switch (k) { case EngineKind::Off: return "off"; case EngineKind::Blend: return "blend"; case EngineKind::Fast: return "fast"; case EngineKind::Quality: return "quality"; case EngineKind::DlssG: return "dlss"; case EngineKind::Vfg: return "vfg"; case EngineKind::Fsr: return "fsr"; }
    return "off";
}
const char* engineLabel(EngineKind k) {
    switch (k) {
    case EngineKind::Off: return "Off (source only)";
    case EngineKind::Blend: return "Blend (cross-fade)";
    case EngineKind::Fast: return "Low latency (GPU motion flow)";
    case EngineKind::Quality: return "Smoothest (GPU motion flow, quality)";
    case EngineKind::DlssG: return "DLSS Frame Generation (NVIDIA)";
    case EngineKind::Vfg: return "NVIDIA VFG (AI video frame generation)";
    case EngineKind::Fsr: return "AMD FSR Frame Generation";
    }
    return "";
}
const char* engineShortLabel(EngineKind k) {
    switch (k) {
    case EngineKind::Off: return "Off";
    case EngineKind::Blend: return "Blend";
    case EngineKind::Fast: return "Low latency";
    case EngineKind::Quality: return "Smoothest";
    case EngineKind::DlssG: return "NVIDIA DLSS FG";
    case EngineKind::Vfg: return "NVIDIA VFG (AI)";
    case EngineKind::Fsr: return "AMD FSR (2×)";
    }
    return "";
}
std::optional<EngineKind> engineFromId(const std::string& s) {
    if (s == "off") return EngineKind::Off;
    if (s == "blend") return EngineKind::Blend;
    if (s == "fast" || s == "lowlatency" || s == "vt") return EngineKind::Fast;
    if (s == "quality" || s == "smooth" || s == "vtfrc") return EngineKind::Quality;
    if (s == "dlss" || s == "dlssg" || s == "dlss-g") return EngineKind::DlssG;
    if (s == "vfg" || s == "nvidia-vfg") return EngineKind::Vfg;
    if (s == "fsr" || s == "amd-fsr" || s == "fsr3") return EngineKind::Fsr;
    return std::nullopt;
}

// ---- Stats

void Stats::recordSource(double t) { std::lock_guard<std::mutex> l(m_); push(source_, t, 300); }
void Stats::recordPresent(double t) {
    std::lock_guard<std::mutex> l(m_);
    if (!present_.empty()) maxGap_ = std::max(maxGap_, t - present_.back());
    push(present_, t, 1200);
}
double Stats::takeMaxGapMs() { std::lock_guard<std::mutex> l(m_); double g = maxGap_ * 1000; maxGap_ = 0; return g; }
void Stats::recordInput(double t) { std::lock_guard<std::mutex> l(m_); push(input_, t, 300); }
void Stats::recordRefresh(double t) { std::lock_guard<std::mutex> l(m_); push(refresh_, t, 1200); }
void Stats::recordForced() { forced++; }
void Stats::recordRepeat() { repeats++; }
void Stats::resetInterp() { std::lock_guard<std::mutex> l(m_); interp_.clear(); }
void Stats::recordInterp(double ms) { std::lock_guard<std::mutex> l(m_); push(interp_, ms, 60); }
void Stats::recordSourceDrop() { std::lock_guard<std::mutex> l(m_); dropped_++; }

/// Events per second over the last 2 seconds (responsive after a settings change).
double Stats::rate(const std::deque<double>& a) {
    if (a.empty() || hostNow() - a.back() > 1.0) return 0;   // nothing recent: stopped
    double l = a.back();
    size_t n = 0; double first = l;
    for (auto it = a.rbegin(); it != a.rend() && *it >= l - 2.0; ++it) { n++; first = *it; }
    if (n <= 2 || l <= first) return 0;
    return double(n - 1) / (l - first);
}

Stats::Snapshot Stats::snapshot() {
    std::lock_guard<std::mutex> l(m_);
    double avg = 0, mx = 0;
    for (double v : interp_) { avg += v; mx = std::max(mx, v); }
    if (!interp_.empty()) avg /= double(interp_.size());
    return {rate(input_), rate(source_), rate(present_), rate(refresh_), avg, mx, pipelineDelayMs.load(), dropped_};
}

// ---- Options

Options Options::parse(int argc, wchar_t** argv) {
    Options o;
    std::vector<std::string> a;
    for (int i = 1; i < argc; i++) a.push_back(narrow(argv[i]));
    size_t i = 0;
    auto next = [&]() -> std::string { return ++i < a.size() ? a[i] : std::string(); };
    auto num = [&]() -> double { std::string s = next(); return s.empty() ? 0 : atof(s.c_str()); };
    for (; i < a.size(); i++) {
        const std::string& k = a[i];
        if (k == "--source") { o.source = next(); o.sourceGiven = true; }
        else if (k == "--device") { o.device = next(); if (o.source == "auto") o.source = "capture"; o.sourceGiven = true; }
        else if (k == "--width") o.width = int(num());
        else if (k == "--height") o.height = int(num());
        else if (k == "--fps") o.fps = num();
        else if (k == "--repeat") o.repeat = std::max(1, int(num()));
        else if (k == "--interp") o.engine = engineFromId(next());
        else if (k == "--mult") o.mult = std::clamp(int(num()), 1, 8);
        else if (k == "--no-audio") o.audio = false;
        else if (k == "--list-devices") o.listDevices = true;
        else if (k == "--bench") o.bench = true;
        else if (k == "--check-colors") o.checkColors = true;
        else if (k == "--check-hud") o.checkHud = true;
        else if (k == "--check-quality") o.checkQuality = true;
        else if (k == "--check-dlss") o.checkDlss = true;
        else if (k == "--activate") o.activate = true;
        else if (k == "--experimental-dlss") o.experimentalDlss = true;
        else if (k == "--dump") o.dumpDir = next();
        else if (k == "--check-real") o.realDir = next();
        else if (k == "--dump-live") o.liveDumpDir = next();
        else if (k == "--snap") o.snapPath = next();
        else if (k == "--ui-shot") o.uiShot = next();
        else if (k == "--log-file") { o.logFile = next(); o.log = true; }
        else if (k == "--no-dedupe") o.dedupe = false;
        else if (k == "--no-edge-fix") o.edgeFix = false;
        else if (k == "--no-hud-lock") o.hudLock = false;
        else if (k == "--scene") o.scene = next();
        else if (k == "--noise") o.noise = true;
        else if (k == "--log") o.log = true;
        else if (k == "--test-tone") o.testTone = true;
        else if (k == "--volume") o.volume = float(num());
        else if (k == "--game-fps") o.gameFPS = num();
        else if (k == "--signal-hz") o.signalHz = num();
        else if (k == "--cycle-modes") o.cycleModes = true;
        else if (k == "--simulate-slow") o.simulateSlowMs = num();
        else if (k == "--max-hz") o.maxHz = num();
        else if (k == "--quit-after") o.quitAfter = num();
        else if (k == "--after-gpu-reset") o.afterGpuReset = true;
        else if (k == "--stress") { o.stress = num(); if (o.stress <= 0) o.stress = 60; }
        else if (k == "--windowed") o.windowed = true;
        else if (k == "--fullscreen") o.fullscreen = true;
    }
    if (o.quitAfter > 0 || !o.uiShot.empty() || !o.liveDumpDir.empty()) o.log = true;
    return o;
}

bool signedBy(const std::wstring& path, const wchar_t* company) {
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
    bool ok = false;
    if (r == ERROR_SUCCESS) {
        if (auto* prov = WTHelperProvDataFromStateData(wd.hWVTStateData))
            if (auto* signer = WTHelperGetProvSignerFromChain(prov, 0, FALSE, 0); signer && signer->csCertChain && signer->pasCertChain) {
                wchar_t name[256] = {};
                CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
                ok = wcscmp(name, company) == 0;
            }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    return ok;
}
