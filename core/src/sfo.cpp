#include "any_ps3/sfo.h"
#include "any_ps3/bytes.h"

namespace any_ps3 {

static bool is_string_fmt(uint16_t fmt) { return fmt == 0x0004 || fmt == 0x0204; }

const SfoEntry* SfoData::find(const char* key) const {
    for (const auto& e : entries)
        if (e.key == key) return &e;
    return nullptr;
}
std::string SfoData::str(const char* key, const std::string& def) const {
    const SfoEntry* e = find(key);
    return (e && is_string_fmt(e->data_fmt) && !e->str.empty()) ? e->str : def;
}
int32_t SfoData::integer(const char* key, int32_t def) const {
    const SfoEntry* e = find(key);
    return (e && e->data_fmt == 0x0404) ? e->ivalue : def;
}
bool SfoData::has(const char* key) const { return find(key) != nullptr; }

SfoData parse_sfo(const uint8_t* data, size_t len) {
    ByteReader r(data, len);
    const uint8_t* magic = r.take(4);
    if (magic[0] != 0x00 || magic[1] != 'P' || magic[2] != 'S' || magic[3] != 'F')
        throw Error("not a PARAM.SFO (bad magic)");
    /*uint32_t version =*/ r.u32le();
    uint32_t key_off = r.u32le();
    uint32_t data_off = r.u32le();
    uint32_t count = r.u32le();
    if (count > 4096) throw Error("PARAM.SFO: implausible entry count");
    if (key_off > len || data_off > len) throw Error("PARAM.SFO: table offsets out of bounds");

    SfoData out;
    out.entries.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t key_rel = r.u16le();
        uint16_t fmt = r.u16le();
        uint32_t data_len = r.u32le();
        uint32_t data_max = r.u32le();
        uint32_t data_rel = r.u32le();
        (void)data_max;

        if (key_off + key_rel >= len) throw Error("PARAM.SFO: key offset out of bounds");
        // key is a NUL-terminated string inside the key table
        const uint8_t* kp = data + key_off + key_rel;
        size_t klen = 0;
        while (key_off + key_rel + klen < len && kp[klen] != 0) klen++;
        if (key_off + key_rel + klen >= len) throw Error("PARAM.SFO: unterminated key");

        if (data_off + data_rel > len) throw Error("PARAM.SFO: data offset out of bounds");
        if (data_len > len - (data_off + data_rel))
            data_len = (uint32_t)(len - (data_off + data_rel));

        SfoEntry e;
        e.key.assign((const char*)kp, klen);
        e.data_fmt = fmt;
        e.data_len = data_len;
        const uint8_t* dp = data + data_off + data_rel;
        if (is_string_fmt(fmt)) {
            // UTF-8 (possibly special-char mapped); trim at first NUL.
            size_t slen = 0;
            while (slen < data_len && dp[slen] != 0) slen++;
            e.str.assign((const char*)dp, slen);
        } else if (fmt == 0x0404) {
            e.ivalue = data_len >= 4 ? (int32_t)rd32le(dp) : 0;
        } else {
            // Unknown format: keep raw bytes in str so callers can still inspect.
            e.str.assign((const char*)dp, data_len);
        }
        out.entries.push_back(std::move(e));
    }
    return out;
}

} // namespace any_ps3
