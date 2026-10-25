#include "anyps3/sfo/sfo.h"

#include <cstring>

namespace anyps3 {

static constexpr u32 kSfoMagic = 0x00505346;      // "\0PSF"
// Entry data formats seen in real PARAM.SFO files:
//   0x0004 UTF-8 string (special-char mapping, PS1/PS2-era)
//   0x0104 UTF-8 string (plain)
//   0x0204 UTF-8 string (PS3/PSP standard)
//   0x0404 32-bit integer (big-endian)
static constexpr u16 kFmtUtf8 = 0x0204;
static constexpr u16 kFmtInt32 = 0x0404;

static bool IsStringFmt(u16 fmt) {
    return fmt == 0x0004 || fmt == 0x0104 || fmt == 0x0204;
}

bool ParamSfo::Parse(const u8* data, size_t size) {
    entries_.clear();
    if (size < 20) return false;
    u32 magic = ReadBE32(data);
    if (magic != kSfoMagic) return false;
    /*u32 version = ReadBE32(data + 4);*/  // tolerated: 0x0004 .. 0x1104 seen in the wild
    u32 key_table = ReadBE32(data + 8);
    u32 data_table = ReadBE32(data + 12);
    u32 entry_count = ReadBE32(data + 16);
    if (entry_count > 4096) return false;
    const u32 header_end = 20 + entry_count * 16;
    if (header_end > size) {
        // entry table truncated: parse what fits (lenient)
        entry_count = static_cast<u32>((size - 20) / 16);
    }

    // Two interpretations exist in the wild:
    //   absolute: table offsets measured from the file start (all modern files)
    //   relative: measured from the end of the entry table (ancient writers,
    //             hinted by tiny offset values)
    struct Cand {
        u32 key_base, data_base;
        const char* name;
    };
    Cand cands[2] = {{key_table, data_table, "absolute"},
                     {header_end + key_table, header_end + data_table, "relative"}};

    ParamSfo best;
    size_t best_count = 0;
    bool perfect = false;
    for (int attempt = 0; attempt < 2 && !perfect; attempt++) {
        u32 key_base = cands[attempt].key_base;
        u32 data_base = cands[attempt].data_base;
        if (attempt == 1 && (key_table >= header_end || data_table >= header_end ||
                             key_table == cands[0].key_base)) {
            // Relative only makes sense when the stored values are small
            // (i.e. plausibly relative to the header end).
            if (!(key_table < header_end && data_table < header_end)) continue;
        }
        if (key_base > size || data_base > size) continue;

        ParamSfo tmp;
        size_t good = 0;
        for (u32 i = 0; i < entry_count; i++) {
            const u8* e = data + 20 + i * 16;
            if (static_cast<size_t>(e + 16 - data) > size) break;
            u16 key_off = ReadBE16(e);
            u16 fmt = ReadBE16(e + 2);
            u32 len = ReadBE32(e + 4);
            /*u32 max_len = ReadBE32(e + 8);*/  // capacity; entries are padded to it
            u32 val_off = ReadBE32(e + 12);

            if (static_cast<u64>(key_base) + key_off >= size ||
                static_cast<u64>(data_base) + val_off > size) {
                continue;  // lenient: skip this entry, keep parsing
            }
            const u8* kp = data + key_base + key_off;
            size_t klen = 0;
            while (key_base + key_off + klen < size && kp[klen] != 0) klen++;
            std::string key(reinterpret_cast<const char*>(kp), klen);
            if (key.empty()) continue;

            const u8* val = data + data_base + val_off;
            size_t avail = size - (data_base + val_off);
            size_t n = len < avail ? len : avail;
            SfoValue v;
            if (fmt == kFmtInt32) {
                v.is_int = true;
                v.int_value = n >= 4 ? ReadBE32(val) : 0;
            } else if (IsStringFmt(fmt)) {
                v.is_int = false;
                v.str.assign(reinterpret_cast<const char*>(val), n);
                size_t z = v.str.find('\0');
                if (z != std::string::npos) v.str.resize(z);
            } else {
                // Unknown format: keep raw bytes so callers can still inspect.
                v.is_int = false;
                v.str.assign(reinterpret_cast<const char*>(val), n);
            }
            tmp.entries_[key] = v;
            good++;
        }
        if (good > best_count) {
            best_count = good;
            best = std::move(tmp);
        }
        if (good == entry_count && entry_count > 0) perfect = true;
    }
    if (best_count == 0) return false;
    entries_ = std::move(best.entries_);
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
