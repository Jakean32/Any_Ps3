#pragma once
// ISO9660 reader over an abstract ByteSource — the core of no-copy import.
// PS3 disc images are plain ISO9660 with uppercase names and possible ";1"
// version suffixes. Directory structures are small, so we build an index of
// the full directory tree at open time (cheap: only directory sectors are
// read). File payloads are NEVER loaded wholesale except when explicitly
// requested (PARAM.SFO / icons are tiny). Random-access extents are exposed
// for large files.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "bytesource.h"

namespace anyps3 {

struct IsoExtent {
    uint64_t lba = 0;      // logical block address (2048-byte sectors)
    uint64_t size = 0;
    bool valid = false;
};

class Iso9660 {
public:
    // Validates the Primary Volume Descriptor and indexes the tree.
    static std::shared_ptr<Iso9660> open(std::shared_ptr<ByteSource> src,
                                         std::string* err);

    // Case-insensitive lookup with optional ";1" version suffix.
    // Paths like "PS3_GAME/PARAM.SFO" or "/PS3_GAME/PARAM.SFO".
    IsoExtent lookup(const std::string& path) const;
    bool file_exists(const std::string& path) const;
    uint64_t file_size(const std::string& path) const;

    // Reads a whole (small) file into memory. Refuses files > max_bytes to
    // guard against accidental full-ISO loads (returns false).
    bool read_file(const std::string& path, std::vector<uint8_t>* out,
                   size_t max_bytes = 32 * 1024 * 1024, std::string* err = nullptr) const;

    // Directory listing (file+dir names, without version suffix).
    std::vector<std::string> list_dir(const std::string& path) const;

    const std::string& volume_id() const { return volume_id_; }
    uint64_t total_size() const { return src_ ? src_->size() : 0; }
    ByteSource* source() const { return src_.get(); }

    struct Entry {
        std::string name_l;  // lowercased, suffix-stripped
        uint64_t lba = 0;
        uint64_t size = 0;
        bool is_dir = false;
        std::map<std::string, Entry> children;  // for dirs
    };

private:
    bool index_dir(const Entry& dir, std::string* err, int depth);
    bool read_extent(const IsoExtent& e, uint64_t off, uint32_t len, void* dst) const;

    std::shared_ptr<ByteSource> src_;
    Entry root_;
    std::string volume_id_;
};

}  // namespace anyps3
