#include "sfo.h"

#include <cstring>

namespace anyps3 {

static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool Sfo::parse(const uint8_t* data, size_t size, Sfo* out, std::string* err) {
    if (size < 20) {
        if (err) *err = "PARAM.SFO too small";
        return false;
    }
    if (memcmp(data, "\0PSF", 4) != 0) {
        if (err) *err = "PARAM.SF0 bad magic (expected \\0PSF)";
        return false;
    }
    uint32_t key_off = rd32(data + 8);
    uint32_t data_off = rd32(data + 12);
    uint32_t entry_count = rd32(data + 16);
    if (entry_count > 4096 || key_off >= size || data_off > size ||
        20 + (uint64_t)entry_count * 16 > size) {
        if (err) *err = "PARAM.SFO header out of range";
        return false;
    }
    out->fields.clear();
    out->fields.reserve(entry_count);
    for (uint32_t i = 0; i < entry_count; i++) {
        const uint8_t* e = data + 20 + (size_t)i * 16;
        uint16_t key_pos = rd16(e + 0);
        uint16_t fmt = rd16(e + 2);
        uint32_t len = rd32(e + 4);
        uint32_t max_len = rd32(e + 8);
        uint32_t val_pos = rd32(e + 12);
        (void)max_len;
        if ((uint64_t)key_off + key_pos >= size || (uint64_t)data_off + val_pos >= size) {
            if (err) *err = "PARAM.SFO entry offset out of range";
            return false;
        }
        // key: NUL-terminated in key table
        const uint8_t* kp = data + key_off + key_pos;
        std::string key;
        while (kp < data + size && *kp) key += char(*kp++);
        SfoField f;
        f.key = key;
        f.fmt = fmt;
        const uint8_t* vp = data + data_off + val_pos;
        size_t avail = size - (data_off + val_pos);
        if (fmt == 0x0204) {  // uint32 LE
            f.len = 4;
            if (avail < 4) {
                if (err) *err = "PARAM.SFO int field out of range";
                return false;
            }
            f.int_val = rd32(vp);
        } else if (fmt == 0x0004) {  // UTF-8 special: u32 LE real length prefix
            if (avail < 4) {
                if (err) *err = "PARAM.SFO string field out of range";
                return false;
            }
            uint32_t real_len = rd32(vp);
            f.len = real_len;
            if ((uint64_t)4 + real_len > avail) real_len = (uint32_t)avail - 4;
            f.str_val.assign((const char*)vp + 4, real_len);
        } else if (fmt == 0x0200) {  // plain UTF-8
            f.len = len;
            uint32_t take = len < avail ? len : (uint32_t)avail;
            f.str_val.assign((const char*)vp, take);
        } else {  // unknown format: keep metadata, skip value
            f.len = len;
        }
        out->fields.push_back(std::move(f));
    }
    return true;
}

static std::string find_str(const Sfo& s, const char* key) {
    for (const auto& f : s.fields)
        if (f.key == key && (f.fmt == 0x0004 || f.fmt == 0x0200)) return f.str_val;
    return "";
}
static uint32_t find_int(const Sfo& s, const char* key) {
    for (const auto& f : s.fields)
        if (f.key == key && f.fmt == 0x0204) return f.int_val;
    return 0;
}

std::string Sfo::title() const { return find_str(*this, "TITLE"); }
std::string Sfo::title_id() const { return find_str(*this, "TITLE_ID"); }
std::string Sfo::app_ver() const { return find_str(*this, "APP_VER"); }
std::string Sfo::category() const { return find_str(*this, "CATEGORY"); }
std::string Sfo::ps3_system_ver() const { return find_str(*this, "PS3_SYSTEM_VER"); }
std::string Sfo::disc_number() const { return find_str(*this, "DISC_NUMBER"); }
uint32_t Sfo::parental_level() const { return find_int(*this, "PARENTAL_LEVEL"); }

}  // namespace anyps3
