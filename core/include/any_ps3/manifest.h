// Any_Ps3 — per-game compile-cache manifest.
// Layout on disk: <cache-folder>/<GameID>/manifest.json
// The manifest records the source signature (sha256), engine version, status
// and per-stage results. Status "outdated" is derived: source hash no longer
// matches. All fields round-trip through JSON (tested).
#pragma once

#include "types.h"
#include "json.h"

namespace any_ps3 {

struct ManifestStage {
    std::string id;      // inspect | analyze | lift | codegen | link
    std::string state;   // done | error | not_implemented | pending
    std::string detail;  // human-readable result / reason
};

struct Manifest {
    int format = 1;
    std::string game_id;
    std::string title;
    std::string source_kind;   // "iso" | "folder"
    std::string source_hash;   // hex sha256 signature of the source
    uint64_t source_size = 0;
    std::string engine_version;
    std::string created_utc;
    std::string updated_utc;
    std::string status;        // imported | analyzed | not_compiled | ready | outdated | error
    std::vector<ManifestStage> stages;

    ManifestStage* stage(const char* id);
    void set_stage(const char* id, const char* state, std::string detail);

    std::string to_json() const;
    static bool from_json(const std::string& text, Manifest& out, std::string& err);
};

// A cached manifest is "outdated" when its recorded source hash no longer
// matches the current source signature.
bool manifest_is_outdated(const Manifest& m, const std::string& current_source_hash);

// Persist helpers. fd variants do NOT close the descriptor.
bool manifest_write_fd(int fd, Manifest& m, std::string& err);
bool manifest_read_fd(int fd, Manifest& out, std::string& err);
bool manifest_write_path(const std::string& path, Manifest& m, std::string& err);
bool manifest_read_path(const std::string& path, Manifest& out, std::string& err);

// ISO-8601 UTC timestamp.
std::string utc_now_iso8601();

} // namespace any_ps3
