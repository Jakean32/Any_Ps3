#pragma once
// PARAM.SFO parser (PS3 game metadata).
// Format: header "\0PSF", little-endian fields, key table + data table.
// Data formats handled: 0x0004 UTF-8 special (4-byte length prefix),
// 0x0200 plain UTF-8, 0x0204 little-endian uint32.
#include <cstdint>
#include <string>
#include <vector>

namespace anyps3 {

struct SfoField {
    std::string key;
    uint16_t fmt = 0;
    uint32_t len = 0;      // actual data length
    std::string str_val;   // for string fields
    uint32_t int_val = 0;  // for 0x0204
};

class Sfo {
public:
    // Parse a PARAM.SFO buffer. Returns false with `err` on invalid data.
    static bool parse(const uint8_t* data, size_t size, Sfo* out, std::string* err = nullptr);

    // Convenience accessors ("" / 0 when absent).
    std::string title() const;      // TITLE
    std::string title_id() const;   // TITLE_ID (e.g. BLUS12345)
    std::string app_ver() const;    // APP_VER
    std::string category() const;   // CATEGORY (GD = disc game, HG = HDD game)
    std::string ps3_system_ver() const;
    std::string disc_number() const;  // DISC_NUMBER
    uint32_t parental_level() const;

    std::vector<SfoField> fields;
};

}  // namespace anyps3
