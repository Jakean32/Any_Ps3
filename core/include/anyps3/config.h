// Any_Ps3 — configuration: global ini + per-game settings + first-run consent flag.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace anyps3 {

// Tiny ini-like store: `key = value` lines, '#' comments. Written for zero deps.
class IniConfig {
public:
    bool LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;

    std::string Get(const std::string& key, const std::string& fallback = "") const;
    int GetInt(const std::string& key, int fallback = 0) const;
    void Set(const std::string& key, const std::string& value);
    void SetInt(const std::string& key, int value);
    bool Has(const std::string& key) const { return kv_.count(key) != 0; }

    const std::map<std::string, std::string>& all() const { return kv_; }

private:
    std::map<std::string, std::string> kv_;
};

// Application configuration rooted at a config directory:
//   <dir>/anyps3.ini                 global settings + first-run consent
//   <dir>/games/<id>/settings.ini    per-game overrides
struct AppConfig {
    std::string config_dir;
    IniConfig global;

    // well-known keys (accessors keep the key names in one place)
    bool FirstRunAccepted() const { return global.GetInt("first_run_accepted", 0) != 0; }
    void AcceptFirstRun() { global.SetInt("first_run_accepted", 1); }

    std::string CacheRoot() const {
        return global.Get("cache_root", config_dir + "/cache");
    }
    void SetCacheRoot(const std::string& v) { global.Set("cache_root", v); }
};

// Per-game settings with safe defaults; every field has a global fallback.
struct GameSettings {
    int resolution_scale = 1;    // 1..4 (render scale multiplier)
    int frame_limit = 0;         // 0 = unlimited, else target fps
    int shader_cache_enabled = 1;
    int ppu_backend = 0;         // 0 = auto (AOT + interpreter fallback), 1 = interpreter
    std::map<std::string, int> controller;  // guest pad button -> host pad button code

    static GameSettings Load(const AppConfig& app, const std::string& title_id);
    void Save(const AppConfig& app, const std::string& title_id) const;
};

}  // namespace anyps3
