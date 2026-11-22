// The on-screen dashboard (H) and the small FPS counter shown when it's hidden — the same layout,
// wording and controls as the Mac app. Drawn with Direct2D into overlay textures that the
// presenter composites over the video; mouse input is hit-tested here.
#pragma once
#include "Gpu.h"

class Dashboard {
public:
    struct Live {
        bool live = false;
        std::string status;
        double outputFPS = 0, gameFPS = 0, targetFPS = 0, delayMs = 0, genMs = 0, displayHz = 0;
        std::string signal, warning, sound, actionTitle;
        std::function<void()> action;
    };

    /// One entry of the Mode menu.
    struct EngineChoice { EngineKind kind; std::string label; bool available; std::string why; };

    // Callbacks to the app
    std::function<void(int)> onMultiplier;
    std::function<void(EngineKind)> onEngine;
    std::function<void(std::string)> onSource;   // device name, "" = test pattern
    std::function<void(bool)> onCompare, onMiniCounter, onHudLock, onMute;
    std::function<void()> onFullscreen, onHide, onSettings;
    std::function<void(int, int)> onResolution;
    std::function<void(float)> onVolume;

    void setScale(float s) { if (s != scale_) { scale_ = s; image_.reset(); mini_.reset(); fonts_ = false; } }
    void setControls(int multiplier, EngineKind engine, bool compare, bool miniCounter, float volume, bool muted, bool hudLock, bool enginesOK,
                     const std::vector<EngineChoice>& engines);
    void setSources(const std::vector<std::string>& devices, const std::string& selected);
    void setResolutions(const std::vector<std::pair<int, int>>& list, std::pair<int, int> selected);
    void update(const Live& s);

    /// Redraws the dashboard texture (UI thread). Returns the image to composite.
    ImagePtr render();
    ImagePtr renderMini(double fps, bool healthy);
    int width() const { return int(340 * scale_); }
    int height() const { return int(contentH_ * scale_); }
    int miniWidth() const { return int(miniW_ * scale_); }
    int miniHeight() const { return int(26 * scale_); }

    /// Mouse input in dashboard pixel coordinates. Returns true when the dashboard handled it.
    bool mouseDown(int x, int y, HWND hwnd, POINT screenPt);
    void mouseMove(int x, int y);
    void mouseUp();
    bool dragging() const { return dragSlider_; }
    /// Mode menu entries (ids firstId + index), shared with the right-click menu.
    static void appendEngineItems(HMENU m, const std::vector<EngineChoice>& list, EngineKind current, UINT firstId);

private:
    enum Id { kNone, kHide, kMult, kEngine, kSource, kRes, kMute, kVolume, kHud, kCompare, kAction, kFull, kSettings, kMini };
    struct Item { Id id; D2D1_RECT_F r; int segments = 0; };
    void ensureFonts();
    float layoutAndDraw(ID2D1DeviceContext* dc, bool draw);
    std::vector<Item> items_;
    float scale_ = 1;
    float contentH_ = 600, miniW_ = 80;
    bool fonts_ = false;
    ComPtr<IDWriteTextFormat> fCaption_, fLabel_, fSmall_, fStatus_, fBig_, fMono_, fValue_, fIcon_, fButton_;
    ComPtr<ID2D1DeviceContext> dc_;
    ImagePtr image_, mini_;
    std::mutex m_;
    // State
    Live live_;
    std::deque<double> history_;
    int multiplier_ = 2; EngineKind engine_ = EngineKind::Fast;
    bool compare_ = false, miniOn_ = true, muted_ = false, hud_ = true, enginesOK_ = true;
    std::vector<EngineChoice> engines_;
    float volume_ = 0.8f;
    std::vector<std::string> sources_; std::string selectedSource_;
    std::vector<std::pair<int, int>> resolutions_; std::pair<int, int> selectedRes_{0, 0};
    bool dragSlider_ = false; D2D1_RECT_F sliderRect_{};
};
