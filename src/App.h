// The app: window, sources, frame generation settings, automatic adjustment, dashboard wiring.
// The Windows counterpart of the Mac app's AppDelegate (main.swift).
#pragma once
#include "Audio.h"
#include "Capture.h"
#include "Dashboard.h"
#include "Pipeline.h"
#include "Renderer.h"

class SettingsWindow;

class App {
public:
    App();
    ~App();
    int run(HINSTANCE inst, int show);

    // ---- State (read by the Settings window)
    Stats stats;
    AudioPassthrough audio;
    std::unique_ptr<AudioCapture> audioCap;
    std::unique_ptr<FramePipeline> pipeline;
    std::unique_ptr<Presenter> renderer;   // Renderer (D3D11) or DlssPresenter (D3D12 + DLSS-G)
    Dashboard dashboard;
    std::unique_ptr<FrameSource> source;
    std::string currentDevice;          // "" = test pattern
    EngineKind engine = EngineKind::Fast;
    int multiplier = 2;
    bool dashboardVisible = true, miniCounterOn = true;
    bool micDenied = false;
    HWND hwnd = nullptr;

    // Automatic performance adjustment (slower PCs): temporary limits on top of the user's choice.
    std::optional<int> autoCap;
    std::optional<EngineKind> autoEngine;
    bool autoOff = false;
    std::string autoNote;
    double lastAdjust = 0;
    int lastDropped = 0;
    int slowTicks_ = 0;
    // Never generate more frames than the display can show.
    std::optional<int> displayCap;
    std::optional<std::pair<int, double>> displayCapCandidate;

    int effectiveMultiplier() const;
    EngineKind effectiveEngine() const { return autoEngine.value_or(engine); }
    double displayHz();

    // ---- Actions
    void selectDevice(const std::string& name);
    void setCaptureResolution(int w, int h);
    void setCaptureRate(int hz);
    void setScaling(const std::string& mode);
    void setMaxRefresh(int hz);
    void setPacing(const std::string& mode);
    void setMultiplier(int m);
    void setEngine(EngineKind k);
    void setHudLock(bool on);
    void setVolume(float v);
    void setMuted(bool m);
    void setAudioOutput(const std::wstring& id);
    void nudgeSync(int ms);
    void toggleDashboard();
    void toggleFullscreen();
    void showSettings();
    void setDashboardCorner(const std::string& corner);
    void setMiniCounter(bool on);
    void clearAuto();
    void applyGeneration();
    void syncControls();
    bool engineBlocked() const;
    std::string engineBlockReason() const;
    std::vector<Dashboard::EngineChoice> engineChoices();
    void showContextMenu(POINT screen);
    bool generating();      // frames are being generated (own engine or DLSS-G)
    bool ensurePresenter(); // the right presenter for the current engine

    std::vector<std::string> deviceNames(bool refresh = false);
    std::vector<std::pair<int, int>> supportedResolutions();
    std::string activeFormatDescription();

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void startInitialSource();
    void startSource(const std::string& kind, const std::string& device, int w, int h, double fps);
    void launch(std::unique_ptr<FrameSource> src);
    void devicesChanged();
    void tick();
    void updateDisplayCap(const Stats::Snapshot& s);
    void enforceEngineLimits();
    void autoAdjust(const Stats::Snapshot& s);
    void refreshDashboard(const Stats::Snapshot* snap = nullptr);
    void placeOverlays();
    void logStats();
    bool handleKey(WPARAM vk);
    std::optional<std::pair<int, int>> lowerResolution();
    int currentHeight();
    std::string pacingTip(double outputTarget);
    std::string displayCapNote();
    void openPrivacySettings(const wchar_t* page);

    std::unique_ptr<SettingsWindow> settings_;
    bool fullscreen_ = false;
    RECT windowedRect_{};
    LONG windowedStyle_ = 0;
    bool waitingForPermission_ = false;
    bool permissionDenied_ = false;
    std::unique_ptr<TestTone> tone_;
    std::vector<std::string> cachedDevices_;
    bool devicesCached_ = false;
    HDEVNOTIFY devNotify_ = nullptr;
    double startTime_ = 0;
    int frameCount_ = 0;
    int cycleIndex_ = 0;
    POINT dashOrigin_{}, miniOrigin_{};
    bool dashCaptured_ = false;
    bool shotDone_ = false;
    double lastMouseMove_ = 0;
    bool cursorHidden_ = false;
};

extern App* g_app;
