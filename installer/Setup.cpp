// FrameGen installer: a small Win32 program with the app's files embedded as resources
// (generated setup_payload.rc: resource 999 = file list, 1000+ = files in that order).
// Installs per user (no administrator rights) to %LOCALAPPDATA%\Programs\FrameGen, adds Start
// menu / desktop shortcuts and an entry in Windows › Apps (uninstall runs FrameGen.exe --uninstall).
//   FrameGen-Setup-<version>.exe        → window
//   FrameGen-Setup-<version>.exe /S     → silent install (desktop shortcut, no launch)
#include "../src/Install.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifndef FG_VERSION
#define FG_VERSION "0.0.0"
#endif

namespace {
struct Payload { std::wstring rel; const BYTE* data; DWORD size; };

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

const BYTE* resource(int id, DWORD& size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return nullptr;
    HGLOBAL g = LoadResource(nullptr, r);
    size = SizeofResource(nullptr, r);
    return g ? (const BYTE*)LockResource(g) : nullptr;
}

std::vector<Payload> payload() {
    std::vector<Payload> files;
    DWORD n = 0;
    const BYTE* list = resource(999, n);
    if (!list) return files;
    std::string text((const char*)list, n);
    size_t p = 0;
    int id = 1000;
    while (p < text.size()) {
        size_t q = text.find('\n', p); if (q == std::string::npos) q = text.size();
        std::string line = text.substr(p, q - p);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\0')) line.pop_back();
        if (!line.empty()) {
            Payload f{widen(line), nullptr, 0};
            for (auto& c : f.rel) if (c == L'/') c = L'\\';
            f.data = resource(id, f.size);
            if (f.data) files.push_back(f);
            id++;
        }
        p = q + 1;
    }
    return files;
}

bool writeFile(const std::wstring& path, const BYTE* data, DWORD size, std::wstring& err, const std::function<void(DWORD)>& progress) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::wstring tmp = path + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = L"Couldn't write " + path; return false; }
    DWORD done = 0;
    while (done < size) {
        DWORD chunk = std::min<DWORD>(size - done, 4u << 20), wrote = 0;
        if (!WriteFile(h, data + done, chunk, &wrote, nullptr) || wrote == 0) { CloseHandle(h); DeleteFileW(tmp.c_str()); err = L"Couldn't write " + path + L" (is the disk full?)"; return false; }
        done += wrote;
        progress(wrote);
    }
    CloseHandle(h);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); err = L"Couldn't replace " + path + L" (is FrameGen still running?)"; return false; }
    return true;
}

/// Installs everything; progress(0..1000). Returns "" or an error message.
std::wstring install(const std::wstring& dir, bool desktop, const std::function<void(int)>& progress) {
    auto files = payload();
    if (files.empty()) return L"This installer is damaged (no files inside). Please download it again.";
    if (!Install::closeRunning(8000)) return L"Please close FrameGen, then click Install again.";
    unsigned long long total = 0, done = 0;
    for (auto& f : files) total += f.size;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (auto& f : files) {
        std::wstring err;
        if (!writeFile(dir + L"\\" + f.rel, f.data, f.size, err, [&](DWORD n) { done += n; progress(int(done * 1000 / std::max(total, 1ull))); })) return err;
    }
    std::wstring exe = dir + L"\\FrameGen.exe";
    Install::createShortcut(Install::startMenuShortcut(), exe, L"Real-time frame generation for capture cards");
    if (desktop) Install::createShortcut(Install::desktopShortcut(), exe, L"Real-time frame generation for capture cards");
    else DeleteFileW(Install::desktopShortcut().c_str());
    Install::registerApp(dir, widen(FG_VERSION), DWORD(total / 1024));
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    progress(1000);
    return L"";
}

/// Extracts the licence files to %TEMP% and shows them.
void showLicences(HWND owner) {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"FrameGen licences";
    for (auto& f : payload()) {
        std::wstring lower = f.rel; CharLowerBuffW(lower.data(), DWORD(lower.size()));
        if (lower.find(L"license") == std::wstring::npos && lower.find(L"licence") == std::wstring::npos && lower.find(L"notices") == std::wstring::npos) continue;
        std::wstring err;
        writeFile(dir + L"\\" + std::filesystem::path(f.rel).filename().wstring(), f.data, f.size, err, [](DWORD) {});
    }
    ShellExecuteW(owner, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- Window

enum { kInstall = 1, kCancel, kDesktop, kLaunch, kLicences, kProgress, kStatus, kDir };
constexpr UINT WM_PROGRESS = WM_APP + 1, WM_DONE = WM_APP + 2;

struct Ui {
    HWND wnd = nullptr;
    HFONT font = nullptr, title = nullptr;
    int dpi = 96;
    std::wstring dir;
    bool busy = false, finished = false;
    std::wstring error;
    int px(int v) const { return MulDiv(v, dpi, 96); }
    HWND item(int id) const { return GetDlgItem(wnd, id); }
} g;

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, g.px(x), g.px(y), g.px(w), g.px(h), g.wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g.font), TRUE);
    return c;
}

void build() {
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g.dpi);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont; lf.lfHeight = -MulDiv(20, g.dpi, 72); lf.lfWeight = FW_SEMIBOLD;
    g.title = CreateFontIndirectW(&lf);
    bool vfg = false;
    for (auto& f : payload()) if (f.rel.rfind(L"nvidia-vfx\\", 0) == 0) vfg = true;
    add(L"STATIC", (L"FrameGen " + widen(FG_VERSION)).c_str(), SS_LEFT, 24, 18, 460, 40, 0, g.title);
    add(L"STATIC", vfg ? L"Real-time frame generation for console capture cards. Includes NVIDIA VFG and NVIDIA DLSS Frame Generation for RTX 40 / 50 graphics cards."
                       : L"Real-time frame generation for console capture cards.",
        SS_LEFT, 24, 62, 460, 40, 0);
    add(L"STATIC", L"Installs for your Windows account (no administrator rights needed) in:", SS_LEFT, 24, 110, 460, 20, 0);
    add(L"STATIC", g.dir.c_str(), SS_LEFT | SS_PATHELLIPSIS, 36, 130, 448, 20, kDir);
    HWND d = add(L"BUTTON", L"Create a desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 160, 300, 22, kDesktop);
    HWND l = add(L"BUTTON", L"Start FrameGen when done", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 184, 300, 22, kLaunch);
    SendMessageW(d, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(l, BM_SETCHECK, BST_CHECKED, 0);
    add(L"STATIC", vfg ? L"FrameGen is MIT licensed. The included NVIDIA files (VFG, DLSS) are covered by NVIDIA's licences; by installing you accept them."
                       : L"FrameGen is MIT licensed.", SS_LEFT, 24, 214, 360, 36, 0);
    add(L"BUTTON", L"View licences", BS_PUSHBUTTON | WS_TABSTOP, 392, 216, 92, 26, kLicences);
    add(PROGRESS_CLASSW, L"", 0, 24, 260, 460, 14, kProgress);
    SendMessageW(g.item(kProgress), PBM_SETRANGE32, 0, 1000);
    add(L"STATIC", L"", SS_LEFT, 24, 280, 460, 20, kStatus);
    add(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 298, 310, 90, 28, kInstall);
    add(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 394, 310, 90, 28, kCancel);
    if (!Install::registeredDir().empty()) SetWindowTextW(g.item(kInstall), L"Update");
    SetFocus(g.item(kInstall));
}

void startInstall() {
    g.busy = true;
    for (int id : {kInstall, kCancel, kDesktop, kLaunch, kLicences}) EnableWindow(g.item(id), FALSE);
    SetWindowTextW(g.item(kStatus), L"Installing…");
    bool desktop = SendMessageW(g.item(kDesktop), BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::thread([desktop] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::wstring err = install(g.dir, desktop, [](int p) { PostMessageW(g.wnd, WM_PROGRESS, WPARAM(p), 0); });
        g.error = err;
        PostMessageW(g.wnd, WM_DONE, err.empty(), 0);
        CoUninitialize();
    }).detach();
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: g.wnd = h; build(); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kInstall:
            if (g.finished) {
                if (SendMessageW(g.item(kLaunch), BM_GETCHECK, 0, 0) == BST_CHECKED)
                    ShellExecuteW(nullptr, L"open", (g.dir + L"\\FrameGen.exe").c_str(), nullptr, g.dir.c_str(), SW_SHOWNORMAL);
                DestroyWindow(h);
            } else startInstall();
            return 0;
        case kCancel: DestroyWindow(h); return 0;
        case kLicences: showLicences(h); return 0;
        }
        break;
    case WM_PROGRESS: SendMessageW(g.item(kProgress), PBM_SETPOS, wp, 0); return 0;
    case WM_DONE:
        g.busy = false;
        if (wp) {
            g.finished = true;
            SetWindowTextW(g.item(kStatus), L"FrameGen is installed. You'll find it in the Start menu.");
            SetWindowTextW(g.item(kInstall), L"Finish");
            EnableWindow(g.item(kInstall), TRUE);
            EnableWindow(g.item(kLaunch), TRUE);
        } else {
            SetWindowTextW(g.item(kStatus), L"Installation didn't finish.");
            MessageBoxW(h, g.error.c_str(), L"FrameGen Setup", MB_ICONWARNING);
            for (int id : {kInstall, kCancel, kDesktop, kLaunch, kLicences}) EnableWindow(g.item(id), TRUE);
        }
        return 0;
    case WM_CLOSE: if (g.busy) return 0; break;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp; SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g.dir = Install::registeredDir();
    if (g.dir.empty()) g.dir = Install::defaultDir();
    std::wstring args = cmd ? cmd : L"";
    if (args.find(L"/S") != std::wstring::npos || args.find(L"/silent") != std::wstring::npos) {
        std::wstring err = install(g.dir, true, [](int) {});
        return err.empty() ? 0 : 1;
    }
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"FrameGenSetup";
    RegisterClassExW(&wc);
    HDC screen = GetDC(nullptr); g.dpi = GetDeviceCaps(screen, LOGPIXELSY); ReleaseDC(nullptr, screen);
    RECT r{0, 0, g.px(508), g.px(356)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, g.dpi);
    RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"FrameGen Setup", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, inst, nullptr);
    if (!wnd) return 1;
    ShowWindow(wnd, show);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (IsDialogMessageW(wnd, &m)) continue;
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    return 0;
}
