// Entry points: FrameGen.exe (Windows app) and framegen-cli.exe (same program as a console app
// for the test tools and --log).
#include "App.h"
#include "Gpu.h"
#include "DlssG.h"
#include "Install.h"
#include <dbghelp.h>
#include <commctrl.h>
#include <mfapi.h>
#include <shellapi.h>
#include <timeapi.h>
#include <cstdio>

bool g_launchedFromConsole = false;
int runListDevices();
int runBench();
int runDump(const std::string& dir);
int runColorCheck();
int runHudCheck();
int runQualityCheck();
int runRealCheck(const std::string& dir);
int runStress(double seconds);

/// Unexpected crash: save a minidump next to the log (%LOCALAPPDATA%\FrameGen) and say where.
static LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) return EXCEPTION_EXECUTE_HANDLER;
    SYSTEMTIME t; GetLocalTime(&t);
    std::wstring path = Install::dataDir() + strfw(L"\\crash-%04d%02d%02d-%02d%02d%02d.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    logf("[crash] exception 0x%08lx — report saved to %s", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0, narrow(path).c_str());
#ifndef FG_CONSOLE
    MessageBoxW(nullptr, (L"FrameGen stopped unexpectedly. Sorry about that!\n\nA crash report was saved to:\n" + path +
                          L"\n\nSend it (with FrameGen.log from the same folder) to whoever gave you FrameGen.").c_str(), L"FrameGen", MB_ICONERROR);
#endif
    return EXCEPTION_EXECUTE_HANDLER;
}

static int appMain(int argc, wchar_t** argv, int show) {
    for (int i = 1; i < argc; i++)
        if (wcscmp(argv[i], L"--uninstall") == 0) {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            bool quiet = false; for (int j = 1; j < argc; j++) quiet = quiet || wcscmp(argv[j], L"--quiet") == 0;
            return Install::uninstall(quiet);
        }
    g_opt = Options::parse(argc, argv);
    SetUnhandledExceptionFilter(onCrash);
#ifndef FG_CONSOLE
    // The app keeps a log of each run (the previous run's too) for troubleshooting.
    if (g_opt.logFile.empty()) {
        std::wstring dir = Install::dataDir();
        MoveFileExW((dir + L"\\FrameGen.log").c_str(), (dir + L"\\FrameGen.previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        g_opt.logFile = narrow(dir + L"\\FrameGen.log");
    }
#endif
    {
        SYSTEMTIME t; GetLocalTime(&t);
        logf("[app] FrameGen %s, started %04d-%02d-%02d %02d:%02d", FG_VERSION, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (g_opt.experimentalDlss || g_opt.checkDlss || getenv("FG_EXPERIMENTAL_DLSS")) DlssG::preInit();   // Streamline must start before any DXGI / Direct3D call
    MFStartup(MF_VERSION);
    timeBeginPeriod(1);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    if (!Gpu::get().init()) {
        MessageBoxW(nullptr, L"FrameGen needs a graphics card with DirectX 11 support, and a current graphics driver.", L"FrameGen", MB_ICONERROR);
        return 1;
    }
    int rc;
    if (g_opt.listDevices) rc = runListDevices();
    else if (g_opt.bench) rc = runBench();
    else if (!g_opt.dumpDir.empty()) rc = runDump(g_opt.dumpDir);
    else if (g_opt.checkColors) rc = runColorCheck();
    else if (g_opt.checkHud) rc = runHudCheck();
    else if (g_opt.checkQuality) rc = runQualityCheck();
    else if (!g_opt.realDir.empty()) rc = runRealCheck(g_opt.realDir);
    else if (g_opt.stress > 0) rc = runStress(g_opt.stress);
    else if (g_opt.checkDlss) rc = DlssG::runCheck();
    else {
        App app;
        rc = app.run(GetModuleHandleW(nullptr), show);
    }
    timeEndPeriod(1);
    MFShutdown();
    return rc;
}

#ifdef FG_CONSOLE
int wmain(int argc, wchar_t** argv) {
    g_launchedFromConsole = true;
    return appMain(argc, argv, SW_SHOWNOACTIVATE);
}
#else
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show) {
    // Launched from a terminal: print --log output there (and don't steal keyboard focus).
    DWORD type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    bool redirected = type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_launchedFromConsole = true;
        if (!redirected) { FILE* f; freopen_s(&f, "CONOUT$", "w", stdout); freopen_s(&f, "CONOUT$", "w", stderr); }
    }
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = appMain(argc, argv, show);
    LocalFree(argv);
    return rc;
}
#endif
