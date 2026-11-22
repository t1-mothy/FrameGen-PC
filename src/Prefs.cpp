#include "Prefs.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>

Prefs& Prefs::get() { static Prefs p; return p; }

Prefs::Prefs() {
    wchar_t* appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        std::wstring dir = std::wstring(appdata) + L"\\FrameGen";
        CoTaskMemFree(appdata);
        CreateDirectoryW(dir.c_str(), nullptr);
        path_ = dir + L"\\settings.ini";
    }
    // FG_PREFS=<file> uses a separate settings file (tests must not change the user's settings).
    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"FG_PREFS", env, MAX_PATH)) path_ = env;
    std::ifstream f(path_);
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#' || line[0] == ';') continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        kv_[k] = v;
    }
}

void Prefs::save() {
    if (path_.empty()) return;
    std::wstring tmp = path_ + L".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << "# FrameGen settings (delete this file to reset everything)\n";
        for (auto& [k, v] : kv_) f << k << "=" << v << "\n";
    }
    MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string Prefs::str(const std::string& key, const std::string& def) {
    std::lock_guard<std::mutex> l(m_);
    auto it = kv_.find(key);
    return it == kv_.end() ? def : it->second;
}
int Prefs::integer(const std::string& key, int def) { auto s = str(key); return s.empty() ? def : atoi(s.c_str()); }
double Prefs::number(const std::string& key, double def) { auto s = str(key); return s.empty() ? def : atof(s.c_str()); }
bool Prefs::boolean(const std::string& key, bool def) { auto s = str(key); return s.empty() ? def : (s == "1" || s == "true"); }
void Prefs::set(const std::string& key, const std::string& v) {
    std::lock_guard<std::mutex> l(m_);
    if (kv_[key] == v) return;
    kv_[key] = v;
    save();
}
