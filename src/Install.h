// Installation for all users of the PC (the installer asks for administrator rights): shared by
// the installer (installer/Setup.cpp) and FrameGen.exe, which uninstalls itself with --uninstall
// (Windows › Apps › FrameGen; asks for administrator rights too).
//   Program files:  chosen in the installer (default C:\Program Files\FrameGen)
//   Registration:   HKLM\...\Uninstall\FrameGen; shortcuts in the all-users Start menu / desktop
//   Logs, crashes:  %LOCALAPPDATA%\FrameGen (per user)
//   Settings:       %APPDATA%\FrameGen\settings.ini (per user, kept on uninstall)
// Versions up to 1.2.4 installed per user (%LOCALAPPDATA%\Programs\FrameGen, HKCU, the user's own
// shortcuts); installing this version removes such an install.
// The installer records every file it writes in <install folder>\uninstall.list; uninstalling (or
// moving the install elsewhere) removes exactly those files and the folders they leave empty —
// never anything else in a folder the user chose.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace Install {
constexpr const wchar_t* kFileList = L"uninstall.list";
std::wstring defaultDir();               // C:\Program Files\FrameGen
std::wstring legacyDir();                // per-user default of versions up to 1.2.4
std::wstring dataDir();                  // created on demand
std::wstring registeredDir();            // where the installer put FrameGen ("" = not installed)
bool registeredPerUser();                // that registration is an old per-user one (HKCU)
std::wstring desktopShortcut();          // all users
std::wstring startMenuShortcut();        // all users
/// Removes an old per-user registration and its shortcuts (not its files).
void removePerUserRegistration();
bool isElevated();
bool createShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& description);
void registerApp(const std::wstring& dir, const std::wstring& version, DWORD sizeKB);
bool samePath(const std::wstring& a, const std::wstring& b);
/// Writes uninstall.list (relative paths, one per line) into dir.
void writeFileList(const std::wstring& dir, const std::vector<std::wstring>& rel);
/// Removes an earlier install's files from dir (now: those in uninstall.list; installs from
/// before 1.2.5 had no list and always lived in legacyDir(), which then goes entirely).
void removeOldInstall(const std::wstring& dir);
/// Closes running FrameGen windows (asks them to quit). False if one is still open after `ms`.
bool closeRunning(DWORD ms);
/// FrameGen.exe --uninstall [--quiet | --confirmed]: removes the registered installation (asking
/// for administrator rights if needed). Returns the exit code.
int uninstall(bool quiet, bool confirmed = false);
}
