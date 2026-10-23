#pragma once
// Per-game compile cache: <cache_root>/<GAME_ID>/ with
//   manifest.json   — version, source hash, status, per-stage results
//   logs/           — real compile logs (one file per run)
//   code/           — future AOT artifacts (data files; see exec strategy)
//   thumbs/         — cover thumbnails
//
// Android executable-code strategy (documented, honest):
//   Android shared storage (SAF folders) NEVER allows mapping pages as
//   executable. Any_Ps3 therefore stores compiled artifacts as plain data
//   files under the user-chosen shared folder, and at launch copies them
//   into the app's PRIVATE data directory where execution is permitted
//   (alternative considered: anonymous mmap(PROT_EXEC), rejected because
//   it complicates symbol relocation). The loader for actual AOT artifacts
//   is NOT implemented yet — reported honestly by the UI when reached.
#include <cstdint>
#include <string>
#include <vector>

#include "anyps3.h"
#include "game_import.h"
#include "util.h"

namespace anyps3 {

enum class CacheStatus { NotCompiled, Compiling, Ready, Stale, Failed };

inline const char* cache_status_name(CacheStatus s) {
    switch (s) {
        case CacheStatus::NotCompiled: return "not_compiled";
        case CacheStatus::Compiling: return "compiling";
        case CacheStatus::Ready: return "ready";
        case CacheStatus::Stale: return "stale";
        case CacheStatus::Failed: return "failed";
    }
    return "?";
}
inline CacheStatus cache_status_from(const std::string& s) {
    if (s == "compiling") return CacheStatus::Compiling;
    if (s == "ready") return CacheStatus::Ready;
    if (s == "stale") return CacheStatus::Stale;
    if (s == "failed") return CacheStatus::Failed;
    return CacheStatus::NotCompiled;
}

struct StageInfo {
    std::string name;    // e.g. "analyze", "ir_lift", "codegen", "link"
    std::string status;  // anyps3::stage_status_name()
    std::string note;    // human-readable honest note
};

struct CacheManifest {
    int format_version = kManifestFormatVersion;
    std::string anyps3_version;
    std::string game_id;
    std::string source_hash;  // sha256 over source ref + size + SFO content
    std::string source_ref;
    uint64_t source_size = 0;
    CacheStatus status = CacheStatus::NotCompiled;
    int64_t created_ms = 0;
    int64_t updated_ms = 0;
    std::vector<StageInfo> stages;
    std::string note;
};

class CacheStore {
public:
    explicit CacheStore(std::string root);

    // Create or load the manifest for a game.
    bool ensure(const GameRecord& rec, CacheManifest* out, std::string* err);
    bool load(const std::string& game_id, CacheManifest* out, std::string* err) const;
    bool save(const CacheManifest& m, std::string* err) const;
    bool exists(const std::string& game_id) const;

    std::string game_dir(const std::string& id) const;
    std::string manifest_path(const std::string& id) const;
    std::string logs_dir(const std::string& id) const;
    std::string code_dir(const std::string& id) const;
    const std::string& root() const { return root_; }
    void set_root(const std::string& root) { root_ = root; make_dirs(root_); }

    bool delete_cache(const std::string& game_id, std::string* err);

    // Source identity: hash of (ref + size). Content hashing of multi-GB
    // ISOs is out of scope; size+ref is the honest documented identity.
    static std::string source_hash_of(const GameRecord& rec, const std::string& sfo_hash);
    // Detect staleness: stored hash differs from current record identity.
    bool is_stale(const CacheManifest& m, const GameRecord& rec, const std::string& sfo_hash) const;

    // Format-version compatibility check (future-proofing).
    static bool compatible(const CacheManifest& m, std::string* why_not);

private:
    std::string root_;
};

}  // namespace anyps3
