// Any_Ps3 — ISO9660 reader (2048-byte sectors) + minimal writer used by tests.
#pragma once

#include "anyps3/types.h"

#include <map>
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

// Directory record layout (ISO 9660:1988, "CD001"):
//   0: record length  1: ext attr length
//   2..9:   extent LBA (LE u32 @2, BE u32 @6)
//   10..17: data length (LE u32 @10, BE u32 @14)
//   18..24: recording time  25: flags (bit1 = directory)
//   26..29: volume sequence  30: name length  31..: identifier (0=\x00,1=\x01)
class IsoReader {
public:
    bool Open(const std::string& path);
    bool Open(std::vector<u8> image_data);

    std::optional<IsoFileEntry> Find(const std::string& iso_path) const;
    bool ReadFile(const std::string& iso_path, std::vector<u8>* out) const;
    bool ReadSectors(u32 lba, u32 count, u8* out) const;
    bool ListDir(const std::string& iso_path, std::vector<IsoFileEntry>* out) const;

    const std::vector<u8>& data() const { return data_; }

private:
    bool ParsePvd();
    bool WalkDir(u32 dir_lba, u64 dir_size, const std::string& prefix);

    std::vector<u8> data_;
    u32 root_lba_ = 0;
    u64 root_size_ = 0;
    std::map<std::string, IsoFileEntry> entries_;  // "A/B/C" (no leading slash)
};

// Minimal ISO9660 writer — builds synthetic game images for tests (round-trip checks).
// Not a general-purpose ISO tool: no Rock Ridge, no Joliet, single primary volume only.
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
