// Persisted user preferences (survive app restarts): %APPDATA%\FrameGen\settings.ini.
#pragma once
#include "Core.h"
#include <map>
#include <algorithm>

class Prefs {
public:
    static Prefs& get();
    std::string str(const std::string& key, const std::string& def = "");
    int integer(const std::string& key, int def = 0);
    double number(const std::string& key, double def = 0);
    bool boolean(const std::string& key, bool def = false);
    void set(const std::string& key, const std::string& v);
    void set(const std::string& key, int v) { set(key, std::to_string(v)); }
    void set(const std::string& key, double v) { set(key, strf("%g", v)); }
    void setBool(const std::string& key, bool v) { set(key, std::string(v ? "1" : "0")); }
    std::wstring path() const { return path_; }

    // Typed accessors with the Mac app's defaults.
    int multiplier() { return integer("multiplier", 2); }            // 1 = frame generation off
    EngineKind engine() { return engineFromId(str("engine", "fast")).value_or(EngineKind::Fast); }
    bool dashboardVisible() { return boolean("dashboardVisible", true); }
    bool miniCounter() { return boolean("miniCounter", true); }
    float volume() { return float(number("volume", 0.8)); }
    bool muted() { return boolean("muted", false); }
    int audioOffsetMs() { return integer("audioOffsetMs", 0); }
    bool hudStabilizer() { return boolean("hudStabilizer", false); }
    int captureRate() { return integer("captureRate", 0); }          // 0 = auto (fastest up to 120 Hz)
    std::string deviceName() { return str("deviceName", ""); }
    int captureWidth() { return integer("captureWidth", 0); }        // 0 × 0 = auto (1920 × 1080)
    int captureHeight() { return integer("captureHeight", 0); }
    std::string scaling() { return str("scaling", "fit"); }          // fit | fill | stretch
    int maxRefresh() { return integer("maxRefresh", 0); }            // 0 = display maximum
    bool startFullScreen() { return boolean("startFullScreen", false); }
    bool autoAdjust() { return boolean("autoAdjust", true); }
    bool displayCapEnabled() { return boolean("displayCapEnabled", true); }
    bool showTips() { return boolean("showTips", true); }
    std::string dashboardCorner() { return str("dashboardCorner", "topLeft"); }
    std::string pacing() { return str("pacing", "auto"); }           // auto | vsync | vrr

private:
    Prefs();
    void save();
    std::mutex m_;
    std::map<std::string, std::string> kv_;
    std::wstring path_;
};
