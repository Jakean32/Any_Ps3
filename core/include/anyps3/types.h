// Any_Ps3 — common types and big-endian helpers.
// The PS3 PPU is big-endian; guest data must always be read through these helpers.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace anyps3 {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

inline u16 ReadBE16(const u8* p) {
    return static_cast<u16>((p[0] << 8) | p[1]);
}
inline u32 ReadBE32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8)  | static_cast<u32>(p[3]);
}
inline u64 ReadBE64(const u8* p) {
    return (static_cast<u64>(ReadBE32(p)) << 32) | ReadBE32(p + 4);
}
inline void WriteBE16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v >> 8); p[1] = static_cast<u8>(v);
}
inline void WriteBE32(u8* p, u32 v) {
    p[0] = static_cast<u8>(v >> 24); p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);  p[3] = static_cast<u8>(v);
}
inline void WriteBE64(u8* p, u64 v) {
    WriteBE32(p, static_cast<u32>(v >> 32)); WriteBE32(p + 4, static_cast<u32>(v));
}

// Little-endian helpers (SPU local store is little-endian).
inline u16 ReadLE16(const u8* p) {
    u16 v; std::memcpy(&v, p, 2); return v;
}
inline u32 ReadLE32(const u8* p) {
    u32 v; std::memcpy(&v, p, 4); return v;
}
inline u64 ReadLE64(const u8* p) {
    u64 v; std::memcpy(&v, p, 8); return v;
}
inline void WriteLE16(u8* p, u16 v) { std::memcpy(p, &v, 2); }
inline void WriteLE32(u8* p, u32 v) { std::memcpy(p, &v, 4); }
inline void WriteLE64(u8* p, u64 v) { std::memcpy(p, &v, 8); }

// 64-bit FNV-1a hash — used for AOT cache manifests and shader cache keys.
inline u64 Fnv1a64(const void* data, size_t len, u64 seed = 0xcbf29ce484222325ULL) {
    const u8* p = static_cast<const u8*>(data);
    u64 h = seed;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

std::string ToHex(const void* data, size_t len);

}  // namespace anyps3
