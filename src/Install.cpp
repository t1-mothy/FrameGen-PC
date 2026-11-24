#include "Install.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace {
const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FrameGen";

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring narrowUtf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string wideToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

/// Relative paths from dir's uninstall.list (empty if there's none).
std::vector<std::wstring> readFileList(const std::wstring& dir) {
    std::vector<std::wstring> rel;
    std::ifstream f(std::filesystem::path(dir) / Install::kFileList, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line.find("..") != std::string::npos || line.find(':') != std::string::npos) continue;   // only plain relative paths
        rel.push_back(narrowUtf8ToWide(line));
    }
    return rel;
}

/// Folders that would be left empty by removing `rel`, deepest first, then dir itself.
std::vector<std::wstring> foldersOf(const std::wstring& dir, const std::vector<std::wstring>& rel) {
    std::set<std::wstring> sub;
    for (auto& r : rel)
        for (auto p = std::filesystem::path(r).parent_path(); !p.empty(); p = p.parent_path()) sub.insert(p.wstring());
    std::vector<std::wstring> v(sub.begin(), sub.end());
    std::sort(v.begin(), v.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
    std::vector<std::wstring> out;
    for (auto& s : v) out.push_back(dir + L"\\" + s);
    out.push_back(dir);
    return out;
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
std::wstring defaultDir() { return knownFolder(FOLDERID_ProgramFiles) + L"\\FrameGen"; }
std::wstring legacyDir() { return knownFolder(FOLDERID_LocalAppData) + L"\\Programs\\FrameGen"; }

bool isElevated() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION e{}; DWORD n = 0;
    bool ok = GetTokenInformation(t, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(t);
    return ok;
}

std::wstring dataDir() {
    std::wstring d = knownFolder(FOLDERID_LocalAppData) + L"\\FrameGen";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

static std::wstring installLocation(HKEY root) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(root, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}
std::wstring registeredDir() {
    std::wstring d = installLocation(HKEY_LOCAL_MACHINE);
    return d.empty() ? installLocation(HKEY_CURRENT_USER) : d;
}
bool registeredPerUser() { return installLocation(HKEY_LOCAL_MACHINE).empty() && !installLocation(HKEY_CURRENT_USER).empty(); }

void removePerUserRegistration() {
    DeleteFileW((knownFolder(FOLDERID_Desktop) + L"\\FrameGen.lnk").c_str());
    DeleteFileW((knownFolder(FOLDERID_Programs) + L"\\FrameGen.lnk").c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
}

bool samePath(const std::wstring& a, const std::wstring& b) { return normalized(a) == normalized(b); }

void writeFileList(const std::wstring& dir, const std::vector<std::wstring>& rel) {
    std::ofstream f(std::filesystem::path(dir) / kFileList, std::ios::binary | std::ios::trunc);
    for (auto& r : rel) f << wideToUtf8(r) << "\r\n";
}

void removeOldInstall(const std::wstring& dir) {
    std::error_code ec;
    auto rel = readFileList(dir);
    if (!rel.empty()) {
        for (auto& r : rel) std::filesystem::remove(std::filesystem::path(dir) / r, ec);
        std::filesystem::remove(std::filesystem::path(dir) / kFileList, ec);
        for (auto& d : foldersOf(dir, rel)) RemoveDirectoryW(d.c_str());   // only if empty
    } else if (samePath(dir, legacyDir()) && std::filesystem::exists(std::filesystem::path(dir) / L"FrameGen.exe")) {
        std::filesystem::remove_all(dir, ec);   // pre-1.2.5 install: a folder of its own
    }
}

std::wstring desktopShortcut() { return knownFolder(FOLDERID_PublicDesktop) + L"\\FrameGen.lnk"; }
std::wstring startMenuShortcut() { return knownFolder(FOLDERID_CommonPrograms) + L"\\FrameGen.lnk"; }

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
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
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

int uninstall(bool quiet, bool confirmed) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = std::filesystem::path(exe).parent_path().wstring();
    std::wstring reg = registeredDir();
    auto say = [&](const wchar_t* text, UINT icon) { if (!quiet) MessageBoxW(nullptr, text, L"Uninstall FrameGen", icon); };
    if (const char* t = getenv("FG_UNINSTALL_TRACE")) {   // test aid: where the check below looks
        if (FILE* f = fopen(t, "a")) {
            fwprintf(f, L"[uninstall] registered '%ls' (%ls) | this copy '%ls' (%ls) | exe exists %d | elevated %d\n", reg.c_str(), normalized(reg).c_str(), dir.c_str(),
                     normalized(dir).c_str(), int(std::filesystem::exists(dir + L"\\FrameGen.exe")), int(isElevated()));
            fclose(f);
        }
    }
    // Only ever delete the folder the installer registered, and only from the copy inside it.
    if (reg.empty() || normalized(reg) != normalized(dir) || !std::filesystem::exists(dir + L"\\FrameGen.exe")) {
        say(L"This copy of FrameGen wasn't installed with the FrameGen installer, so there's nothing to uninstall. Just delete its folder.", MB_ICONINFORMATION);
        return 1;
    }
    if (!quiet && !confirmed && MessageBoxW(nullptr, L"Remove FrameGen from this PC?\n\nYour settings are kept, in case you install it again.", L"Uninstall FrameGen", MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;
    // An all-users install (Program Files, HKLM) needs administrator rights to remove: run this
    // again with them (Windows asks), already confirmed. This copy exits as soon as that one is
    // done, so FrameGen.exe can be deleted.
    if (!registeredPerUser() && !isElevated()) {
        SHELLEXECUTEINFOW se{sizeof se};
        se.fMask = SEE_MASK_NOCLOSEPROCESS;
        se.lpVerb = L"runas"; se.lpFile = exe; se.lpParameters = quiet ? L"--uninstall --quiet" : L"--uninstall --confirmed"; se.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&se) || !se.hProcess) {
            say(L"FrameGen wasn't removed: Windows needs administrator permission to remove it.", MB_ICONWARNING);
            return 1;
        }
        WaitForSingleObject(se.hProcess, INFINITE);
        DWORD rc = 1; GetExitCodeProcess(se.hProcess, &rc); CloseHandle(se.hProcess);
        return int(rc);
    }
    if (!closeRunning(8000)) {
        say(L"Please close FrameGen first, then try again.", MB_ICONWARNING);
        return 1;
    }
    if (registeredPerUser()) removePerUserRegistration();   // an install from 1.2.4 or earlier
    else {
        DeleteFileW(desktopShortcut().c_str());
        DeleteFileW(startMenuShortcut().c_str());
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
    }
    // Remove exactly the installed files (never anything else in a folder the user chose). This
    // program is running from the folder, so a short-lived helper deletes FrameGen.exe, the list
    // and the emptied folders once we've exited (two tries, in case exiting takes a moment).
    std::wstring cmd = L"cmd.exe /d /c";
    auto rel = readFileList(dir);
    std::wstring step;
    if (!rel.empty()) {
        std::error_code ec;
        for (auto& r : rel) if (_wcsicmp(r.c_str(), L"FrameGen.exe") != 0) std::filesystem::remove(std::filesystem::path(dir) / r, ec);
        step = L" & del /f /q \"" + dir + L"\\FrameGen.exe\" 2>nul & del /f /q \"" + dir + L"\\" + kFileList + L"\" 2>nul";
        for (auto& d : foldersOf(dir, rel)) step += L" & rmdir \"" + d + L"\" 2>nul";
    } else if (samePath(dir, legacyDir())) {
        step = L" & rmdir /s /q \"" + dir + L"\" 2>nul";   // pre-1.2.5 install: a folder of its own
    } else {
        step = L" & del /f /q \"" + dir + L"\\FrameGen.exe\" 2>nul";
    }
    cmd += L" ping 127.0.0.1 -n 3 >nul" + step + L" & ping 127.0.0.1 -n 4 >nul" + step;
    say(L"FrameGen has been removed.", MB_ICONINFORMATION);   // before the helper starts: it needs us gone
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    STARTUPINFOW si{sizeof si}; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, tmp, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    return 0;
}
}
