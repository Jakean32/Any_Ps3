#include "anyps3/config.h"

#include <cstdio>
#include <sys/stat.h>

namespace anyps3 {

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static void MkDirP(const std::string& path) {
    std::string cur;
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        cur = path.substr(0, pos);
        ::mkdir(cur.c_str(), 0755);
    }
    ::mkdir(path.c_str(), 0755);
}

bool IniConfig::LoadFromFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    kv_.clear();
    char line[2048];
    while (std::fgets(line, sizeof(line), f)) {
        std::string l = Trim(line);
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(l.substr(0, eq));
        std::string v = Trim(l.substr(eq + 1));
        if (!k.empty()) kv_[k] = v;
    }
    std::fclose(f);
    return true;
}

bool IniConfig::SaveToFile(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    for (const auto& [k, v] : kv_) {
        std::fprintf(f, "%s = %s\n", k.c_str(), v.c_str());
    }
    std::fclose(f);
    return true;
}

std::string IniConfig::Get(const std::string& key, const std::string& fallback) const {
    auto it = kv_.find(key);
    return it == kv_.end() ? fallback : it->second;
}

int IniConfig::GetInt(const std::string& key, int fallback) const {
    auto it = kv_.find(key);
    if (it == kv_.end()) return fallback;
    try {
        return std::stoi(it->second);
    } catch (...) {
        return fallback;
    }
}

void IniConfig::Set(const std::string& key, const std::string& value) { kv_[key] = value; }

void IniConfig::SetInt(const std::string& key, int value) {
    kv_[key] = std::to_string(value);
}

// ---------------------------------------------------------------------------
// GameSettings
// ---------------------------------------------------------------------------

GameSettings GameSettings::Load(const AppConfig& app, const std::string& title_id) {
    GameSettings s;
    // global fallbacks first
    s.resolution_scale = app.global.GetInt("resolution_scale", 1);
    s.frame_limit = app.global.GetInt("frame_limit", 0);
    s.shader_cache_enabled = app.global.GetInt("shader_cache", 1);
    s.ppu_backend = app.global.GetInt("ppu_backend", 0);

    IniConfig ini;
    if (ini.LoadFromFile(app.config_dir + "/games/" + title_id + "/settings.ini")) {
        s.resolution_scale = ini.GetInt("resolution_scale", s.resolution_scale);
        s.frame_limit = ini.GetInt("frame_limit", s.frame_limit);
        s.shader_cache_enabled = ini.GetInt("shader_cache", s.shader_cache_enabled);
        s.ppu_backend = ini.GetInt("ppu_backend", s.ppu_backend);
        for (const auto& [k, v] : ini.all()) {
            if (k.compare(0, 5, "pad_") == 0) {
                s.controller[k.substr(4)] = std::atoi(v.c_str());
            }
        }
    }
    if (s.resolution_scale < 1) s.resolution_scale = 1;
    if (s.resolution_scale > 4) s.resolution_scale = 4;
    return s;
}

void GameSettings::Save(const AppConfig& app, const std::string& title_id) const {
    std::string dir = app.config_dir + "/games/" + title_id;
    MkDirP(dir);
    IniConfig ini;
    ini.SetInt("resolution_scale", resolution_scale);
    ini.SetInt("frame_limit", frame_limit);
    ini.SetInt("shader_cache", shader_cache_enabled);
    ini.SetInt("ppu_backend", ppu_backend);
    for (const auto& [k, v] : controller) {
        ini.SetInt("pad_" + k, v);
    }
    ini.SaveToFile(dir + "/settings.ini");
}

}  // namespace anyps3
