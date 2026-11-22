#include "Install.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <filesystem>

namespace {
const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FrameGen";

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring normalized(std::wstring p) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(p, ec);
    if (!ec) p = c.wstring();
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    CharLowerBuffW(p.data(), DWORD(p.size()));
    return p;
}
}

namespace Install {
std::wstring defaultDir() { return knownFolder(FOLDERID_LocalAppData) + L"\\Programs\\FrameGen"; }

std::wstring dataDir() {
    std::wstring d = knownFolder(FOLDERID_LocalAppData) + L"\\FrameGen";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring registeredDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

std::wstring desktopShortcut() { return knownFolder(FOLDERID_Desktop) + L"\\FrameGen.lnk"; }
std::wstring startMenuShortcut() { return knownFolder(FOLDERID_Programs) + L"\\FrameGen.lnk"; }

bool createShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& description) {
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(std::filesystem::path(target).parent_path().c_str());
    link->SetDescription(description.c_str());
    link->SetIconLocation(target.c_str(), 0);
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(lnk.c_str(), TRUE));
    if (file) file->Release();
    link->Release();
    return ok;
}

void registerApp(const std::wstring& dir, const std::wstring& version, DWORD sizeKB) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    auto str = [&](const wchar_t* name, const std::wstring& v) { RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t))); };
    auto num = [&](const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v); };
    std::wstring exe = dir + L"\\FrameGen.exe";
    str(L"DisplayName", L"FrameGen");
    str(L"DisplayVersion", version);
    str(L"Publisher", L"FrameGen");
    str(L"DisplayIcon", exe);
    str(L"InstallLocation", dir);
    str(L"UninstallString", L"\"" + exe + L"\" --uninstall");
    str(L"QuietUninstallString", L"\"" + exe + L"\" --uninstall --quiet");
    num(L"NoModify", 1);
    num(L"NoRepair", 1);
    num(L"EstimatedSize", sizeKB);
    RegCloseKey(k);
}

bool closeRunning(DWORD ms) {
    DWORD me = GetCurrentProcessId();
    auto others = [&](bool close) {
        bool any = false;
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, L"FrameGenMain", nullptr)) != nullptr) {
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            if (pid == me) continue;
            any = true;
            if (close) PostMessageW(h, WM_CLOSE, 0, 0);
        }
        return any;
    };
    if (!others(true)) return true;
    for (DWORD t = 0; t < ms; t += 100) { Sleep(100); if (!others(false)) { Sleep(300); return true; } }
    return false;
}

int uninstall(bool quiet) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = std::filesystem::path(exe).parent_path().wstring();
    std::wstring reg = registeredDir();
    // Only ever delete the folder the installer registered, and only from the copy inside it.
    if (reg.empty() || normalized(reg) != normalized(dir) || !std::filesystem::exists(dir + L"\\FrameGen.exe")) {
        if (!quiet) MessageBoxW(nullptr, L"This copy of FrameGen wasn't installed with the FrameGen installer, so there's nothing to uninstall. Just delete its folder.", L"FrameGen", MB_ICONINFORMATION);
        return 1;
    }
    if (!quiet && MessageBoxW(nullptr, L"Remove FrameGen from this PC?\n\nYour settings are kept, in case you install it again.", L"Uninstall FrameGen", MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;
    if (!closeRunning(8000)) {
        if (!quiet) MessageBoxW(nullptr, L"Please close FrameGen first, then try again.", L"Uninstall FrameGen", MB_ICONWARNING);
        return 1;
    }
    DeleteFileW(desktopShortcut().c_str());
    DeleteFileW(startMenuShortcut().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    // This program is running from the folder: a short-lived helper deletes it once we've exited.
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring cmd = L"cmd.exe /d /c ping 127.0.0.1 -n 3 >nul & rmdir /s /q \"" + dir + L"\"";
    STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, tmp, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    if (!quiet) MessageBoxW(nullptr, L"FrameGen has been removed.", L"Uninstall FrameGen", MB_ICONINFORMATION);
    return 0;
}
}
