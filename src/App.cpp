#include "App.h"
#include "Hud.h"
#include "Interpolators.h"
#include "Prefs.h"
#include "VfgEngine.h"
#include "Settings.h"
#include "DlssPresenter.h"
#include "Synthetic.h"
#include <dbt.h>
#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

App* g_app = nullptr;
extern bool g_launchedFromConsole;

static const GUID kCategoryVideoCamera = {0xE5323777, 0xF976, 0x4f5b, {0x9B, 0x55, 0xB9, 0x46, 0x99, 0xC4, 0x6E, 0x44}};
static const GUID kCategoryCapture = {0x65E8773D, 0x8F56, 0x11D0, {0xA3, 0xB9, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};
enum { kTimerTick = 1, kTimerLog = 2, kTimerDevices = 3, kTimerCycle = 4 };

App::App() {
    g_app = this;
    auto& p = Prefs::get();
    engine = EngineSupport::best(p.engine()).value_or(EngineKind::Fast);
    // First run on a PC with NVIDIA VFG: it gives the cleanest picture, so start with it.
    if (p.str("engine", "").empty() && EngineSupport::isSupported(EngineKind::Vfg)) engine = EngineKind::Vfg;
    multiplier = EngineSupport::any() ? p.multiplier() : 1;
    dashboardVisible = p.dashboardVisible();
    miniCounterOn = p.miniCounter();
    g_hudLock = g_opt.hudLock && p.hudStabilizer();
    g_repairEdges = g_opt.edgeFix;
    g_simulatedSlowMs = g_opt.simulateSlowMs;
    // Command-line flags override saved preferences.
    if (g_opt.engine) { if (*g_opt.engine == EngineKind::Off) multiplier = 1; else engine = *g_opt.engine; }
    if (g_opt.mult) multiplier = *g_opt.mult;
    if (!EngineSupport::isSupported(engine)) engine = EngineKind::Fast;   // saved NVIDIA engine this PC can't run
    audio.volume = g_opt.volume.value_or(p.volume());
    audio.muted = g_opt.volume ? false : p.muted();
    audio.userOffset = p.audioOffsetMs() / 1000.0;
    audio.setOutput(widen(p.str("audioOutput")));
    audioCap = std::make_unique<AudioCapture>(audio);
}

App::~App() {
    double t0 = hostNow();
    auto step = [&](const char* what) { if (g_opt.log) logf("[app] %s stopped after %.0f ms", what, (hostNow() - t0) * 1000); };
    if (source) source->stop();
    if (audioCap) audioCap->stop();
    step("audio capture");
    tone_.reset();
    audio.stop();
    step("audio output");
    if (renderer) renderer->stop();
    step("presenter");
    pipeline.reset();
    step("pipeline");
    g_app = nullptr;
}

int App::effectiveMultiplier() const {
    if (autoOff) return 1;
    return std::min({multiplier, autoCap.value_or(8), displayCap.value_or(8)});
}

double App::displayHz() {
    double hz = renderer ? renderer->displayHz() : 60;
    if (g_opt.maxHz > 0) hz = std::min(hz, g_opt.maxHz);
    return hz;
}

// ------------------------------------------------------------------ Window

int App::run(HINSTANCE inst, int show) {
    startTime_ = hostNow();
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"FrameGenMain";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(1280, dpi, 96), MulDiv(720, dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = std::min<int>(r.right - r.left, work.right - work.left), h = std::min<int>(r.bottom - r.top, work.bottom - work.top);
    int x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 2;
    // Remember the window position.
    auto& p = Prefs::get();
    if (p.integer("winW") > 200 && p.integer("winH") > 150) { x = p.integer("winX"); y = p.integer("winY"); w = p.integer("winW"); h = p.integer("winH"); }
    hwnd = CreateWindowExW(0, wc.lpszClassName, L"FrameGen", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, this);
    if (!hwnd) return 1;
    // Keep the window on a visible monitor.
    RECT wr; GetWindowRect(hwnd, &wr);
    if (!MonitorFromRect(&wr, MONITOR_DEFAULTTONULL)) SetWindowPos(hwnd, nullptr, work.left + 40, work.top + 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    dashboard.setScale(GetDpiForWindow(hwnd) / 96.f);

    bool startDlss = effectiveMultiplier() > 1 && effectiveEngine() == EngineKind::DlssG;
    pipeline = std::make_unique<FramePipeline>(effectiveMultiplier() > 1 && !startDlss ? effectiveEngine() : EngineKind::Off, std::max(effectiveMultiplier(), 2), stats);
    pipeline->wantMotion = startDlss;
    pipeline->dedupe.enabled = g_opt.dedupe;
    pipeline->liveDumpDir = g_opt.liveDumpDir;
    if (!ensurePresenter()) { MessageBoxW(hwnd, L"Could not create the display surface.", L"FrameGen", MB_ICONERROR); return 1; }

    // Don't take keyboard focus when launched from a terminal: stray keystrokes would change
    // settings (Lesson 15). Double-clicking the app activates it normally.
    ShowWindow(hwnd, g_launchedFromConsole && !g_opt.activate ? SW_SHOWNOACTIVATE : show);
    if (g_opt.activate) {   // test aid: take focus (Windows only allows it from the foreground thread's input queue)
        HWND fg = GetForegroundWindow();
        DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
        if (fgThread && fgThread != me) AttachThreadInput(fgThread, me, TRUE);
        SetForegroundWindow(hwnd); BringWindowToTop(hwnd); SetFocus(hwnd);
        if (fgThread && fgThread != me) AttachThreadInput(fgThread, me, FALSE);
    }
    if ((p.startFullScreen() && g_opt.quitAfter <= 0 && !g_opt.windowed) || g_opt.fullscreen) toggleFullscreen();
    // Dashboard controls (mouse) do the same as the keys and the Settings window.
    dashboard.onMultiplier = [this](int m) { setMultiplier(m); };
    dashboard.onEngine = [this](EngineKind k) { setEngine(k); if (multiplier <= 1) setMultiplier(2); };
    dashboard.onSource = [this](std::string name) { selectDevice(name); };
    dashboard.onResolution = [this](int w, int h) { setCaptureResolution(w, h); };
    dashboard.onCompare = [this](bool on) { renderer->compare = on; syncControls(); };
    dashboard.onMiniCounter = [this](bool on) { setMiniCounter(on); };
    dashboard.onHudLock = [this](bool on) { setHudLock(on); };
    dashboard.onMute = [this](bool on) { setMuted(on); };
    dashboard.onVolume = [this](float v) { setVolume(v); };
    dashboard.onFullscreen = [this] { toggleFullscreen(); };
    dashboard.onHide = [this] { toggleDashboard(); };
    dashboard.onSettings = [this] { showSettings(); };
    syncControls();
    placeOverlays();

    // Re-scan devices when a capture card is plugged in or removed.
    DEV_BROADCAST_DEVICEINTERFACE_W f{}; f.dbcc_size = sizeof f; f.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    f.dbcc_classguid = kCategoryVideoCamera;
    devNotify_ = RegisterDeviceNotificationW(hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);
    f.dbcc_classguid = kCategoryCapture;
    RegisterDeviceNotificationW(hwnd, &f, DEVICE_NOTIFY_WINDOW_HANDLE);

    // Playing with a controller doesn't count as user activity: keep the display awake.
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    audio.start();
    startInitialSource();
    SetTimer(hwnd, kTimerTick, 250, nullptr);
    if (g_opt.log) SetTimer(hwnd, kTimerLog, 1000, nullptr);
    if (g_opt.cycleModes) SetTimer(hwnd, kTimerCycle, 3000, nullptr);
    if (!g_opt.uiShot.empty() && !dashboardVisible) { dashboardVisible = true; }   // the shot shows the dashboard; saved setting untouched
    if (const char* s = getenv("FG_SETTINGS_SHOT")) {   // test aid: snapshot each Settings tab as PNG
        showSettings();
        settings_->snapshotTabs(s);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (settings_ && settings_->hwnd() && IsDialogMessageW(settings_->hwnd(), &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    SetThreadExecutionState(ES_CONTINUOUS);
    return 0;
}

LRESULT CALLBACK App::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App* self;
    if (msg == WM_NCCREATE) {
        self = (App*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd = h;
    } else {
        self = (App*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT App::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (renderer && wp != SIZE_MINIMIZED) renderer->resize(LOWORD(lp), HIWORD(lp));
        placeOverlays();
        return 0;
    case WM_DPICHANGED: {
        dashboard.setScale(HIWORD(wp) / 96.f);
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        refreshDashboard();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == kTimerTick) tick();
        else if (wp == kTimerLog) logStats();
        else if (wp == kTimerDevices) { KillTimer(hwnd, kTimerDevices); devicesChanged(); }
        else if (wp == kTimerCycle) {   // test aid: change modes every 3 s to check for freezes
            static const std::pair<EngineKind, int> seq[] = {{EngineKind::Quality, 2}, {EngineKind::Fast, 3}, {EngineKind::Quality, 4},
                                                             {EngineKind::Fast, 1}, {EngineKind::Fast, 2}, {EngineKind::Quality, 3}};
            auto [k, m] = seq[cycleIndex_++ % 6];
            setEngine(k); setMultiplier(m);
            logf("[cycle] -> %s x%d", engineId(k), m);
        }
        return 0;
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE || wp == DBT_DEVNODES_CHANGED) SetTimer(hwnd, kTimerDevices, 800, nullptr);
        return TRUE;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp == VK_RETURN) { toggleFullscreen(); return 0; }   // Alt+Enter
        if (handleKey(wp)) { refreshDashboard(); return 0; }
        break;
    case WM_SYSCHAR: if (wp == VK_RETURN) return 0; break;   // no beep on Alt+Enter
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (getenv("FG_CLICKLOG")) logf("[click] %d,%d dash %ld,%ld %dx%d visible %d", x, y, dashOrigin_.x, dashOrigin_.y, dashboard.width(), dashboard.height(), int(dashboardVisible));
        POINT sp{x, y}; ClientToScreen(hwnd, &sp);
        if (dashboardVisible && x >= dashOrigin_.x && y >= dashOrigin_.y && x < dashOrigin_.x + dashboard.width() && y < dashOrigin_.y + dashboard.height()) {
            if (dashboard.mouseDown(x - dashOrigin_.x, y - dashOrigin_.y, hwnd, sp)) {
                if (dashboard.dragging()) { SetCapture(hwnd); dashCaptured_ = true; }
                refreshDashboard();
                return 0;
            }
        }
        // Clicking the small FPS counter brings the dashboard back. A hidden dashboard catches nothing.
        if (!dashboardVisible && miniCounterOn && x >= miniOrigin_.x && y >= miniOrigin_.y &&
            x < miniOrigin_.x + dashboard.miniWidth() && y < miniOrigin_.y + dashboard.miniHeight()) { toggleDashboard(); return 0; }
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT sp{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (sp.x == -1 && sp.y == -1) { RECT r; GetClientRect(hwnd, &r); sp = {r.right / 2, r.bottom / 2}; ClientToScreen(hwnd, &sp); }   // menu key
        showContextMenu(sp);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        bool onDash = dashboardVisible && x >= dashOrigin_.x && y >= dashOrigin_.y && x < dashOrigin_.x + dashboard.width() && y < dashOrigin_.y + dashboard.height();
        if (!onDash) toggleFullscreen();
        return 0;
    }
    case WM_MOUSEMOVE:
        lastMouseMove_ = hostNow();
        if (cursorHidden_) { ShowCursor(TRUE); cursorHidden_ = false; }
        if (dashCaptured_) { dashboard.mouseMove(GET_X_LPARAM(lp) - dashOrigin_.x, GET_Y_LPARAM(lp) - dashOrigin_.y); refreshDashboard(); }
        return 0;
    case WM_LBUTTONUP:
        if (dashCaptured_) { dashboard.mouseUp(); ReleaseCapture(); dashCaptured_ = false; refreshDashboard(); }
        return 0;
    case WM_MOVE: case WM_EXITSIZEMOVE:
        if (!fullscreen_ && !IsIconic(hwnd) && !IsZoomed(hwnd)) {
            RECT r; GetWindowRect(hwnd, &r);
            auto& p = Prefs::get();
            if (msg == WM_EXITSIZEMOVE) { p.set("winX", int(r.left)); p.set("winY", int(r.top)); p.set("winW", int(r.right - r.left)); p.set("winH", int(r.bottom - r.top)); }
        }
        return 0;
    case WM_CLOSE: {
        double t0 = hostNow();
        ShowWindow(hwnd, SW_HIDE);   // never show a frozen picture while shutting down
        if (g_opt.log) logf("[app] closing");
        if (source) source->stop();
        if (g_opt.log) logf("[app] capture stopped after %.0f ms", (hostNow() - t0) * 1000);
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        if (devNotify_) UnregisterDeviceNotification(devNotify_);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool App::handleKey(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
    if (ctrl) {
        switch (vk) {
        case VK_OEM_4: nudgeSync(-10); return true;    // Ctrl+[  sound earlier
        case VK_OEM_6: nudgeSync(10); return true;     // Ctrl+]  sound later
        case VK_OEM_COMMA: showSettings(); return true;
        case 'M': setMuted(!audio.muted); return true;
        }
        return false;
    }
    switch (vk) {
    case '0': setMultiplier(1); return true;
    case '2': setEngine(EngineKind::Fast); if (multiplier <= 1) setMultiplier(2); return true;
    case '3': setEngine(EngineKind::Quality); if (multiplier <= 1) setMultiplier(2); return true;
    case 'D': setEngine(EngineKind::DlssG); if (multiplier <= 1) setMultiplier(2); return true;
    case 'V': setEngine(EngineKind::Vfg); if (multiplier <= 1) setMultiplier(2); return true;
    case 'M': { int next[] = {2, 2, 3, 4, 2}; setMultiplier(next[std::clamp(multiplier, 1, 4)]); return true; }
    case 'C': renderer->compare = !renderer->compare; syncControls(); return true;
    case 'U': setHudLock(!g_hudLock); return true;
    case 'F': case VK_F11: toggleFullscreen(); return true;
    case 'H': case VK_TAB: toggleDashboard(); return true;
    case VK_ESCAPE: if (fullscreen_) toggleFullscreen(); return true;
    }
    return false;
}

void App::toggleFullscreen() {
    if (!fullscreen_) {
        windowedStyle_ = GetWindowLongW(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &windowedRect_);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd, GWL_STYLE, (windowedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen_ = true;
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, windowedStyle_);
        SetWindowPos(hwnd, nullptr, windowedRect_.left, windowedRect_.top, windowedRect_.right - windowedRect_.left, windowedRect_.bottom - windowedRect_.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
        fullscreen_ = false;
        if (cursorHidden_) { ShowCursor(TRUE); cursorHidden_ = false; }
    }
}

void App::placeOverlays() {
    if (!hwnd || !renderer) return;
    RECT rc; GetClientRect(hwnd, &rc);
    float s = GetDpiForWindow(hwnd) / 96.f;
    std::string c = Prefs::get().dashboardCorner();
    bool top = c.rfind("top", 0) == 0, left = c.size() >= 4 && c.substr(c.size() - 4) == "Left";
    int m = int(16 * s), mm = int(14 * s);
    dashOrigin_.x = left ? m : rc.right - m - dashboard.width();
    dashOrigin_.y = top ? m : rc.bottom - m - dashboard.height();
    miniOrigin_.x = left ? mm : rc.right - mm - dashboard.miniWidth();
    miniOrigin_.y = top ? mm : rc.bottom - mm - dashboard.miniHeight();
}

void App::toggleDashboard() {
    dashboardVisible = !dashboardVisible;
    Prefs::get().setBool("dashboardVisible", dashboardVisible);
    refreshDashboard();
}

void App::setDashboardCorner(const std::string& corner) { Prefs::get().set("dashboardCorner", corner); placeOverlays(); refreshDashboard(); }
void App::setMiniCounter(bool on) { miniCounterOn = on; Prefs::get().setBool("miniCounter", on); syncControls(); refreshDashboard(); }

void App::showSettings() {
    if (!settings_) settings_ = std::make_unique<SettingsWindow>(*this);
    settings_->show(hwnd);
}

// ------------------------------------------------------------------ Sources

std::vector<std::string> App::deviceNames(bool refresh) {
    if (devicesCached_ && !refresh) return cachedDevices_;
    cachedDevices_.clear();
    for (auto& d : CaptureSource::devices(refresh)) cachedDevices_.push_back(d.name);
    devicesCached_ = true;
    return cachedDevices_;
}

void App::startInitialSource() {
    auto& p = Prefs::get();
    if (g_opt.sourceGiven && g_opt.source != "auto") {
        if (g_opt.source == "capture") {
            auto names = deviceNames();
            currentDevice = g_opt.device.empty() ? (names.empty() ? "" : names[0]) : g_opt.device;
            startSource("capture", g_opt.device, g_opt.width, g_opt.height, g_opt.fps);
        } else {
            currentDevice.clear();
            startSource("synthetic", "", g_opt.width, g_opt.height, g_opt.fps);
        }
    } else {
        // Double-clicked app: the remembered card, else the first card, else the test pattern.
        auto names = deviceNames();
        std::string pick;
        std::string remembered = p.deviceName();
        if (!remembered.empty() && std::find(names.begin(), names.end(), remembered) != names.end()) pick = remembered;
        else if (!names.empty()) pick = names[0];
        selectDevice(pick);
    }
    dashboard.setSources(deviceNames(), currentDevice);
    dashboard.setResolutions(supportedResolutions(), {p.captureWidth(), p.captureHeight()});
}

void App::selectDevice(const std::string& name) {
    auto& p = Prefs::get();
    currentDevice = name;
    if (!name.empty()) {
        p.set("deviceName", name);
        startSource("capture", name, p.captureWidth(), p.captureHeight(), p.captureRate());
    } else {
        int w = p.captureWidth() > 0 ? p.captureWidth() : 1920, h = p.captureHeight() > 0 ? p.captureHeight() : 1080;
        startSource("synthetic", "", w, h, 0);
    }
    dashboard.setSources(deviceNames(), currentDevice);
    dashboard.setResolutions(supportedResolutions(), {p.captureWidth(), p.captureHeight()});
    SetWindowTextW(hwnd, widen(name.empty() ? "FrameGen — Test pattern" : "FrameGen — " + name).c_str());
    if (settings_) settings_->refresh();
}

void App::startSource(const std::string& kind, const std::string& device, int w, int h, double fps) {
    if (source) source->stop();
    source.reset();
    audioCap->stop();
    tone_.reset();
    pipeline->reset();
    stats.resetInterp();
    lastAdjust = hostNow();   // give the new source a few seconds before judging performance
    stats.clearError();
    permissionDenied_ = false;
    frameCount_ = 0;
    std::unique_ptr<FrameSource> src;
    if (kind == "synthetic") {
        double f = fps > 0 ? fps : 30;
        auto syn = std::make_unique<SyntheticSource>(f, w > 0 ? w : 1920, h > 0 ? h : 1080, g_opt.repeat);
        syn->scene = g_opt.scene;
        syn->noise = g_opt.noise;
        syn->gameFPS = g_opt.gameFPS;
        syn->signalHz = g_opt.signalHz;
        src = std::move(syn);
    } else {
        auto cap = std::make_unique<CaptureSource>(device, w, h, fps);
        cap->stats = &stats;
        src = std::move(cap);
    }
    launch(std::move(src));
}

void App::launch(std::unique_ptr<FrameSource> src) {
    src->onFrame = [this](FramePtr f) {
        int n = ++frameCount_;
        // Optionally save one raw captured frame (check picture, colours, HDCP black screen).
        if (n == 90 && !g_opt.snapPath.empty()) {
            if (Gpu::get().savePNG(*f->image, g_opt.snapPath, pipeline->yuv)) logf("[snap] wrote %s", g_opt.snapPath.c_str());
        }
        pipeline->push(f);
    };
    src->admit = [this](VideoFrame& f) { return pipeline->admit(f); };
    std::string err;
    pipeline->yuv = src->yuv;
    bool ok = src->start(err);
    if (ok) pipeline->yuv = src->yuv;   // the capture card's colour matrix is known after start
    if (!ok) {
        stats.setError(err);
        if (auto* cap = dynamic_cast<CaptureSource*>(src.get())) permissionDenied_ = cap->permissionDenied;
        logf("[source] %s", err.c_str());
    }
    stats.setText(stats.sourceName, src->name());
    if (ok) {
        if (auto* cap = dynamic_cast<CaptureSource*>(src.get())) {
            if (g_opt.audio) {
                bool a = audioCap->start(cap->deviceName);
                if (g_opt.log) logf("[audio] input: %s", a ? audioCap->deviceName.c_str() : "none");
            }
        } else if (auto* syn = dynamic_cast<SyntheticSource*>(src.get()); syn && g_opt.testTone) {
            tone_ = std::make_unique<TestTone>(audio, syn->startTime);
        }
    }
    source = std::move(src);
}

void App::devicesChanged() {
    auto names = deviceNames(true);
    bool present = std::find(names.begin(), names.end(), currentDevice) != names.end();
    auto* cap = dynamic_cast<CaptureSource*>(source.get());
    if (currentDevice.empty() && !names.empty()) {
        selectDevice(names[0]);              // card plugged in while on the test pattern: switch to it
    } else if (!currentDevice.empty() && present && (!source || !stats.error().empty())) {
        selectDevice(currentDevice);         // our card came back
    } else if (!currentDevice.empty() && !present) {
        if (source) source->stop();
        source.reset();
        audioCap->stop();
        stats.setError("Capture card disconnected. Plug it back in to continue.");
    }
    (void)cap;
    dashboard.setSources(names, currentDevice);
    if (settings_) settings_->refresh();
}

std::vector<std::pair<int, int>> App::supportedResolutions() {
    if (currentDevice.empty()) return {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}};   // test pattern
    std::vector<std::pair<int, int>> out;
    for (auto& d : CaptureSource::devices()) {
        if (d.name != currentDevice) continue;
        for (auto& f : d.formats) {
            std::pair<int, int> r{f.w, f.h};
            if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
        }
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.first * a.second > b.first * b.second; });
    return out;
}

std::string App::activeFormatDescription() {
    auto* cap = dynamic_cast<CaptureSource*>(source.get());
    if (!cap || cap->activeRate <= 0) return currentDevice.empty() ? "Showing the test pattern." : "Not capturing yet.";
    return strf("Now capturing %d × %d at %.0f Hz (%s).", cap->activeW, cap->activeH, cap->activeRate, cap->activeFourcc.c_str());
}

void App::setCaptureResolution(int w, int h) {
    auto& p = Prefs::get();
    p.set("captureWidth", w); p.set("captureHeight", h);
    clearAuto(); displayCap.reset();
    selectDevice(currentDevice);
}

void App::setCaptureRate(int hz) {
    Prefs::get().set("captureRate", hz);
    clearAuto(); displayCap.reset();
    if (!currentDevice.empty()) selectDevice(currentDevice);
}

void App::setScaling(const std::string& mode) { Prefs::get().set("scaling", mode); renderer->scaling = mode == "fill" ? 1 : mode == "stretch" ? 2 : 0; }
void App::setMaxRefresh(int hz) { Prefs::get().set("maxRefresh", hz); renderer->maxHz = hz > 0 ? double(hz) : g_opt.maxHz; displayCap.reset(); }
void App::setPacing(const std::string& mode) { Prefs::get().set("pacing", mode); renderer->vrr = mode == "vrr"; }

/// One click for slower PCs: the next step down (4K/1440p → 1080p → 720p) the card offers.
int App::currentHeight() {
    if (pipeline->lastH > 0) return pipeline->lastH;
    if (auto* cap = dynamic_cast<CaptureSource*>(source.get())) return cap->activeH;
    int h = Prefs::get().captureHeight();
    return h > 0 ? h : 1080;
}
std::optional<std::pair<int, int>> App::lowerResolution() {
    int h = currentHeight();
    auto list = supportedResolutions();
    for (int target : {1080, 720, 480}) {
        if (target >= h) continue;
        for (auto& r : list) if (r.second == target && r.first >= target * 4 / 3) return r;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ Frame generation settings

bool App::generating() { return effectiveMultiplier() > 1 && (pipeline->kind != EngineKind::Off || pipeline->wantMotion); }

/// DLSS-G needs its own Direct3D 12 presenter; everything else uses the Direct3D 11 one. Switching
/// releases the old swap chain first (a window has one at a time).
bool App::ensurePresenter() {
    bool wantDlss = effectiveMultiplier() > 1 && effectiveEngine() == EngineKind::DlssG && EngineSupport::isSupported(EngineKind::DlssG);
    const char* want = wantDlss ? "dlss" : "d3d11";
    if (renderer && strcmp(renderer->kind(), want) == 0) return true;
    auto& p = Prefs::get();
    bool compare = renderer ? renderer->compare.load() : false;
    if (renderer) {
        renderer->stop();
        renderer.reset();
        auto l = Gpu::get().lock();
        Gpu::get().ctx->ClearState();
        Gpu::get().ctx->Flush();
    }
    std::string err;
#ifdef FG_HAVE_STREAMLINE
    if (wantDlss) {
        renderer = std::make_unique<DlssPresenter>(*pipeline, stats);
        if (!renderer->init(hwnd, err)) {
            logf("[dlss] %s", err.c_str());
            renderer.reset();
            autoEngine = EngineKind::Fast;
            autoNote = "DLSS Frame Generation couldn't start (" + err + "), so Low latency is used instead.";
        }
    }
#endif
    if (!renderer) {
        renderer = std::make_unique<Renderer>(*pipeline, stats);
        if (!renderer->init(hwnd, err)) { logf("[present] %s", err.c_str()); return false; }
    }
    renderer->maxHz = g_opt.maxHz > 0 ? g_opt.maxHz : (p.maxRefresh() > 0 ? double(p.maxRefresh()) : 0.0);
    renderer->scaling = p.scaling() == "fill" ? 1 : p.scaling() == "stretch" ? 2 : 0;
    renderer->vrr = p.pacing() == "vrr";
    renderer->compare = compare;
    renderer->generate = std::max(1, effectiveMultiplier() - 1);
    RECT rc; GetClientRect(hwnd, &rc);
    renderer->resize(rc.right - rc.left, rc.bottom - rc.top);
    renderer->start();
    logf("[present] using the %s presenter", renderer->kind());
    return true;
}

void App::applyGeneration() {
    int m = effectiveMultiplier();
    bool dlss = m > 1 && effectiveEngine() == EngineKind::DlssG && EngineSupport::isSupported(EngineKind::DlssG);
    pipeline->wantMotion = dlss;
    if (m <= 1 || !EngineSupport::any() || dlss || engineBlocked()) pipeline->configure(EngineKind::Off, std::max(m, 2));
    else pipeline->configure(effectiveEngine(), m);
    ensurePresenter();
    if (renderer) renderer->generate = std::max(1, m - 1);
    if (dlss && strcmp(renderer->kind(), "dlss") != 0) { pipeline->wantMotion = false; pipeline->configure(effectiveEngine(), m); }
    stats.resetInterp();
    lastDropped = stats.snapshot().dropped;   // judge the new setting only on its own drops
    syncControls();
}

void App::clearAuto() { autoCap.reset(); autoEngine.reset(); autoOff = false; autoNote.clear(); lastAdjust = hostNow(); }

void App::setMultiplier(int m) {
    multiplier = EngineSupport::any() ? m : 1;
    Prefs::get().set("multiplier", multiplier);
    clearAuto();
    applyGeneration();
}

/// The user picked an NVIDIA engine this PC can't run: why, in plain English ("" = fine). Generation
/// stays off until they choose another mode.
std::string App::engineBlockReason() const {
    if (engine == EngineKind::Vfg && !EngineSupport::isSupported(EngineKind::Vfg)) return Vfg::support().reason;
    if (engine == EngineKind::DlssG && !EngineSupport::isSupported(EngineKind::DlssG)) {
        const auto& d = DlssG::support();
        if (d.gpuUnsupported || !Gpu::get().isNvidia) return "Your GPU does not support NVIDIA DLSS Frame Generation. Frame generation is disabled, please select another option.";
        return "NVIDIA DLSS Frame Generation can't run: " + d.reason + ". Frame generation is disabled, please select another option.";
    }
    return "";
}
bool App::engineBlocked() const { return !engineBlockReason().empty(); }

void App::setEngine(EngineKind k) {
    if (k != EngineKind::Vfg && k != EngineKind::DlssG && !EngineSupport::isSupported(k)) return;
    engine = k;
    Prefs::get().set("engine", std::string(engineId(k)));
    clearAuto();
    applyGeneration();
    if (std::string why = engineBlockReason(); !why.empty()) {
        logf("[app] %s selected but unavailable: %s", engineShortLabel(k), why.c_str());
        if (hwnd) MessageBoxW(hwnd, widen(why).c_str(), (L"FrameGen — " + widen(engineShortLabel(k))).c_str(), MB_ICONWARNING | MB_OK);
    }
}

void App::setHudLock(bool on) {
    g_hudLock = on;
    Prefs::get().setBool("hudStabilizer", on);
    HudStabilizer::get().reset();
    syncControls();
}

void App::setVolume(float v) { audio.volume = v; audio.muted = false; Prefs::get().set("volume", double(v)); Prefs::get().setBool("muted", false); }
void App::setAudioOutput(const std::wstring& id) { audio.setOutput(id); Prefs::get().set("audioOutput", narrow(id)); }
void App::setMuted(bool m) { audio.muted = m; Prefs::get().setBool("muted", m); syncControls(); }
void App::nudgeSync(int ms) {
    auto& p = Prefs::get();
    p.set("audioOffsetMs", std::clamp(p.audioOffsetMs() + ms, -200, 300));
    audio.userOffset = p.audioOffsetMs() / 1000.0;
    if (settings_) settings_->refresh();
    refreshDashboard();
}

std::vector<Dashboard::EngineChoice> App::engineChoices() {
    std::vector<Dashboard::EngineChoice> v;
    for (EngineKind k : {EngineKind::Fast, EngineKind::Quality, EngineKind::Vfg, EngineKind::DlssG}) {
        std::string why;
        if (k == EngineKind::Vfg) why = Vfg::support().reason;
        // The NVIDIA engines are always offered: picking one on a PC that can't run it says why.
        v.push_back({k, engineShortLabel(k), k == EngineKind::Vfg || k == EngineKind::DlssG || EngineSupport::isSupported(k), why});
    }
    return v;
}

/// Right-click menu: every quick control, for mouse users (also with the dashboard hidden).
void App::showContextMenu(POINT screen) {
    enum { kOff = 1, kM2, kM3, kM4, kHud = 10, kCmp, kFull, kDash, kSet, kMuteId, kEng = 100 };
    HMENU m = CreatePopupMenu(), gen = CreatePopupMenu(), mode = CreatePopupMenu();
    int em = effectiveMultiplier();
    AppendMenuW(gen, MF_STRING | (multiplier <= 1 ? MF_CHECKED : 0), kOff, L"Off");
    for (int i = 2; i <= 4; i++) AppendMenuW(gen, MF_STRING | (multiplier == i ? MF_CHECKED : 0) | (EngineSupport::any() ? 0 : MF_GRAYED), kOff + i - 1, widen(strf("%d×", i)).c_str());
    auto choices = engineChoices();
    Dashboard::appendEngineItems(mode, choices, engine, kEng);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)gen, widen(multiplier > 1 ? strf("Frame generation: %d×", multiplier) : std::string("Frame generation: Off")).c_str());
    AppendMenuW(m, MF_POPUP | (multiplier > 1 ? 0 : MF_GRAYED), (UINT_PTR)mode, widen(std::string("Mode: ") + engineShortLabel(engine)).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_hudLock ? MF_CHECKED : 0) | (em > 1 ? 0 : MF_GRAYED), kHud, L"Stabilize HUD && text	U");
    AppendMenuW(m, MF_STRING | (renderer && renderer->compare ? MF_CHECKED : 0), kCmp, L"Compare original / generated	C");
    AppendMenuW(m, MF_STRING | (audio.muted ? MF_CHECKED : 0), kMuteId, L"Mute	Ctrl+M");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (dashboardVisible ? MF_CHECKED : 0), kDash, L"Show dashboard	H");
    AppendMenuW(m, MF_STRING | (fullscreen_ ? MF_CHECKED : 0), kFull, L"Full screen	F");
    AppendMenuW(m, MF_STRING, kSet, L"Settings…	Ctrl+,");
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, 0, hwnd, nullptr);
    DestroyMenu(m);   // also destroys the submenus
    if (cmd >= kOff && cmd <= kM4) setMultiplier(cmd - kOff + 1);
    else if (cmd >= kEng && size_t(cmd - kEng) < choices.size()) { if (choices[cmd - kEng].available) setEngine(choices[cmd - kEng].kind); }
    else switch (cmd) {
        case kHud: setHudLock(!g_hudLock); break;
        case kCmp: renderer->compare = !renderer->compare; syncControls(); break;
        case kMuteId: setMuted(!audio.muted); break;
        case kDash: toggleDashboard(); break;
        case kFull: toggleFullscreen(); break;
        case kSet: showSettings(); break;
    }
    refreshDashboard();
}

void App::syncControls() {
    dashboard.setControls(multiplier, engine, renderer && renderer->compare, miniCounterOn, audio.volume, audio.muted, g_hudLock, EngineSupport::any(),
                          engineChoices());
    if (settings_) settings_->refresh();
}

/// Slower PCs: if generating takes too long to keep up with the game, step down 4× → 3× → 2×,
/// then try the other engine, then pause generation (Lesson 13).
void App::autoAdjust(const Stats::Snapshot& s) {
    double now = hostNow();
    // Drops during the grace period (start-up, a settings change) never count.
    if (!Prefs::get().autoAdjust() || effectiveMultiplier() <= 1 || pipeline->kind == EngineKind::Off || s.sourceFPS <= 10 || now - lastAdjust <= 4 || s.interpAvgMs <= 0) {
        lastDropped = s.dropped; slowTicks_ = 0; return;
    }
    double budget = 1000 / s.sourceFPS;
    // Too slow = generation takes most of the game's frame time. Dropped frames alone can come from
    // the console or card (a mode switch, a hiccup), so they only count while generation is also
    // heavy (> 50 % of the budget). It must persist for 1 s (4 ticks) before stepping down.
    bool heavyDrops = s.dropped > lastDropped + 15 && s.interpAvgMs > budget * 0.5;
    lastDropped = s.dropped;
    if (s.interpAvgMs > budget * 0.8 || heavyDrops) slowTicks_++; else slowTicks_ = 0;
    if (slowTicks_ < 4) return;
    slowTicks_ = 0;
    int m = effectiveMultiplier();
    if (m > 2) {
        autoCap = m - 1;
        autoNote = strf("Reduced to %d× — this PC can't generate %d× in time.", m - 1, m);
    } else if (!autoEngine && (effectiveEngine() == EngineKind::Quality || effectiveEngine() == EngineKind::Vfg)) {
        autoEngine = EngineKind::Fast;
        autoNote = "Switched to Low latency mode — it runs better on this PC.";
    } else {
        autoOff = true;
        autoNote = lowerResolution() ? "Frame generation paused — this PC can't keep up at this resolution. Use the button below to switch to a lower capture resolution (also in Settings › Video)."
                                     : "Frame generation paused — this PC can't keep up with this input. Close other apps, or try a lower resolution.";
    }
    lastAdjust = now;
    applyGeneration();
}

/// Engine limits by resolution (none for the built-in engines; kept for NVIDIA/AMD engines).
void App::enforceEngineLimits() {}

/// Caps the multiplier so output never exceeds the display refresh rate. Changes only after the
/// game's frame rate has been stable for 2 s, to avoid flip-flopping.
void App::updateDisplayCap(const Stats::Snapshot& s) {
    if (!Prefs::get().displayCapEnabled()) { if (displayCap) { displayCap.reset(); applyGeneration(); } return; }
    if (s.sourceFPS <= 5) return;
    int cap = std::max(1, int(displayHz() / s.sourceFPS + 0.12));
    std::optional<int> newCap = cap >= 8 ? std::nullopt : std::optional<int>(cap);
    if (newCap == displayCap) { displayCapCandidate.reset(); return; }
    double now = hostNow();
    int v = newCap.value_or(0);
    if (displayCapCandidate && displayCapCandidate->first == v) {
        if (now - displayCapCandidate->second > 2) { displayCap = newCap; displayCapCandidate.reset(); applyGeneration(); }
    } else {
        displayCapCandidate = std::make_pair(v, now);
    }
}

/// Even pacing needs the refresh rate to be a whole multiple of the output frame rate (60 fps on
/// 280 Hz alternates 4 and 5 refreshes per frame). Suggest a better mode (Lesson 12).
std::string App::pacingTip(double outputTarget) {
    if (outputTarget <= 20 || g_opt.maxHz > 0 || (renderer && renderer->vrr)) return "";
    double hz = displayHz();
    auto even = [&](double r) { double k = r / outputTarget; return k >= 1 && std::abs(k - std::round(k)) < 0.02; };
    if (even(hz)) return "";
    int better = 0;
    for (int r : renderer->displayRates()) if (even(r) && r >= outputTarget) better = std::max(better, r);
    if (!better) return "";
    return strf("Tip: for perfectly even motion set this display to %d Hz (Windows Settings › Display › Advanced display) — %.0f fps doesn't divide evenly into %.0f Hz. Or use Variable refresh (Settings › Display).",
                better, outputTarget, hz);
}

std::string App::displayCapNote() {
    if (!displayCap || *displayCap >= multiplier || autoOff) return "";
    int hz = int(std::lround(displayHz()));
    return *displayCap <= 1 ? strf("The game already runs at this display's %d Hz, so there are no frames to add.", hz)
                            : strf("Limited to %d× — this display shows up to %d fps.", *displayCap, hz);
}

void App::openPrivacySettings(const wchar_t* page) { ShellExecuteW(nullptr, L"open", page, nullptr, nullptr, SW_SHOWNORMAL); }

// ------------------------------------------------------------------ Tick & dashboard

void App::tick() {
    audio.targetDelay = renderer->delay.load();
    auto s = stats.snapshot();
    updateDisplayCap(s);
    enforceEngineLimits();
    autoAdjust(s);
    refreshDashboard(&s);
    double t = hostNow() - startTime_;
    if (!g_opt.uiShot.empty() && !shotDone_) {
        const char* d = getenv("FG_SHOT_DELAY");
        if (t > (d ? atof(d) : 4.0)) { renderer->requestShot(g_opt.uiShot); shotDone_ = true; }
    }
    if (g_opt.quitAfter > 0 && t > g_opt.quitAfter) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    // Hide the cursor while playing full screen with the dashboard hidden.
    if (fullscreen_ && !dashboardVisible && !cursorHidden_ && hostNow() - lastMouseMove_ > 2 && GetForegroundWindow() == hwnd) { ShowCursor(FALSE); cursorHidden_ = true; }
}

void App::refreshDashboard(const Stats::Snapshot* snap) {
    if (!renderer) return;
    Stats::Snapshot s = snap ? *snap : stats.snapshot();
    int m = effectiveMultiplier();
    double target = generating() ? s.sourceFPS * m : s.sourceFPS;
    bool healthy = target == 0 || s.outputFPS >= target * 0.93;
    placeOverlays();
    if (!dashboardVisible) {
        renderer->setOverlay(0, {});
        if (miniCounterOn) renderer->setOverlay(1, {dashboard.renderMini(s.outputFPS, healthy), int(miniOrigin_.x), int(miniOrigin_.y), 1, true});
        else renderer->setOverlay(1, {});
        return;
    }
    renderer->setOverlay(1, {});

    bool receiving = s.inputFPS > 1;
    std::string status, err = stats.error();
    std::vector<std::string> warn;
    if (!err.empty()) warn.push_back(err);
    Dashboard::Live live;
    if (permissionDenied_) {
        status = "Camera access needed";
        warn = {"Windows is blocking camera access for desktop apps, so FrameGen can't read your capture card. Turn on \"Let desktop apps access your camera\", then choose the card again."};
        live.actionTitle = "Open Camera Privacy Settings";
        live.action = [this] { openPrivacySettings(L"ms-settings:privacy-webcam"); };
    } else if (currentDevice.empty()) {
        status = "Test pattern";
        if (deviceNames().empty() && err.empty()) warn.push_back("No capture card found. Plug one in and FrameGen will switch to it.");
    } else if (receiving) {
        status = "Live · " + currentDevice;
    } else {
        status = "Connecting · " + currentDevice;
    }
    if (receiving && !currentDevice.empty() && s.inputFPS < 26 && s.sourceFPS < 1)
        if (err.empty()) warn.push_back("No picture from the console? Check it's on, HDMI goes into the card's IN port, and HDCP is off.");
    if (!EngineSupport::any()) warn.push_back(EngineSupport::unsupportedReason());
    if (!autoNote.empty()) warn.push_back(autoNote);
    if (engineBlocked() && multiplier > 1) {
        warn.push_back(engineBlockReason());
        if (!live.action) { live.actionTitle = "Use Low latency instead"; live.action = [this] { setEngine(EngineKind::Fast); }; }
    }
    if (auto n = displayCapNote(); !n.empty()) warn.push_back(n);
    if ((receiving || currentDevice.empty()) && s.sourceFPS > 10) {
        double targetOut = std::round(s.sourceFPS * std::max(m, 1) / 5) * 5;   // e.g. 29.97×2 → 60
        if (Prefs::get().showTips()) if (auto tip = pacingTip(targetOut); !tip.empty()) warn.push_back(tip);
    }
    if (renderer->compare) warn.push_back("Compare on: left = original, right = generated.");
    if (strcmp(renderer->kind(), "dlss") == 0 && GetForegroundWindow() != hwnd)
        warn.push_back("DLSS Frame Generation pauses while this window isn't in focus — click the picture to resume.");
    if (renderer && strcmp(renderer->kind(), "dlss") == 0 && DlssG::maxGenerateKnown() > 0 && multiplier - 1 > DlssG::maxGenerateKnown())
        warn.push_back(strf("DLSS Frame Generation on this graphics card goes up to %d×, so that's what you get.", DlssG::maxGenerateKnown() + 1));

    // Sound line
    int offset = Prefs::get().audioOffsetMs();
    std::string sync = offset == 0 ? "synced to video (Ctrl+[ ] to adjust)" : strf("sync %+d ms (Ctrl+[ ])", offset);
    auto* cap = dynamic_cast<CaptureSource*>(source.get());
    if (cap || !currentDevice.empty()) {
        if (audioCap->accessDenied) {
            live.sound = "Sound off — microphone access is needed to play the card's audio.";
            if (!live.action) { live.actionTitle = "Open Microphone Privacy Settings"; live.action = [this] { openPrivacySettings(L"ms-settings:privacy-microphone"); }; }
        } else if (!audioCap->deviceName.empty()) {
            live.sound = audio.isReceiving() ? "Card audio → " + audio.outputName() + " · " + sync : "Waiting for sound from " + audioCap->deviceName + "…";
        } else {
            live.sound = receiving ? "This capture card has no audio input." : "—";
        }
    } else {
        live.sound = tone_ ? "Test beeps · " + sync : "No sound in test pattern";
    }

    std::string rateText = strf("%.0f Hz", s.inputFPS);
    if (cap && cap->activeRate > 0) rateText = strf("%.0f Hz%s", cap->activeRate, cap->isAutoRate() ? " (auto)" : "");
    std::string size = stats.get(stats.sourceSize);
    live.signal = size.empty() ? "No signal yet" : strf("Card %s @ %s · game %.0f fps · dropped %d", size.c_str(), rateText.c_str(), s.sourceFPS, s.dropped);
    if (!live.action && !autoNote.empty()) {
        if (auto lr = lowerResolution()) {
            auto [lw, lh] = *lr;
            live.actionTitle = strf("Switch to %d × %d", lw, lh);
            live.action = [this, lw, lh] { setCaptureResolution(lw, lh); setMultiplier(std::max(2, multiplier)); };
        }
    }
    live.live = receiving && err.empty() && !permissionDenied_;
    live.status = status;
    live.outputFPS = s.outputFPS; live.gameFPS = s.sourceFPS; live.targetFPS = target;
    live.delayMs = receiving ? s.delayMs : 0;
    live.genMs = generating() ? s.interpAvgMs : 0;
    live.displayHz = displayHz();
    for (size_t i = 0; i < warn.size(); i++) live.warning += (i ? "\n" : "") + warn[i];
    dashboard.update(live);
    auto img = dashboard.render();
    placeOverlays();
    renderer->setOverlay(0, {img, int(dashOrigin_.x), int(dashOrigin_.y), 1, img != nullptr});
}

void App::logStats() {
    auto s = stats.snapshot();
    auto d = pipeline->dedupe.recentDiffs();
    std::string extra = d.empty() ? "" : strf(" | diff med %.2f max %.2f", d[d.size() / 2], d.back());
    auto ts = pipeline->dedupe.tileStats();
    extra += strf(" | maxTile repeats max %.2f · uniques min %.2f | forced %d", ts.first.empty() ? -1.0 : ts.first.back(), ts.second.empty() ? -1.0 : ts.second.front(), stats.forced.load());
    auto j = pipeline->cadence.jitter();
    extra += strf(" | undrawable %d", renderer->undrawable.load());
    extra += strf(" | cadence %.2f ms, jitter raw %.2f -> %.2f ms | display cap %s", pipeline->cadence.interval() * 1000, j.first, j.second,
                  displayCap ? strf("%dx", *displayCap).c_str() : "none");
    extra += strf(" | audio buf %.0f ms target %.0f ms (out lat %.0f) underruns %d in %s", audio.bufferedSeconds() * 1000,
                  std::max(0.01, audio.targetDelay + audio.userOffset - audio.outputLatency()) * 1000, audio.outputLatency() * 1000, audio.underruns(),
                  audio.isReceiving() ? "yes" : "no");
    double gap = stats.takeMaxGapMs();
    std::string err = stats.error();
    logf("[stats] maxgap %.0f ms | signal %.1f fps, repeats %d | unique %.1f fps -> output %.1f fps (refresh %.0f Hz) | interp %.1f ms | delay %.1f ms | dropped %d | %s x%d%s%s%s",
         gap, s.inputFPS, stats.repeats.load(), s.sourceFPS, s.outputFPS, s.refreshHz, s.interpAvgMs, s.delayMs, s.dropped,
         engineId(pipeline->kind), effectiveMultiplier(), autoNote.empty() ? "" : (" | AUTO: " + autoNote).c_str(),
         err.empty() ? "" : (" | ERROR: " + err).c_str(), extra.c_str());
}
