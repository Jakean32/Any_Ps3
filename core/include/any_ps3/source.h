// Any_Ps3 — GameSource: uniform access to a game's files with NO copying.
//   IsoFileSource    : PS3 ISO opened through a random-access fd (SAF on Android).
//   FolderSource     : directory dump (PS3_GAME/PARAM.SFO layout) read in place.
// Both compute a deterministic "source signature" (SHA-256) used to detect
// outdated caches. ISO hashing streams the image in 1 MiB chunks (never fully
// in RAM). Folder hashing covers the sorted file list + key file contents.
#pragma once

#include "types.h"
#include "bytes.h"
#include "iso.h"
#include "sfo.h"

#include <functional>
#include <filesystem>

namespace any_ps3 {

struct GameMeta {
    std::string title, title_id, category, app_ver, system_ver;
    bool valid = false;
};

class GameSource {
public:
    virtual ~GameSource() = default;

    // Human description of the source ("ISO via fd", "folder /path").
    virtual std::string describe() const = 0;

    // Read a file relative to the game root (e.g. "PARAM.SFO" resolves to
    // PS3_GAME/PARAM.SFO inside an ISO or folder dump). Bounded by cap.
    virtual bool read_file(const char* rel, std::vector<uint8_t>& out, size_t cap) = 0;

    // Deterministic signature of the source (hex sha256). progress in [0,1].
    virtual std::string source_hash(std::function<void(float)> progress) = 0;
    virtual uint64_t source_size() const = 0;

    // Parse PARAM.SFO (cached after first call; error string if unreadable).
    GameMeta meta(std::string* err);
    const GameMeta& cached_meta() const { return meta_; }

    // Total bytes an ISO occupies (0 for folders).
    virtual uint64_t image_bytes() const { return 0; }

protected:
    GameMeta meta_;
    bool meta_loaded_ = false;
};

// PS3 ISO through a file descriptor. Does NOT take fd ownership.
class IsoFileSource : public GameSource {
public:
    // Opens the ISO. Returns false + error if not a valid ISO9660 image
    // (including the honest "UDF not supported yet" diagnostic).
    static std::unique_ptr<IsoFileSource> open(std::unique_ptr<FdSource> fd, std::string& err);

    std::string describe() const override {
        return "ISO via file descriptor (" + std::to_string(image_bytes() / (1024 * 1024)) + " MiB)";
    }
    bool read_file(const char* rel, std::vector<uint8_t>& out, size_t cap) override;
    std::string source_hash(std::function<void(float)> progress) override;
    uint64_t source_size() const override { return iso_.image_size(); }
    uint64_t image_bytes() const override { return iso_.image_size(); }

    // Copy one embedded file straight to another fd (thumbnail cache) — no RAM blowup.
    bool copy_entry_to_fd(const char* rel, int out_fd);

    const std::string& error() const { return err_; }

private:
    IsoFileSource(std::unique_ptr<FdSource> fd) : iso_(std::move(fd)) {}
    IsoReader iso_;
    std::string err_;
};

// Folder dump: <root>/PS3_GAME/... read in place.
class FolderSource : public GameSource {
public:
    static std::unique_ptr<FolderSource> open(std::string root, std::string& err);

    std::string describe() const override { return "folder " + root_; }
    bool read_file(const char* rel, std::vector<uint8_t>& out, size_t cap) override;
    std::string source_hash(std::function<void(float)> progress) override;
    uint64_t source_size() const override;

private:
    explicit FolderSource(std::string root) : root_(std::move(root)) {}
    std::string root_;
    std::string game_dir_; // root/PS3_GAME or root itself
};

// Where to find game files inside the source.
std::string game_rel_path(const char* rel);

} // namespace any_ps3
