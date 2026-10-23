#pragma once
// Library facade tying together import, cache store and compile pipeline.
// This is the single API both frontends (Android JNI, Windows ImGui) use.
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cache_manifest.h"
#include "game_import.h"
#include "recompiler.h"
#include "util.h"

namespace anyps3 {

struct AppConfig {
    std::string meta_dir;    // app-owned metadata (library.json, thumbs, config)
    std::string cache_root;  // user-chosen compile/cache folder
    bool wizard_done = false;
    std::string legal_version_accepted;  // "" = not accepted
    // Frontend extras (persisted verbatim): cache_tree_uri, cache_raw_path,
    // cache_is_direct, ...
    std::map<std::string, std::string> extra;
    bool load(const std::string& dir);
    bool save() const;
    std::string get(const std::string& key, const std::string& def = "") const;
    bool set(const std::string& key, const std::string& value);
    std::string path() const { return cfg_path_; }

private:
    std::string cfg_path_;
};

struct LibraryEntry {
    GameRecord rec;
    bool has_manifest = false;
    CacheManifest manifest;
};

struct PlatformLogSink {
    // Frontends register this to mirror pipeline progress into UI logs.
    std::function<void(const std::string& line)> fn;
};

class Library {
public:
    Library(AppConfig cfg);

    GameImport& importer() { return import_; }
    CacheStore& cache() { return cache_; }

    // Aggregated view for the UI.
    std::vector<LibraryEntry> entries(std::string* err = nullptr);

    // Refresh presence + staleness flags. Fills UI log lines.
    bool refresh(std::vector<std::string>* log_lines, std::string* err);

    // Import operations (no-copy).
    ScanStats scan_folder(const std::string& path, bool recursive, std::string* err) {
        return import_.scan_folder(path, recursive, err);
    }
    bool import_iso_path(const std::string& path, std::string* err);
    bool import_folder(const std::string& dir, std::string* err);
    // Android SAF: URI + an open fd for random access; fd is consumed
    // (closed) only when owned; ref is the persisted URI.
    bool import_saf_uri(const std::string& uri, int fd, bool owns_fd, std::string* err);
    // Re-open check for SAF URIs: opens fd via platform (Java passes fd).
    bool check_saf_uri_present(const std::string& uri, int fd, std::string* err);
    // SAF folder game: platform opened PARAM.SFO / ICON0 / PIC1 fds separately;
    // core does the metadata work from the buffers (still no copy).
    bool import_saf_folder(const std::string& game_dir_uri, const std::string& sfo_uri,
                           int sfo_fd, int icon_fd, int pic1_fd, std::string* err);

    // Cache operations.
    bool compile(const std::string& title_id, const ProgressFn& progress, std::string* err);
    bool recompile(const std::string& title_id, const ProgressFn& progress, std::string* err);
    bool delete_cache(const std::string& title_id, std::string* err);
    bool remove_game(const std::string& title_id, bool also_delete_cache, std::string* err);

    // Status helpers for UI.
    CacheStatus status_of(const std::string& title_id, std::string* note);
    static std::string status_ui_name(CacheStatus s);
    uint64_t cache_free_bytes() { return free_space_bytes(cfg_.cache_root); }

    const AppConfig& config() const { return cfg_; }
    // Change cache root at runtime (persists config, re-points the store).
    bool set_cache_root(const std::string& root) {
        cfg_.cache_root = root;
        bool ok = cfg_.save();
        cache_.set_root(root);
        log_line("cache root set: " + root);
        return ok;
    }
    void set_sink(PlatformLogSink s) { sink_ = std::move(s); }
    void log_line(const std::string& line) {
        if (sink_.fn) sink_.fn(line);
        ui_log_.push_back(line);
        if (ui_log_.size() > 2000) ui_log_.erase(ui_log_.begin());
    }
    const std::vector<std::string>& ui_log() const { return ui_log_; }

    // Minimum recommended free space for a compile cache (documentation).
    static constexpr uint64_t kMinRecommendedFree = 2ULL * 1024 * 1024 * 1024;  // 2 GiB

private:
    bool resolve_sfo_hash(const GameRecord& rec, std::string* sfo_hash);
    AppConfig cfg_;
    GameImport import_;
    CacheStore cache_;
    PlatformLogSink sink_;
    std::vector<std::string> ui_log_;
};

}  // namespace anyps3
