// Any_Ps3 — ISO9660 (+ Joliet SVD) reader over a random-access FdSource.
//
// This is how Any_Ps3 reads PARAM.SFO / ICON0.PNG / EBOOT.BIN directly out of
// an ISO WITHOUT copying the image into app storage and WITHOUT loading it
// into RAM: every access is a bounded seek+read through the file descriptor.
//
// Honest scope: ISO9660 + Joliet supported. UDF-only images (many retail PS3
// disc dumps) are DETECTED and reported as "not supported yet".
#pragma once

#include "types.h"
#include "bytes.h"

namespace any_ps3 {

struct IsoDirEntry {
    std::string name;   // decoded UTF-8, ";" version suffix stripped
    uint32_t lba = 0;
    uint64_t size = 0;
    bool is_dir = false;
};

class IsoReader {
public:
    explicit IsoReader(std::unique_ptr<FdSource> src) : src_(std::move(src)) {}

    // Opens and locates the volume descriptor chain. Returns false on failure
    // (error() explains — including the UDF detection case).
    bool open();

    // List a directory. path "" = root; "PS3_GAME"; segments separated by '/'.
    // Comparison is case-insensitive. Returns false if the path is not found.
    bool list(const std::string& path, std::vector<IsoDirEntry>& out);

    // Find a single entry (file or dir). Returns false if not found.
    bool stat(const std::string& path, IsoDirEntry& out);

    // Read a file (whole, capped) or a byte range of a file. Range read is the
    // primitive used for random access (cover art, SFO, ELF headers).
    bool read_file(const std::string& path, std::vector<uint8_t>& out, size_t cap);
    bool read_file_range(const std::string& path, uint64_t offset, uint64_t len,
                         std::vector<uint8_t>& out);

    uint64_t image_size() const { return src_ ? src_->size() : 0; }
    const std::string& error() const { return error_; }

    // Raw byte read at an absolute image offset (used for whole-image hashing).
    size_t raw_read(uint64_t offset, void* buf, size_t len) {
        return src_ ? src_->read_at(offset, buf, len) : 0;
    }

    // Random-access read within an already stat'ed entry (used for streaming
    // an embedded file to another fd without loading it into RAM).
    bool read_entry_range(const IsoDirEntry& e, uint64_t offset, uint64_t len, void* out);

    static constexpr size_t kSector = 2048;

private:
    bool read_sector(uint32_t lba, uint8_t* buf);
    bool parse_dir_extent(uint32_t lba, uint64_t size, bool ucs2,
                          std::vector<IsoDirEntry>& out);
    bool walk(const std::string& path, IsoDirEntry& out);

    std::unique_ptr<FdSource> src_;
    // root directory records: [0] = primary (PVD), [1] = Joliet (SVD) if present
    IsoDirEntry root_[2];
    bool joliet_ = false;
    std::string error_;
};

} // namespace any_ps3
