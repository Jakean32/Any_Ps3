#pragma once
// No-copy game import. A GameRecord stores ONLY a reference (filesystem
// path or SAF content URI) plus small metadata; the game itself is never
// copied into app storage. Icons are extracted once and cached as small
// thumbnails. Missing sources are detected and flagged, never auto-deleted.
#include <cstdint>
#include <string>
#include <vector>

#include "anyps3.h"
#include "bytesource.h"
#include "iso9660.h"
#include "sfo.h"

namespace anyps3 {

enum class SourceType { Folder, IsoFile, SafUri };

struct GameRecord {
    std::string title;      // from PARAM.SFO TITLE
    std::string title_id;   // e.g. NPEA00001
    std::string app_ver;
    std::string category;
    SourceType src_type = SourceType::Folder;
    std::string src_ref;    // absolute path or content:// URI
    uint64_t src_size = 0;
    int64_t added_ms = 0;
    int64_t last_seen_ms = 0;
    bool missing = false;   // source file/folder no longer accessible
    std::string icon_path;  // cached thumbnail (app metadata dir)
    std::string pic1_path;  // cached background thumbnail
};

struct ScanStats {
    int found = 0;      // valid game sources discovered
    int imported = 0;   // newly imported
    int updated = 0;    // already present, refreshed
    int skipped = 0;    // not a PS3 game / unreadable
    std::vector<std::string> log;
};

class GameImport {
public:
    // meta_dir holds library.json + thumbs/. Created if missing.
    GameImport(std::string meta_dir);

    // Scan a folder for PS3 games (folder games with PS3_GAME/ or direct
    // PARAM.SFO, plus *.iso images). `recursive` walks one extra level.
    ScanStats scan_folder(const std::string& path, bool recursive, std::string* err);

    // Import a single ISO file by path or by an already-opened source
    // (Android SAF fd path). `ref` is stored verbatim (path or URI).
    bool import_iso(const std::string& ref, ByteSource* src, std::string* err);

    // Import a folder game (dir containing PS3_GAME/PARAM.SFO or PARAM.SFO).
    bool import_folder_game(const std::string& dir, std::string* err);

    // Import from pre-read parts (SAF folder games: the platform opened
    // PARAM.SFO / ICON0.PNG / PIC1.PNG descriptors individually). Still no
    // copy: only small thumbnails are written to the app's metadata dir.
    bool import_from_parts(const std::string& src_ref, SourceType type,
                           const std::vector<uint8_t>& sfo_data,
                           const std::vector<uint8_t>* icon,
                           const std::vector<uint8_t>* pic1, std::string* err);

    // Mark a SAF-URI record present/missing after the platform re-opened it.
    bool set_saf_present(const std::string& uri, bool present, uint64_t size);

    // Re-check a record: presence of source, re-extract thumbs if missing.
    bool refresh(GameRecord* rec, std::string* err);

    // Refresh all records; updates `missing` flags. Returns # still present.
    int refresh_all(std::string* err);

    // Remove record (and optionally its compile cache, handled by Library).
    bool remove_record(const std::string& title_id, std::string* err);

    // Persistence: library.json in meta_dir.
    bool load(std::string* err);
    bool save(std::string* err) const;

    const std::vector<GameRecord>& games() const { return games_; }
    GameRecord* find(const std::string& title_id);
    const GameRecord* find(const std::string& title_id) const;
    std::vector<std::string> pending_scan_logs() const { return last_scan_log_; }

private:
    bool import_sfo_based(const std::string& ref, SourceType type, ByteSource* src,
                          const Sfo& sfo, const std::string& icon_rel, const std::string& pic1_rel,
                          Iso9660* iso, const std::string& folder_dir, std::string* err);
    std::string meta_dir_;
    std::string lib_path_;
    std::string thumbs_dir_;
    std::vector<GameRecord> games_;
    std::vector<std::string> last_scan_log_;
};

// Utility: does `dir` look like a PS3 folder game?
bool looks_like_folder_game(const std::string& dir);
// Try to open the PARAM.SFO from a folder game; returns false if absent.
bool read_folder_sfo(const std::string& dir, std::vector<uint8_t>* out, std::string* ps3_game_dir);

}  // namespace anyps3
