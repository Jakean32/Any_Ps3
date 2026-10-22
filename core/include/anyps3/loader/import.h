// Any_Ps3 — game import pipeline: detects ISO / folder (JB) / standalone executables,
// extracts PARAM.SFO + ICON0.PNG, and maintains a local library index.
#pragma once

#include "anyps3/types.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace anyps3 {

struct GameInfo {
    std::string title;
    std::string title_id;      // e.g. "BLUS12345", "NPEB00000"
    std::string category;      // GD (disc), HG (hdd game), etc.
    std::string app_ver;       // e.g. "01.00"
    std::string system_ver;    // e.g. "4.91"
    std::string source_path;   // user-provided ISO/folder path
    std::string content_root;  // "PS3_GAME" for disc dumps, "" for PSN-style layouts
    std::vector<u8> icon_png;
};

// Read a file from a game source (folder or ISO image), path relative to content root.
bool ReadGameFile(const std::string& source_path, const std::string& rel_path,
                  std::vector<u8>* out, std::string* err);

// Inspect a candidate (ISO or folder) and fill GameInfo. Does not copy anything.
bool InspectGame(const std::string& source_path, GameInfo* out, std::string* err);

// Library index — small key=value manifests under <config_dir>/games/<TITLE_ID>/
struct LibraryEntry {
    GameInfo info;
    std::string dir;  // library dir for this game
};

class GameLibrary {
public:
    explicit GameLibrary(std::string base_dir);
    bool Import(const std::string& source_path, GameInfo* out, std::string* err);
    bool List(std::vector<LibraryEntry>* out) const;
    bool Remove(const std::string& title_id, std::string* err);
    const std::string& dir() const { return dir_; }

private:
    std::string dir_;
};

}  // namespace anyps3
