// The Settings window: every option in one place, in the same five tabs as the Mac app
// (Video, Frame Generation, Display, Sound, Interface) with the same explanatory notes.
#pragma once
#include "Core.h"
#include <map>

class App;

class SettingsWindow {
public:
    explicit SettingsWindow(App& app) : app_(app) {}
    ~SettingsWindow();
    void show(HWND owner);
    void refresh();
    HWND hwnd() const { return hwnd_; }
    void snapshotTabs(const std::string& dir);   // test aid: PNG of each tab

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    void build();
    HWND add(int tab, const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y, int w, int h, int id = 0);
    int label(int tab, const std::wstring& text, int y);
    int note(int tab, const std::wstring& text, int y);
    void selectTab(int i);
    void onCommand(int id, int code);
    void chooseVfgFolder();
    int px(int v) const { return MulDiv(v, dpi_, 96); }

    App& app_;
    HWND hwnd_ = nullptr, tabs_ = nullptr;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    std::vector<std::pair<int, HWND>> controls_;   // (tab, control)
    std::map<int, HWND> byId_;
    bool refreshing_ = false;
    std::vector<std::pair<int, int>> resList_;
    std::vector<std::pair<std::wstring, std::string>> outputs_;
    std::vector<EngineKind> engines_;   // Mode combobox entries
};
