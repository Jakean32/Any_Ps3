#include "anyps3/sfo/sfo.h"

#include <cstring>

namespace anyps3 {

static constexpr u32 kSfoMagic = 0x00505346;      // "\0PSF"
static constexpr u16 kFmtUtf8 = 0x0204;
static constexpr u16 kFmtInt32 = 0x0404;

bool ParamSfo::Parse(const u8* data, size_t size) {
    entries_.clear();
    if (size < 20) return false;
    u32 magic = ReadBE32(data);
    if (magic != kSfoMagic) return false;
    u32 key_table = ReadBE32(data + 8);
    u32 data_table = ReadBE32(data + 12);
    u32 entry_count = ReadBE32(data + 16);
    if (key_table > size || data_table > size) return false;

    for (u32 i = 0; i < entry_count; i++) {
        const u8* e = data + 20 + i * 16;
        if (static_cast<size_t>(e + 16 - data) > size) return false;
        u16 key_off = ReadBE16(e);
        u16 fmt = ReadBE16(e + 2);
        u32 len = ReadBE32(e + 4);
        u32 max_len = ReadBE32(e + 8);
        u32 val_off = ReadBE32(e + 12);
        (void)max_len;
        if (key_table + key_off >= size || data_table + val_off >= size) return false;
        std::string key(reinterpret_cast<const char*>(data) + key_table + key_off);
        if (key.empty()) continue;
        SfoValue v;
        v.is_int = (fmt == kFmtInt32);
        const u8* val = data + data_table + val_off;
        size_t avail = size - (data_table + val_off);
        size_t n = len < avail ? len : avail;
        if (v.is_int) {
            v.int_value = n >= 4 ? ReadBE32(val) : 0;
        } else {
            v.str.assign(reinterpret_cast<const char*>(val), n);
            size_t z = v.str.find('\0');
            if (z != std::string::npos) v.str.resize(z);
        }
        entries_[key] = v;
    }
    return true;
}

std::optional<std::string> ParamSfo::GetString(const std::string& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.is_int) return std::nullopt;
    return it->second.str;
}

std::optional<u32> ParamSfo::GetInt(const std::string& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.is_int) return std::nullopt;
    return it->second.int_value;
}

std::vector<u8> ParamSfo::Build() const {
    ParamSfoBuilder b;
    for (const auto& [k, v] : entries_) {
        if (v.is_int) b.SetInt(k, v.int_value);
        else b.SetString(k, v.str);
    }
    return b.Build();
}

std::vector<u8> ParamSfoBuilder::Build() const {
    std::vector<u8> keys_blob;
    std::vector<u8> data_blob;
    struct Slot {
        const std::string* key;
        const SfoValue* val;
        u16 key_off;
        u32 val_off;
    };
    std::vector<Slot> slots;
    // Map iteration is ordered by key (std::map) — deterministic output.
    for (const auto& [k, v] : b_) {
        Slot s;
        s.key = &k;
        s.val = &v;
        s.key_off = static_cast<u16>(keys_blob.size());
        keys_blob.insert(keys_blob.end(), k.begin(), k.end());
        keys_blob.push_back(0);
        // 4-byte align data
        while (data_blob.size() % 4) data_blob.push_back(0);
        s.val_off = static_cast<u32>(data_blob.size());
        if (v.is_int) {
            u8 t[4];
            WriteBE32(t, v.int_value);
            data_blob.insert(data_blob.end(), t, t + 4);
        } else {
            data_blob.insert(data_blob.end(), v.str.begin(), v.str.end());
            data_blob.push_back(0);
        }
        slots.push_back(s);
    }
    const u32 header_size = 20 + static_cast<u32>(slots.size()) * 16;
    const u32 key_table = header_size;
    u32 data_table = key_table + static_cast<u32>(keys_blob.size());
    data_table = (data_table + 3) & ~3u;

    std::vector<u8> out(data_table + data_blob.size(), 0);
    u8* p = out.data();
    WriteBE32(p + 0, kSfoMagic);
    WriteBE32(p + 4, 0x101);
    WriteBE32(p + 8, key_table);
    WriteBE32(p + 12, data_table);
    WriteBE32(p + 16, static_cast<u32>(slots.size()));
    for (size_t i = 0; i < slots.size(); i++) {
        const Slot& s = slots[i];
        u8* e = p + 20 + i * 16;
        WriteBE16(e, s.key_off);
        WriteBE16(e + 2, s.val->is_int ? kFmtInt32 : kFmtUtf8);
        u32 len = s.val->is_int ? 4u : static_cast<u32>(s.val->str.size() + 1);
        u32 max_len = s.val->is_int ? 4u : ((len + 3) & ~3u);
        WriteBE32(e + 4, len);
        WriteBE32(e + 8, max_len);
        WriteBE32(e + 12, s.val_off);
    }
    std::memcpy(p + key_table, keys_blob.data(), keys_blob.size());
    std::memcpy(p + data_table, data_blob.data(), data_blob.size());
    return out;
}

}  // namespace anyps3
