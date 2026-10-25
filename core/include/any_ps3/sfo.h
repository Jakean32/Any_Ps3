// Any_Ps3 — PARAM.SFO parser (System File Object, PS3 metadata).
// Format (all little-endian):
//   header: magic "\0PSF", version u32, key_table_off u32, data_table_off u32, entry_count u32
//   entry (16 bytes): key_off u16, data_fmt u16, data_len u32, data_max u32, data_off u32
//   data_fmt: 0x0004 = UTF-8 string, 0x0204 = UTF-8 string (special chars),
//             0x0404 = int32, 0x0203 = utf16? (not used by PS3 game SFOs)
#pragma once

#include "types.h"

namespace any_ps3 {

struct SfoEntry {
    std::string key;
    uint16_t data_fmt = 0;
    uint32_t data_len = 0;
    std::string str;   // valid when data_fmt is a UTF-8 type
    int32_t ivalue = 0; // valid when data_fmt == 0x0404
};

class SfoData {
public:
    std::vector<SfoEntry> entries;

    const SfoEntry* find(const char* key) const;
    std::string str(const char* key, const std::string& def = "") const;
    int32_t integer(const char* key, int32_t def = 0) const;
    bool has(const char* key) const;

    // Convenience game metadata getters.
    std::string title() const { return str("TITLE"); }
    std::string title_id() const { return str("TITLE_ID"); }
    std::string category() const { return str("CATEGORY"); }
    std::string app_ver() const { return str("APP_VER"); }
    std::string system_ver() const { return str("PS3_SYSTEM_VER"); }

    bool empty() const { return entries.empty(); }
};

// Throws any_ps3::Error on malformed data.
SfoData parse_sfo(const uint8_t* data, size_t len);

} // namespace any_ps3
