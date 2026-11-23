#include "Settings.h"
#include "App.h"
#include "Interpolators.h"
#include "Prefs.h"
#include "VfgEngine.h"
#include "Install.h"
#include <shellapi.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <algorithm>

enum {
    kDevice = 1001, kRes, kRate, kActive,
    kMult = 1101, kEngine, kHud, kAuto, kCap, kVfgModel, kVfgStatus, kVfgFolder,
    kScaling = 1201, kRefresh, kPacing, kFullscreen, kTips,
    kMute = 1301, kVolume, kSync, kSyncLabel, kOutput,
    kCorner = 1401, kMini, kLogs,
};

static const int kRates[] = {0, 120, 60, 50, 30};
static const wchar_t* kRateNames[] = {L"Auto (up to 120 Hz)", L"120 Hz", L"60 Hz", L"50 Hz (PAL)", L"30 Hz"};
static const int kRefreshes[] = {0, 240, 144, 120, 60};
static const char* kCorners[] = {"topLeft", "topRight", "bottomLeft", "bottomRight"};
static const char* kScalings[] = {"fit", "fill", "stretch"};
static const char* kPacings[] = {"vsync", "vrr"};
static const char* kVfgModels[] = {"low", "medium", "high"};

SettingsWindow::~SettingsWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (font_) DeleteObject(font_);
}

void SettingsWindow::show(HWND owner) {
    if (!hwnd_) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"FrameGenSettings";
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        dpi_ = owner ? GetDpiForWindow(owner) : 96;
        RECT r{0, 0, px(560), px(500)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi_);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"FrameGen Settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, this);
        build();
    }
    refresh();
    // Centre over the main window.
    RECT o{}, s{};
    if (owner) GetWindowRect(owner, &o); else SystemParametersInfoW(SPI_GETWORKAREA, 0, &o, 0);
    GetWindowRect(hwnd_, &s);
    SetWindowPos(hwnd_, HWND_TOP, (o.left + o.right - (s.right - s.left)) / 2, (o.top + o.bottom - (s.bottom - s.top)) / 2, 0, 0, SWP_NOSIZE);
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
}

HWND SettingsWindow::add(int tab, const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | style, px(x), px(y), px(w), px(h), hwnd_, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font_, TRUE);
    controls_.push_back({tab, c});
    if (id) byId_[id] = c;
    return c;
}

int SettingsWindow::label(int tab, const std::wstring& text, int y) {
    add(tab, L"STATIC", text, SS_RIGHT, 24, y + 3, 132, 20);
    return y;
}

int SettingsWindow::note(int tab, const std::wstring& text, int y) {
    // Measure the wrapped height.
    HDC dc = GetDC(hwnd_);
    HGDIOBJ old = SelectObject(dc, font_);
    RECT r{0, 0, px(360), 0};
    DrawTextW(dc, text.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
    int h = MulDiv(r.bottom, 96, dpi_) + 4;
    HWND c = add(tab, L"STATIC", text, SS_LEFT, 168, y, 360, h);
    (void)c;
    return y + h + 8;
}

void SettingsWindow::build() {
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi_);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    tabs_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, px(12), px(12), px(536), px(476), hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(tabs_, WM_SETFONT, (WPARAM)font_, TRUE);
    const wchar_t* names[] = {L"Video", L"Frame Generation", L"Display", L"Sound", L"Interface"};
    for (int i = 0; i < 5; i++) { TCITEMW ti{}; ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)names[i]; TabCtrl_InsertItem(tabs_, i, &ti); }
    const DWORD combo = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, check = BS_AUTOCHECKBOX | WS_TABSTOP;
    int y;

    // Video
    y = 56;
    label(0, L"Capture device:", y); add(0, WC_COMBOBOXW, L"", combo, 168, y, 340, 300, kDevice); y += 34;
    label(0, L"Resolution:", y); add(0, WC_COMBOBOXW, L"", combo, 168, y, 340, 300, kRes); y += 32;
    y = note(0, L"Frame generation work grows with resolution. On less powerful graphics cards choose 1280 × 720 — it needs less than half the processing of 1080p, and the picture is scaled up to fill your screen.", y);
    label(0, L"Capture rate:", y); add(0, WC_COMBOBOXW, L"", combo, 168, y, 340, 300, kRate); y += 32;
    y = note(0, L"Auto captures as fast as the card allows (up to 120 Hz), so any console signal comes through. Choose a fixed rate only if your card misbehaves.", y);
    add(0, L"STATIC", L"", SS_LEFT, 168, y, 360, 20, kActive);

    // Frame Generation
    y = 56;
    label(1, L"Frame generation:", y); add(1, WC_COMBOBOXW, L"", combo, 168, y, 200, 300, kMult); y += 34;
    label(1, L"Mode:", y); add(1, WC_COMBOBOXW, L"", combo, 168, y, 240, 200, kEngine); y += 32;
    y = note(1, L"Low latency responds fastest. Smoothest searches motion more carefully for every generated frame. NVIDIA VFG (AI) gives the cleanest picture on fast camera movement. NVIDIA DLSS FG uses DLSS Frame Generation (works while this window is focused). Both need an RTX 40/50.", y);
    label(1, L"VFG quality:", y); add(1, WC_COMBOBOXW, L"", combo, 168, y, 240, 200, kVfgModel); y += 32;
    label(1, L"NVIDIA VFG:", y); add(1, L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, 168, y + 3, 250, 20, kVfgStatus);
    add(1, L"BUTTON", L"Choose folder…", BS_PUSHBUTTON | WS_TABSTOP, 424, y, 100, 24, kVfgFolder); y += 34;
    add(1, L"BUTTON", L"Stabilize HUD && text", check, 168, y, 340, 22, kHud); y += 28;
    add(1, L"BUTTON", L"Automatically lower settings if this PC can't keep up", check, 168, y, 360, 22, kAuto); y += 28;
    add(1, L"BUTTON", L"Don't generate more frames than the display can show", check, 168, y, 360, 22, kCap);

    // Display
    y = 56;
    label(2, L"Scaling:", y); add(2, WC_COMBOBOXW, L"", combo, 168, y, 200, 200, kScaling); y += 32;
    y = note(2, L"Fit shows the whole picture (black bars if the window's shape differs). Fill crops to cover the window. Stretch fills it exactly.", y);
    label(2, L"Maximum refresh:", y); add(2, WC_COMBOBOXW, L"", combo, 168, y, 200, 200, kRefresh); y += 32;
    y = note(2, L"For the most even motion, your display's refresh rate should be a whole multiple of the output frame rate — e.g. 240 Hz for 60 or 120 fps.", y);
    label(2, L"Frame pacing:", y); add(2, WC_COMBOBOXW, L"", combo, 168, y, 340, 200, kPacing); y += 32;
    y = note(2, L"Variable refresh shows every frame the moment it's ready — use it with a G-SYNC / FreeSync display in full screen. Leave it on V-Sync otherwise (it would tear).", y);
    add(2, L"BUTTON", L"Start in full screen", check, 168, y, 340, 22, kFullscreen); y += 28;
    add(2, L"BUTTON", L"Show tips (e.g. a better refresh rate for even motion)", check, 168, y, 360, 22, kTips);

    // Sound
    y = 56;
    label(3, L"Play sound on:", y); add(3, WC_COMBOBOXW, L"", combo, 168, y, 340, 300, kOutput); y += 34;
    add(3, L"BUTTON", L"Mute", check, 168, y, 200, 22, kMute); y += 32;
    label(3, L"Volume:", y); add(3, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 164, y, 240, 28, kVolume); y += 36;
    label(3, L"Sound sync:", y); add(3, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 164, y, 240, 28, kSync);
    add(3, L"STATIC", L"", SS_LEFT, 410, y + 4, 80, 20, kSyncLabel); y += 38;
    note(3, L"Move right if the sound comes before the picture, left if it lags. Bluetooth headphones add their own delay. Shortcut: Ctrl+[ and Ctrl+].", y);
    SendMessageW(byId_[kVolume], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(byId_[kSync], TBM_SETRANGEMIN, TRUE, -200);
    SendMessageW(byId_[kSync], TBM_SETRANGEMAX, TRUE, 300);

    // Interface
    y = 56;
    label(4, L"Dashboard position:", y); add(4, WC_COMBOBOXW, L"", combo, 168, y, 200, 200, kCorner); y += 34;
    add(4, L"BUTTON", L"Show a small FPS counter when the dashboard is hidden", check, 168, y, 360, 22, kMini); y += 32;
    y = note(4, L"Right-click the picture for every quick option. Press H (or click the small FPS counter) to show or hide the dashboard — its controls work with the mouse. Keys: F full screen, C compare, U HUD stabilisation, M 2×/3×/4×/6×/8×, 0 off, 2 Low latency, 3 Smoothest, V NVIDIA VFG, D NVIDIA DLSS FG.", y);
    add(4, L"STATIC", widen(std::string("FrameGen ") + FG_VERSION), SS_LEFT, 168, y + 4, 200, 20);
    add(4, L"BUTTON", L"Open logs folder", BS_PUSHBUTTON | WS_TABSTOP, 384, y, 140, 26, kLogs);

    auto fill = [&](int id, std::initializer_list<const wchar_t*> items) { for (auto* s : items) SendMessageW(byId_[id], CB_ADDSTRING, 0, (LPARAM)s); };
    fill(kMult, {L"Off", L"2× (e.g. 30 → 60 fps)", L"3× (e.g. 30 → 90 fps)", L"4× (e.g. 30 → 120 fps)", L"5× (e.g. 30 → 150 fps)",
                 L"6× (e.g. 30 → 180 fps)", L"7× (e.g. 30 → 210 fps)", L"8× (e.g. 30 → 240 fps)"});
    fill(kVfgModel, {L"Fast (lowest delay)", L"Balanced", L"Best quality (2× / 3×)"});
    fill(kScaling, {L"Fit", L"Fill", L"Stretch"});
    fill(kRefresh, {L"Display maximum", L"240 Hz", L"144 Hz", L"120 Hz", L"60 Hz"});
    fill(kPacing, {L"Every display refresh (V-Sync)", L"Variable refresh (G-SYNC / FreeSync)"});
    fill(kCorner, {L"Top left", L"Top right", L"Bottom left", L"Bottom right"});
    for (auto* s : kRateNames) SendMessageW(byId_[kRate], CB_ADDSTRING, 0, (LPARAM)s);
    selectTab(0);
}

void SettingsWindow::selectTab(int i) {
    TabCtrl_SetCurSel(tabs_, i);
    for (auto& [tab, c] : controls_) ShowWindow(c, tab == i ? SW_SHOW : SW_HIDE);
    for (auto& [tab, c] : controls_) if (tab == i) SetWindowPos(c, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

static void setCheck(HWND c, bool on) { SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
static bool isChecked(HWND c) { return SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED; }

void SettingsWindow::refresh() {
    if (!hwnd_) return;
    refreshing_ = true;
    auto& p = Prefs::get();
    // Devices
    HWND dev = byId_[kDevice];
    SendMessageW(dev, CB_RESETCONTENT, 0, 0);
    auto names = app_.deviceNames();
    int sel = -1;
    for (size_t i = 0; i < names.size(); i++) { SendMessageW(dev, CB_ADDSTRING, 0, (LPARAM)widen(names[i]).c_str()); if (names[i] == app_.currentDevice) sel = int(i); }
    SendMessageW(dev, CB_ADDSTRING, 0, (LPARAM)L"Test pattern");
    SendMessageW(dev, CB_SETCURSEL, sel >= 0 ? sel : int(names.size()), 0);
    // Resolutions the current card offers
    HWND res = byId_[kRes];
    SendMessageW(res, CB_RESETCONTENT, 0, 0);
    resList_ = {{0, 0}};
    SendMessageW(res, CB_ADDSTRING, 0, (LPARAM)L"Auto (1920 × 1080)");
    for (auto& r : app_.supportedResolutions()) {
        resList_.push_back(r);
        SendMessageW(res, CB_ADDSTRING, 0, (LPARAM)widen(strf("%d × %d", r.first, r.second) + (r.second == 720 ? "  — best for less powerful PCs" : "")).c_str());
    }
    int rs = 0;
    for (size_t i = 0; i < resList_.size(); i++) if (resList_[i] == std::make_pair(p.captureWidth(), p.captureHeight())) rs = int(i);
    SendMessageW(res, CB_SETCURSEL, rs, 0);
    int ri = 0; for (int i = 0; i < 5; i++) if (kRates[i] == p.captureRate()) ri = i;
    SendMessageW(byId_[kRate], CB_SETCURSEL, ri, 0);
    EnableWindow(byId_[kRate], !app_.currentDevice.empty());
    SetWindowTextW(byId_[kActive], widen(app_.activeFormatDescription()).c_str());

    SendMessageW(byId_[kMult], CB_SETCURSEL, std::clamp(app_.multiplier - 1, 0, 7), 0);
    // Modes this PC can run (NVIDIA VFG appears once its runtime is found).
    HWND eng = byId_[kEngine];
    SendMessageW(eng, CB_RESETCONTENT, 0, 0);
    engines_.clear();
    int esel = 0;
    for (auto& c : app_.engineChoices()) {
        if (!c.available) continue;
        if (c.kind == app_.engine) esel = int(engines_.size());
        engines_.push_back(c.kind);
        SendMessageW(eng, CB_ADDSTRING, 0, (LPARAM)widen(c.label).c_str());
    }
    SendMessageW(eng, CB_SETCURSEL, esel, 0);
    EnableWindow(eng, app_.multiplier > 1);
    int vm = 1; for (int i = 0; i < 3; i++) if (p.str("vfgModel", "medium") == kVfgModels[i]) vm = i;
    SendMessageW(byId_[kVfgModel], CB_SETCURSEL, vm, 0);
    const auto& vs = Vfg::support();
    EnableWindow(byId_[kVfgModel], vs.supported);
    SetWindowTextW(byId_[kVfgStatus], vs.supported ? (L"Ready — " + vs.dir).c_str() : widen(Gpu::get().isNvidia ? "Not found — choose the folder with NVVideoEffects.dll" : vs.reason).c_str());
    setCheck(byId_[kHud], g_hudLock);
    setCheck(byId_[kAuto], p.autoAdjust());
    setCheck(byId_[kCap], p.displayCapEnabled());

    int si = 0; for (int i = 0; i < 3; i++) if (p.scaling() == kScalings[i]) si = i;
    SendMessageW(byId_[kScaling], CB_SETCURSEL, si, 0);
    int fi = 0; for (int i = 0; i < 5; i++) if (kRefreshes[i] == p.maxRefresh()) fi = i;
    SendMessageW(byId_[kRefresh], CB_SETCURSEL, fi, 0);
    SendMessageW(byId_[kPacing], CB_SETCURSEL, p.pacing() == "vrr" ? 1 : 0, 0);
    setCheck(byId_[kFullscreen], p.startFullScreen());
    setCheck(byId_[kTips], p.showTips());

    HWND outCombo = byId_[kOutput];
    SendMessageW(outCombo, CB_RESETCONTENT, 0, 0);
    outputs_ = AudioPassthrough::outputs();
    SendMessageW(outCombo, CB_ADDSTRING, 0, (LPARAM)L"Windows default output");
    int osel = 0; std::wstring cur = app_.audio.wantedOutput();
    for (size_t i = 0; i < outputs_.size(); i++) {
        SendMessageW(outCombo, CB_ADDSTRING, 0, (LPARAM)widen(outputs_[i].second).c_str());
        if (outputs_[i].first == cur) osel = int(i) + 1;
    }
    SendMessageW(outCombo, CB_SETCURSEL, osel, 0);
    setCheck(byId_[kMute], app_.audio.muted);
    SendMessageW(byId_[kVolume], TBM_SETPOS, TRUE, LPARAM(app_.audio.volume * 100));
    SendMessageW(byId_[kSync], TBM_SETPOS, TRUE, p.audioOffsetMs());
    SetWindowTextW(byId_[kSyncLabel], widen(strf("%+d ms", p.audioOffsetMs())).c_str());

    int ci = 0; for (int i = 0; i < 4; i++) if (p.dashboardCorner() == kCorners[i]) ci = i;
    SendMessageW(byId_[kCorner], CB_SETCURSEL, ci, 0);
    setCheck(byId_[kMini], p.miniCounter());
    refreshing_ = false;
}

void SettingsWindow::onCommand(int id, int code) {
    if (refreshing_) return;
    auto& p = Prefs::get();
    auto cur = [&](int i) { return int(SendMessageW(byId_[i], CB_GETCURSEL, 0, 0)); };
    if (code == CBN_SELCHANGE) {
        switch (id) {
        case kDevice: {
            auto names = app_.deviceNames();
            int i = cur(kDevice);
            app_.selectDevice(i >= 0 && size_t(i) < names.size() ? names[i] : "");
            refresh(); break;
        }
        case kRes: { int i = cur(kRes); if (i >= 0 && size_t(i) < resList_.size()) app_.setCaptureResolution(resList_[i].first, resList_[i].second); refresh(); break; }
        case kRate: { int i = cur(kRate); if (i >= 0 && i < 5) app_.setCaptureRate(kRates[i]); refresh(); break; }
        case kMult: app_.setMultiplier(cur(kMult) + 1); refresh(); break;
        case kEngine: { int i = cur(kEngine); if (i >= 0 && size_t(i) < engines_.size()) app_.setEngine(engines_[i]); break; }
        case kVfgModel:
            p.set("vfgModel", std::string(kVfgModels[std::clamp(cur(kVfgModel), 0, 2)]));
            if (app_.effectiveEngine() == EngineKind::Vfg) app_.applyGeneration();   // reload with the new model
            break;
        case kScaling: app_.setScaling(kScalings[std::clamp(cur(kScaling), 0, 2)]); break;
        case kRefresh: app_.setMaxRefresh(kRefreshes[std::clamp(cur(kRefresh), 0, 4)]); break;
        case kPacing: app_.setPacing(kPacings[std::clamp(cur(kPacing), 0, 1)]); break;
        case kCorner: app_.setDashboardCorner(kCorners[std::clamp(cur(kCorner), 0, 3)]); break;
        case kOutput: { int i = cur(kOutput); app_.setAudioOutput(i > 0 && size_t(i - 1) < outputs_.size() ? outputs_[i - 1].first : L""); break; }
        }
    } else if (code == BN_CLICKED && id == kVfgFolder) {
        chooseVfgFolder();
    } else if (code == BN_CLICKED && id == kLogs) {
        ShellExecuteW(hwnd_, L"open", Install::dataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (code == BN_CLICKED) {
        bool on = isChecked(byId_[id]);
        switch (id) {
        case kHud: app_.setHudLock(on); break;
        case kAuto: p.setBool("autoAdjust", on); app_.clearAuto(); app_.applyGeneration(); break;
        case kCap: p.setBool("displayCapEnabled", on); app_.displayCap.reset(); app_.applyGeneration(); break;
        case kFullscreen: p.setBool("startFullScreen", on); break;
        case kTips: p.setBool("showTips", on); break;
        case kMute: app_.setMuted(on); break;
        case kMini: app_.setMiniCounter(on); break;
        }
    }
}

/// Folder picker for the NVIDIA VFG runtime (the folder holding NVVideoEffects.dll).
void SettingsWindow::chooseVfgFolder() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0; dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose the NVIDIA VFG folder (contains NVVideoEffects.dll)");
    if (FAILED(dlg->Show(hwnd_))) return;
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return;
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    std::wstring dir = path; CoTaskMemFree(path);
    if (!Vfg::isRuntimeFolder(dir) && Vfg::isRuntimeFolder(dir + L"\\bin")) dir += L"\\bin";
    if (!Vfg::isRuntimeFolder(dir)) {
        MessageBoxW(hwnd_, L"That folder doesn't contain the NVIDIA VFG runtime (NVVideoEffects.dll, NVCVImage.dll and nvVFXVideoFrameGeneration.dll).",
                    L"FrameGen", MB_ICONWARNING);
        return;
    }
    Prefs::get().set("vfgRuntime", narrow(dir));
    Vfg::refresh();
    app_.syncControls();
    refresh();
}

void SettingsWindow::snapshotTabs(const std::string& dir) {
    if (!hwnd_) return;
    CreateDirectoryW(widen(dir).c_str(), nullptr);
    for (int i = 0; i < 5; i++) {
        selectTab(i);
        UpdateWindow(hwnd_);
        RECT r; GetWindowRect(hwnd_, &r);
        int w = r.right - r.left, h = r.bottom - r.top;
        HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        PrintWindow(hwnd_, mem, PW_RENDERFULLCONTENT);
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint8_t> px(size_t(w) * h * 4);
        GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
        for (size_t k = 0; k < size_t(w) * h; k++) std::swap(px[4 * k], px[4 * k + 2]);   // BGRA → RGBA
        Gpu::savePNG(px.data(), w, h, strf("%s\\settings_%d.png", dir.c_str(), i));
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    }
    selectTab(0);
}

LRESULT CALLBACK SettingsWindow::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self;
    if (msg == WM_NCCREATE) {
        self = (SettingsWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = h;
    } else {
        self = (SettingsWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
    }
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT SettingsWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NOTIFY:
        if (((NMHDR*)lp)->hwndFrom == tabs_ && ((NMHDR*)lp)->code == TCN_SELCHANGE) selectTab(TabCtrl_GetCurSel(tabs_));
        return 0;
    case WM_COMMAND: onCommand(LOWORD(wp), HIWORD(wp)); return 0;
    case WM_HSCROLL: {
        if (refreshing_) return 0;
        HWND c = (HWND)lp;
        int pos = int(SendMessageW(c, TBM_GETPOS, 0, 0));
        if (c == byId_[kVolume]) { app_.setVolume(pos / 100.f); setCheck(byId_[kMute], false); }
        else if (c == byId_[kSync]) { app_.nudgeSync(pos - Prefs::get().audioOffsetMs()); SetWindowTextW(byId_[kSyncLabel], widen(strf("%+d ms", Prefs::get().audioOffsetMs())).c_str()); }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        int id = GetDlgCtrlID((HWND)lp);
        bool isNote = id == 0 && GetWindowTextLengthW((HWND)lp) > 60;
        SetTextColor(dc, GetSysColor(isNote || id == kActive || id == kVfgStatus ? COLOR_GRAYTEXT : COLOR_WINDOWTEXT));
        static HBRUSH tabBg = CreateSolidBrush(RGB(249, 249, 249));   // themed tab page background
        SetBkColor(dc, RGB(249, 249, 249));
        return (LRESULT)tabBg;
    }
    case WM_CLOSE: ShowWindow(hwnd_, SW_HIDE); return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
