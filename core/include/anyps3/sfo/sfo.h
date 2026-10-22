// Any_Ps3 — PARAM.SFO parser.
// Layout (publicly documented format):
//   header: magic u32 "\0PSF" (0x00505346), version u32 (0x101),
//           key_table u32, data_table u32, entry_count u32
//   entry:  key_off u16, data_fmt u16, data_len u32, data_max u32, data_off u32  (16 bytes)
//   data_fmt: 0x0204 = UTF-8 string, 0x0404 = 32-bit integer
#pragma once

#include "anyps3/types.h"

#include <map>
#include <optional>
#include <string>

namespace anyps3 {

struct SfoValue {
    bool is_int = false;
    std::string str;
    u32 int_value = 0;
};

class ParamSfo {
public:
    bool Parse(const u8* data, size_t size);
    bool Parse(const std::vector<u8>& data) { return Parse(data.data(), data.size()); }

    std::optional<std::string> GetString(const std::string& key) const;
    std::optional<u32> GetInt(const std::string& key) const;

    const std::map<std::string, SfoValue>& entries() const { return entries_; }

    // Serialize (used by tests and by tools that write homebrew PARAM.SFO).
    std::vector<u8> Build() const;

private:
    std::map<std::string, SfoValue> entries_;
};

// Convenience builder used by tests/tools.
class ParamSfoBuilder {
public:
    ParamSfoBuilder& SetString(const std::string& key, const std::string& value) {
        SfoValue v;
        v.is_int = false;
        v.str = value;
        b_[key] = v;
        return *this;
    }
    ParamSfoBuilder& SetInt(const std::string& key, u32 value) {
        SfoValue v;
        v.is_int = true;
        v.int_value = value;
        b_[key] = v;
        return *this;
    }
    std::vector<u8> Build() const;

private:
    std::map<std::string, SfoValue> b_;
};

}  // namespace anyps3
