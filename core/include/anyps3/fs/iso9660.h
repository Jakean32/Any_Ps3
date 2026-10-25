// Any_Ps3 — ISO9660 reader (2048-byte sectors) + minimal writer used by tests.
//
// Backends (transparent to callers):
//   * fd-backed   — path or SAF file descriptor; sector reads are seek+read,
//                   the image is NEVER loaded into RAM (multi-GB safe).
//   * in-memory   — tests round-trip synthetic images.
//
// PS3 disc-encryption layer: on open, the first 4096 bytes are parsed for the
// region table (see any_ps3/disc.h). Encrypted regions are decrypted on the
// fly per sector when a disc key is available (3k3y embedded key, sibling
// .dkey/.key file, or a user-provided key via SetDiscKey*). Without a key,
// encrypted sectors read as (honest) ciphertext and the reader reports its
// state so the import pipeline can flag the game as "Encrypted - disc key
// needed" instead of failing.
#pragma once

#include "anyps3/types.h"
#include "any_ps3/disc.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace anyps3 {

struct IsoFileEntry {
    std::string name;   // as stored (may carry ";1" version suffix)
    u32 lba = 0;        // start sector
    u64 size = 0;       // bytes
    bool is_dir = false;
};

class IsoReader {
public:
    IsoReader() = default;
    ~IsoReader();

    IsoReader(const IsoReader&) = delete;
    IsoReader& operator=(const IsoReader&) = delete;

    // ---- opening ------------------------------------------------------------

    // Open from a host path (Windows/Linux). Picks up a sibling .dkey/.key
    // file automatically when the image carries encrypted regions.
    bool Open(const std::string& path);

    // Open from an in-memory image (tests).
    bool Open(std::vector<u8> image_data);

    // Open from a file descriptor (Android SAF). The fd is dup'ed; the
    // caller keeps ownership of the original. No key auto-pickup (there are
    // no sibling files on SAF trees) — use SetDiscKey*.
    bool OpenFd(int fd, const std::string& display_name);

    // ---- encryption state (valid after any Open call, even a failed one) ----

    const any_ps3::DiscEncryptionInfo& encryption() const { return crypto_.info(); }
    bool HasKey() const { return crypto_.has_key(); }
    bool NeedsKey() const { return crypto_.NeedsKey(); }

    enum class KeySource { None, Embedded3k3y, SiblingFile, User };
    KeySource key_source() const { return key_source_; }
    // Path of the sibling key file that was picked up ("" otherwise).
    const std::string& sibling_key_path() const { return sibling_key_path_; }

    // Attach a disc key (hex text, or raw 16 bytes) and re-walk the tree
    // through the decrypted view. Returns false when the key does not make
    // the filesystem readable (likely wrong key) — key stays attached so
    // callers can report the state honestly.
    bool SetDiscKeyText(const std::string& text);
    bool SetDiscKeyRaw(const uint8_t key[16]);

    // ---- filesystem (as before; reads transparently decrypt) ---------------

    std::optional<IsoFileEntry> Find(const std::string& iso_path) const;
    bool ReadFile(const std::string& iso_path, std::vector<u8>* out) const;
    bool ReadSectors(u32 lba, u32 count, u8* out) const;
    bool ListDir(const std::string& iso_path, std::vector<IsoFileEntry>* out) const;

    // Memory backend only; empty for fd-backed readers.
    const std::vector<u8>& data() const { return mem_data_; }
    u64 image_size() const;
    const std::string& open_error() const { return open_error_; }

private:
    struct Backend {
        virtual ~Backend() = default;
        virtual bool ReadAt(u64 offset, size_t len, void* out) const = 0;
        virtual u64 Size() const = 0;
    };
    struct MemBackend;
    struct FdBackend;

    bool FinishOpen(std::unique_ptr<Backend> backend, bool auto_pickup_key);
    bool ParsePvd();
    bool WalkDir(u32 dir_lba, u64 dir_size, const std::string& prefix);

    std::unique_ptr<Backend> backend_;
    std::string source_path_;       // path (or display name) of the opened image
    bool sibling_pickup_ = false;   // path-opened: look for .dkey/.key next to it
    std::vector<u8> mem_data_;  // only when opened from memory
    any_ps3::DiscCrypto crypto_;
    KeySource key_source_ = KeySource::None;
    std::string sibling_key_path_;
    std::string open_error_;
    u32 root_lba_ = 0;
    u64 root_size_ = 0;
    std::map<std::string, IsoFileEntry> entries_;  // "A/B/C" (no leading slash)
};

// Minimal ISO9660 writer — builds synthetic game images for tests (round-trip
// checks). Not a general-purpose ISO tool: no Rock Ridge, no Joliet, single
// primary volume only.
class IsoWriter {
public:
    void AddFile(const std::string& iso_path, std::vector<u8> bytes);
    void AddDir(const std::string& iso_path);
    std::vector<u8> Build() const;

private:
    struct Node {
        std::string name;  // e.g. "PS3_GAME/PARAM.SFO"
        bool is_dir = false;
        std::vector<u8> bytes;
    };
    std::vector<Node> nodes_;
};

}  // namespace anyps3
