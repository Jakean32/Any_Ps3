// Any_Ps3 — game import pipeline: detects ISO / folder (JB) / standalone executables,
// extracts PARAM.SFO + ICON0.PNG, and maintains a local library index.
//
// ISO import is NO-COPY by design: the image is opened through a file
// descriptor and read with random access (works with Android SAF fds); the
// image is never duplicated into app storage and never fully loaded into RAM.
// Encrypted (Redump/3k3y-style) ISOs: the region table at sector 0 is parsed,
// metadata that sits in encrypted regions is decrypted on the fly when the
// user supplies a disc key (.dkey/.key file or per-game settings); without a
// key the game imports with state "Encrypted - disc key needed" instead of
// failing, and metadata falls back to PS3_DISC.SFB or the file name.
#pragma once

#include "anyps3/types.h"
#include "anyps3/config.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace anyps3 {

// Encrypted-image state stored per game.
enum class EncState : int {
    None = 0,        // plain ISO (no encryption layer) or folder dump
    NeedsKey = 1,    // encryption layer present, no usable disc key yet
    KeyInUse = 2,    // encryption layer present, decryption active
};

struct GameInfo {
    std::string title;
    std::string title_id;      // e.g. "BLUS12345", "NPEB00000"
    std::string category;      // GD (disc), HG (hdd game), etc.
    std::string app_ver;       // e.g. "01.00"
    std::string system_ver;    // e.g. "4.91"
    std::string source_path;   // user-provided ISO/folder path
    std::string source_uri;    // Android SAF URI ("" on path-backed hosts)
    std::string content_root;  // "PS3_GAME" for disc dumps, "" for PSN-style layouts
    std::vector<u8> icon_png;

    // --- encrypted-image bookkeeping (TASK: encrypted ISO support) ---
    EncState encrypted = EncState::None;
    std::string dkey_path;     // resolved key file (path), "3k3y-embedded", or ""
    std::string meta_source;   // "PARAM.SFO" / "PS3_DISC.SFB" / "filename"
    std::string diag;          // one-line import diagnostics (logged too)
};

// Read a file from a game source (folder or ISO image), path relative to content root.
// `key_text` (optional): disc key (hex) to use for encrypted images.
bool ReadGameFile(const std::string& source_path, const std::string& rel_path,
                  std::vector<u8>* out, std::string* err,
                  const std::string* key_text = nullptr);

// fd-based variants (Android SAF; the fd stays owned by the caller and only
// needs to be valid for the duration of the call).
bool ReadGameFileFd(int fd, const std::string& rel_path, std::vector<u8>* out,
                    std::string* err, const std::string* key_text = nullptr);

// Inspect a candidate (ISO or folder) and fill GameInfo. Does not copy anything.
// Metadata never fails the import: PARAM.SFO -> PS3_DISC.SFB -> filename.
//
// key_text (optional): an explicit user-supplied disc key (hex text or raw
// 16 bytes). When non-empty it is applied to the image and — if it does not
// unlock an encrypted image — the inspect FAILS with an honest error
// instead of importing a "NeedsKey" placeholder. Used by the per-game
// settings "attach key and re-read" flow.
bool InspectGame(const std::string& source_path, GameInfo* out, std::string* err,
                 const std::string& key_text = "");

// fd-based inspect (Android SAF); display_name feeds the filename fallback.
bool InspectGameFd(int fd, const std::string& display_name, GameInfo* out,
                   std::string* err, const std::string& key_text = "");

// Read a user disc-key file (.dkey / .key) and return the key as canonical
// 32-hex text. Accepts 32/64-hex text files, colon/0x/spaces forms and raw
// 16-byte binary keys. Returns "" and sets *err when the content is not a
// usable key. (Any_Ps3 ships no keys; this only reads files the user picks.)
std::string ReadDiscKeyFileAsText(const std::string& path, std::string* err);

// Effective disc key for a game from its settings: a typed 32-hex key wins,
// else the referenced key file is read. "" when neither is available/valid.
std::string EffectiveDiscKeyText(const GameSettings& s, std::string* err);

// Library index — small key=value manifests under <config_dir>/games/<TITLE_ID>/
struct LibraryEntry {
    GameInfo info;
    std::string dir;  // library dir for this game
};

class GameLibrary {
public:
    explicit GameLibrary(std::string base_dir);
    bool Import(const std::string& source_path, GameInfo* out, std::string* err);
    // Store a GameInfo that was produced by InspectGame/InspectGameFd
    // (used by the no-copy fd import path on Android).
    bool ImportInfo(const GameInfo& info, std::string* err);
    bool List(std::vector<LibraryEntry>* out) const;
    bool Remove(const std::string& title_id, std::string* err);
    const std::string& dir() const { return dir_; }

private:
    std::string dir_;
};

// Re-import `info` (metadata re-read through a newly attached disc key) and
// move the per-game settings + stored key file from the old library slot when
// the title id changed (UNKNOWN -> real TITLE_ID once the SFO is readable).
// The old library entry is removed. Returns false + *err on failure; the
// library keeps its previous state in that case.
bool ReplaceLibraryEntry(const AppConfig& app, GameLibrary* lib, const GameInfo& info,
                         const std::string& old_id, std::string* err);

}  // namespace anyps3
