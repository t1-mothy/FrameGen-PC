// FrameGen installer: a small Win32 program with the app's files embedded as resources
// (generated setup_payload.rc: resource 999 = file list, 1000+ = files in that order).
// Installs for all users (setup.manifest asks Windows for administrator rights) to a folder the
// user can choose (default C:\Program Files\FrameGen), adds Start menu / desktop shortcuts and an
// entry in Windows › Apps (uninstall runs FrameGen.exe --uninstall). Every file written is
// recorded in <folder>\uninstall.list, so uninstalling removes only FrameGen's files. An older
// per-user install (1.2.4 and earlier) is removed. FrameGen itself is started without
// administrator rights.
//   FrameGen-Setup-<version>.exe                 → window
//   FrameGen-Setup-<version>.exe /S [/D=folder]  → silent install (desktop shortcut, no launch)
#include "../src/Install.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shldisp.h>
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
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
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

/// Checks a chosen folder. Returns "" if FrameGen can go there, else why not (plain English).
std::wstring checkFolder(const std::wstring& dir) {
    namespace fs = std::filesystem;
    fs::path p(dir);
    if (dir.empty() || !p.is_absolute() || !p.has_root_name()) return L"Please choose a full folder path, e.g. D:\\Games\\FrameGen.";
    if (p == p.root_path()) return L"Please choose a folder, not a whole drive (e.g. " + p.root_name().wstring() + L"\\FrameGen).";
    std::error_code ec;
    // A folder that already holds other things (not an earlier FrameGen): install into a subfolder instead.
    if (fs::is_directory(p, ec) && !fs::is_empty(p, ec) && !fs::exists(p / L"FrameGen.exe", ec) && !fs::exists(p / Install::kFileList, ec))
        return L"That folder already contains other files. Choose an empty folder, or use Browse… (it creates a FrameGen folder inside the one you pick).";
    fs::create_directories(p, ec);
    std::wstring probe = (p / L".framegen-write-test").wstring();
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return L"FrameGen can't write to " + dir + L". Please choose another folder.";
    CloseHandle(h);
    return L"";
}

/// Installs everything into dir; progress(0..1000). Returns "" or an error message.
std::wstring install(const std::wstring& dir, bool desktop, const std::function<void(int)>& progress) {
    auto files = payload();
    if (files.empty()) return L"This installer is damaged (no files inside). Please download it again.";
    if (std::wstring why = checkFolder(dir); !why.empty()) return why;
    if (!Install::closeRunning(8000)) return L"Please close FrameGen, then click Install again.";
    std::wstring previous = Install::registeredDir();
    bool previousPerUser = Install::registeredPerUser();
    unsigned long long total = 0, done = 0;
    for (auto& f : files) total += f.size;
    std::vector<std::wstring> written;
    for (auto& f : files) {
        std::wstring err;
        if (!writeFile(dir + L"\\" + f.rel, f.data, f.size, err, [&](DWORD n) { done += n; progress(int(done * 1000 / std::max(total, 1ull))); })) return err;
        written.push_back(f.rel);
    }
    Install::writeFileList(dir, written);
    // Moved to another folder: remove the earlier install's files (and only those).
    if (!previous.empty() && !Install::samePath(previous, dir)) Install::removeOldInstall(previous);
    std::wstring exe = dir + L"\\FrameGen.exe";
    Install::createShortcut(Install::startMenuShortcut(), exe, L"Real-time frame generation for capture cards");
    if (desktop) Install::createShortcut(Install::desktopShortcut(), exe, L"Real-time frame generation for capture cards");
    else DeleteFileW(Install::desktopShortcut().c_str());
    Install::registerApp(dir, widen(FG_VERSION), DWORD(total / 1024));
    if (previousPerUser) Install::removePerUserRegistration();   // 1.2.4 or earlier: now installed for all users
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    progress(1000);
    return L"";
}

/// Starts a program as the signed-in user (without administrator rights), through Explorer.
void launchUnelevated(const std::wstring& exe, const std::wstring& dir) {
    bool ok = false;
    IShellWindows* windows = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&windows)))) {
        VARIANT loc{}; loc.vt = VT_I4; loc.lVal = CSIDL_DESKTOP;   // VariantInit + CSIDL
        VARIANT empty{};
        long hwnd = 0;
        IDispatch* disp = nullptr;
        if (SUCCEEDED(windows->FindWindowSW(&loc, &empty, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp)) && disp) {
            IServiceProvider* sp = nullptr;
            IShellBrowser* browser = nullptr;
            IShellView* view = nullptr;
            IDispatch* bg = nullptr;
            IShellFolderViewDual* fv = nullptr;
            IDispatch* appDisp = nullptr;
            IShellDispatch2* shell = nullptr;
            if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&sp))) &&
                SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) &&
                SUCCEEDED(browser->QueryActiveShellView(&view)) &&
                SUCCEEDED(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&bg))) &&
                SUCCEEDED(bg->QueryInterface(IID_PPV_ARGS(&fv))) &&
                SUCCEEDED(fv->get_Application(&appDisp)) &&
                SUCCEEDED(appDisp->QueryInterface(IID_PPV_ARGS(&shell)))) {
                BSTR file = SysAllocString(exe.c_str());
                VARIANT args{}, wd{}, verb{}, showCmd{};
                wd.vt = VT_BSTR; wd.bstrVal = SysAllocString(dir.c_str());
                showCmd.vt = VT_I4; showCmd.lVal = SW_SHOWNORMAL;
                ok = SUCCEEDED(shell->ShellExecute(file, args, wd, verb, showCmd));
                SysFreeString(file); SysFreeString(wd.bstrVal);
            }
            for (IUnknown* u : std::initializer_list<IUnknown*>{shell, appDisp, fv, bg, view, browser, sp, disp}) if (u) u->Release();
        }
        windows->Release();
    }
    if (!ok) ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);   // no Explorer running
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

enum { kInstall = 1, kCancel, kDesktop, kLaunch, kLicences, kProgress, kStatus, kDir, kBrowse };
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

HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f = nullptr, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, g.px(x), g.px(y), g.px(w), g.px(h), g.wnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)(f ? f : g.font), TRUE);
    return c;
}

std::wstring textOf(int id) {
    HWND c = g.item(id);
    std::wstring s(size_t(GetWindowTextLengthW(c)) + 1, L'\0');
    GetWindowTextW(c, s.data(), int(s.size()));
    s.resize(wcslen(s.c_str()));
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    while (!s.empty() && s.front() == L' ') s.erase(s.begin());
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return s;
}

/// Folder picker. The chosen folder gets a FrameGen subfolder unless it's already called FrameGen.
void browse() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0; dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Choose where to install FrameGen");
    std::filesystem::path start(textOf(kDir));
    std::error_code ec;
    while (!start.empty() && !std::filesystem::is_directory(start, ec) && start != start.root_path()) start = start.parent_path();
    IShellItem* folder = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) { dlg->SetFolder(folder); folder->Release(); }
    if (SUCCEEDED(dlg->Show(g.wnd))) {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            std::filesystem::path p(path);
            if (_wcsicmp(p.filename().c_str(), L"FrameGen") != 0) p /= L"FrameGen";
            SetWindowTextW(g.item(kDir), p.c_str());
            CoTaskMemFree(path);
        }
        if (item) item->Release();
    }
    dlg->Release();
}

void build() {
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g.dpi);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont; lf.lfHeight = -MulDiv(20, g.dpi, 72); lf.lfWeight = FW_SEMIBOLD;
    g.title = CreateFontIndirectW(&lf);
    add(L"STATIC", L"FrameGen", SS_LEFT, 24, 18, 460, 40, 0, g.title);
    add(L"STATIC", L"Real-time frame generation for capture cards, compatible with NVIDIA VFG (RTX 3*/4/5000 series), DLSS, AMD FSR, and other custom frame generation methods.",
        SS_LEFT, 24, 62, 460, 40, 0);
    add(L"STATIC", L"Choose install path:", SS_LEFT, 24, 112, 460, 20, 0);
    add(L"EDIT", g.dir.c_str(), ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, 24, 134, 368, 24, kDir, nullptr, 0);
    add(L"BUTTON", L"Browse…", BS_PUSHBUTTON | WS_TABSTOP, 400, 133, 84, 26, kBrowse);
    HWND d = add(L"BUTTON", L"Create a desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 170, 300, 22, kDesktop);
    HWND l = add(L"BUTTON", L"Start FrameGen when Done", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 194, 300, 22, kLaunch);
    SendMessageW(d, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(l, BM_SETCHECK, BST_CHECKED, 0);
    add(L"STATIC", L"Not affiliated with any console or capture card trademarks. By installing you agree to NVIDIA's and AMD's licensing (for NVIDIA VFG, DLSS and AMD FSR).",
        SS_LEFT, 24, 226, 360, 52, 0);
    add(L"BUTTON", L"View licences", BS_PUSHBUTTON | WS_TABSTOP, 392, 228, 92, 26, kLicences);
    add(L"STATIC", L"*Experimental use only for RTX 3000 series.", SS_LEFT, 24, 282, 460, 18, 0);
    add(PROGRESS_CLASSW, L"", 0, 24, 310, 460, 14, kProgress);
    SendMessageW(g.item(kProgress), PBM_SETRANGE32, 0, 1000);
    add(L"STATIC", L"", SS_LEFT, 24, 330, 460, 20, kStatus);
    add(L"BUTTON", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 298, 360, 90, 28, kInstall);
    add(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 394, 360, 90, 28, kCancel);
    if (!Install::registeredDir().empty()) SetWindowTextW(g.item(kInstall), L"Update");
    SetFocus(g.item(kInstall));
}

void setInputsEnabled(bool on) {
    for (int id : {kInstall, kCancel, kDesktop, kLaunch, kLicences, kDir, kBrowse}) EnableWindow(g.item(id), on);
}

void startInstall() {
    std::wstring dir = textOf(kDir);
    if (std::wstring why = checkFolder(dir); !why.empty()) { MessageBoxW(g.wnd, why.c_str(), L"FrameGen Setup", MB_ICONWARNING); return; }
    g.dir = dir;
    g.busy = true;
    setInputsEnabled(false);
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
                    launchUnelevated(g.dir + L"\\FrameGen.exe", g.dir);
                DestroyWindow(h);
            } else startInstall();
            return 0;
        case kCancel: DestroyWindow(h); return 0;
        case kLicences: showLicences(h); return 0;
        case kBrowse: browse(); return 0;
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
            setInputsEnabled(true);
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
    if (g.dir.empty() || (Install::registeredPerUser() && Install::samePath(g.dir, Install::legacyDir()))) g.dir = Install::defaultDir();
    std::wstring args = cmd ? cmd : L"";
    if (args.find(L"/S") != std::wstring::npos || args.find(L"/silent") != std::wstring::npos) {
        if (size_t d = args.find(L"/D="); d != std::wstring::npos) {   // /D=folder (the rest of the command line)
            std::wstring dir = args.substr(d + 3);
            while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"')) dir.pop_back();
            if (!dir.empty() && dir.front() == L'"') dir.erase(dir.begin());
            if (!dir.empty()) g.dir = dir;
        }
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
    RECT r{0, 0, g.px(508), g.px(406)};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, g.dpi);
    RECT work; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = r.right - r.left, h = r.bottom - r.top;
    std::wstring caption = L"FrameGen " + widen(FG_VERSION) + L" Setup";
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, caption.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
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
