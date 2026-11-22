// Per-user installation (no administrator rights): shared by the installer (installer/Setup.cpp)
// and FrameGen.exe, which uninstalls itself with --uninstall (Windows › Apps › FrameGen).
//   Program files:  %LOCALAPPDATA%\Programs\FrameGen
//   Logs, crashes:  %LOCALAPPDATA%\FrameGen
//   Settings:       %APPDATA%\FrameGen\settings.ini (kept on uninstall)
#pragma once
#include <windows.h>
#include <string>

namespace Install {
std::wstring defaultDir();
std::wstring dataDir();                 // created on demand
std::wstring registeredDir();           // where the installer put FrameGen ("" = not installed)
std::wstring desktopShortcut();
std::wstring startMenuShortcut();
bool createShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& description);
void registerApp(const std::wstring& dir, const std::wstring& version, DWORD sizeKB);
/// Closes running FrameGen windows (asks them to quit). False if one is still open after `ms`.
bool closeRunning(DWORD ms);
/// FrameGen.exe --uninstall [--quiet]: removes the registered installation. Returns the exit code.
int uninstall(bool quiet);
}
