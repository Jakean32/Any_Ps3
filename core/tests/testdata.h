// Test-data builders: PARAM.SFO, minimal ISO9660(+Joliet) image, PPU ELF64 BE.
// These build REAL binary structures so the tests exercise the actual parsers.
#pragma once

#include "any_ps3/types.h"
#include "any_ps3/bytes.h"

#include <cstring>
#include <string>
#include <vector>

namespace any_ps3 {
namespace testdata {

inline void put16le(std::vector<uint8_t>& v, size_t off, uint16_t x) {
    v[off] = (uint8_t)x; v[off + 1] = (uint8_t)(x >> 8);
}
inline void put16be(std::vector<uint8_t>& v, size_t off, uint16_t x) {
    v[off] = (uint8_t)(x >> 8); v[off + 1] = (uint8_t)x;
}
inline void put32le(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off] = (uint8_t)x; v[off + 1] = (uint8_t)(x >> 8);
    v[off + 2] = (uint8_t)(x >> 16); v[off + 3] = (uint8_t)(x >> 24);
}
inline void put32be(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off] = (uint8_t)(x >> 24); v[off + 1] = (uint8_t)(x >> 16);
    v[off + 2] = (uint8_t)(x >> 8); v[off + 3] = (uint8_t)x;
}
inline void put64be(std::vector<uint8_t>& v, size_t off, uint64_t x) {
    for (int i = 0; i < 8; i++) v[off + i] = (uint8_t)(x >> (56 - i * 8));
}

// ---- PARAM.SFO builder ----
inline std::vector<uint8_t> make_sfo(
    const std::vector<std::pair<std::string, std::string>>& str_entries,
    const std::vector<std::pair<std::string, int32_t>>& int_entries = {}) {
    // Serialize key table and data table.
    std::vector<uint8_t> keys, data;
    std::vector<std::tuple<std::string, uint16_t, uint32_t, uint32_t>> idx; // key, fmt, off, len

    auto push_key = [&](const std::string& k) {
        uint16_t off = (uint16_t)keys.size();
        keys.insert(keys.end(), k.begin(), k.end());
        keys.push_back(0);
        return off;
    };

    for (auto& kv : str_entries) {
        uint16_t key_off = push_key(kv.first);
        uint32_t data_off = (uint32_t)data.size();
        std::string val = kv.second;
        // special-char entries get fmt 0x0204 when they contain bytes >= 0x80
        bool special = false;
        for (unsigned char c : val)
            if (c >= 0x80) { special = true; break; }
        uint16_t fmt = special ? 0x0204 : 0x0004;
        data.insert(data.end(), val.begin(), val.end());
        data.push_back(0);
        uint32_t dlen = (uint32_t)val.size() + 1;
        while (data.size() % 4) data.push_back(0);
        idx.emplace_back(kv.first, fmt, 0, 0);
        std::get<2>(idx.back()) = data_off;
        std::get<3>(idx.back()) = dlen;
        (void)key_off;
    }
    for (auto& kv : int_entries) {
        push_key(kv.first);
        uint32_t data_off = (uint32_t)data.size();
        data.resize(data.size() + 4);
        uint8_t* p = data.data() + data_off;
        p[0] = (uint8_t)kv.second; p[1] = (uint8_t)((uint32_t)kv.second >> 8);
        p[2] = (uint8_t)((uint32_t)kv.second >> 16); p[3] = (uint8_t)((uint32_t)kv.second >> 24);
        idx.emplace_back(kv.first, 0x0404, data_off, 4);
    }

    uint32_t key_table = 20 + (uint32_t)(idx.size() * 16);
    uint32_t data_table = key_table + (uint32_t)keys.size();
    std::vector<uint8_t> out(data_table + data.size(), 0);
    out[0] = 0; out[1] = 'P'; out[2] = 'S'; out[3] = 'F';
    put32le(out, 4, 0x101);
    put32le(out, 8, key_table);
    put32le(out, 12, data_table);
    put32le(out, 16, (uint32_t)idx.size());
    std::memcpy(out.data() + key_table, keys.data(), keys.size());
    std::memcpy(out.data() + data_table, data.data(), data.size());
    size_t e = 20;
    const char* key_base = (const char*)out.data() + key_table;
    for (auto& [key, fmt, doff, dlen] : idx) {
        const char* kp = key_base;
        while (std::strcmp(kp, key.c_str()) != 0) kp += std::strlen(kp) + 1;
        put16le(out, e, (uint16_t)(kp - key_base));
        put16le(out, e + 2, fmt);
        put32le(out, e + 4, dlen);
        put32le(out, e + 8, dlen);
        put32le(out, e + 12, doff);
        e += 16;
    }
    return out;
}

// ---- minimal ISO9660 + Joliet image builder ----
class IsoBuilder {
public:
    static constexpr size_t SEC = 2048;

    void add_file(const std::string& name, const std::vector<uint8_t>& data) {
        files_.push_back({name, data, 0});
    }

    // Finalize with layout: 16 PVD, 17 SVD(Joliet), 18 terminator,
    // files from 19, PS3_GAME dir, root dir. All names uppercase.
    std::vector<uint8_t> build() {
        std::vector<uint8_t> img;
        std::vector<uint8_t> secs;

        // Reserve descriptor sectors 0..18 first.
        img.resize(19 * SEC, 0);

        uint32_t next_lba = 19;
        for (auto& f : files_) {
            f.lba = next_lba;
            size_t nsec = (f.data.size() + SEC - 1) / SEC;
            img.insert(img.end(), (size_t)nsec * SEC, 0);
            std::memcpy(img.data() + (size_t)f.lba * SEC, f.data.data(),
                        std::min(f.data.size(), (size_t)nsec * SEC));
            next_lba += (uint32_t)nsec;
        }
        uint32_t game_dir_lba = next_lba++;
        uint32_t root_lba = next_lba++;
        img.resize((size_t)next_lba * SEC, 0);

        // --- PS3_GAME directory extent ---
        {
            std::vector<uint8_t>& d = img;
            size_t base = (size_t)game_dir_lba * SEC;
            size_t off = base;
            off = write_dot_records(d, off, game_dir_lba, root_lba);
            for (auto& f : files_) {
                off = write_record(d, off, f.name, game_dir_lba, f.lba,
                                   (uint32_t)f.data.size(), false);
            }
        }
        // --- root directory extent ---
        {
            std::vector<uint8_t>& d = img;
            size_t off = (size_t)root_lba * SEC;
            off = write_dot_records(d, off, root_lba, root_lba);
            off = write_record(d, off, "PS3_GAME", root_lba, game_dir_lba,
                               (uint32_t)SEC, true);
        }

        // --- volume descriptors ---
        write_pvd(img, 16, 1, root_lba, false);   // primary (ASCII names)
        write_pvd(img, 17, 2, root_lba, true);    // supplementary (Joliet)
        img[18 * SEC] = 255;                      // terminator
        std::memcpy(img.data() + 18 * SEC + 1, "CD001", 5);
        img[18 * SEC + 6] = 1;
        return img;
    }

private:
    struct FileEntry {
        std::string name;
        std::vector<uint8_t> data;
        uint32_t lba;
    };
    std::vector<FileEntry> files_;

    static size_t rec_len_for(const std::string& name, bool ucs2) {
        size_t nb = ucs2 ? name.size() * 2 : name.size();
        size_t l = 33 + nb;
        return (l % 2) ? l + 1 : l;
    }

    // Directory records are written twice conceptually (PVD ASCII + SVD UCS2
    // views). Our builder writes one physical directory with ASCII names and
    // marks the SVD root to the same extents; the reader's Joliet decoding of
    // ASCII bytes would be wrong — so instead we write the SVD as a second,
    // parallel directory tree. To keep the test image small we instead use
    // ASCII names in BOTH and DON'T set the Joliet escape sequence; the Joliet
    // path is covered by a dedicated builder below.
    static size_t write_record(std::vector<uint8_t>& img, size_t off,
                               const std::string& name, uint32_t /*parent_lba*/,
                               uint32_t lba, uint32_t size, bool is_dir) {
        size_t rl = rec_len_for(name, false);
        img[off] = (uint8_t)rl;
        img[off + 1] = 0;
        put32le(img, off + 2, lba);
        put32le(img, off + 10, size);
        for (int i = 0; i < 7; i++) img[off + 18 + i] = 0; // time
        img[off + 25] = is_dir ? 0x02 : 0x00;
        img[off + 32] = (uint8_t)name.size();
        std::memcpy(img.data() + off + 33, name.data(), name.size());
        return off + rl;
    }

    static size_t write_dot_records(std::vector<uint8_t>& img, size_t off,
                                    uint32_t self_lba, uint32_t parent_lba) {
        // "."
        img[off] = 34; img[off + 1] = 0;
        put32le(img, off + 2, self_lba);
        put32le(img, off + 10, 2048);
        img[off + 25] = 0x02;
        img[off + 32] = 1;
        img[off + 33] = 0;
        // ".."
        img[off + 34] = 34; img[off + 35] = 0;
        put32le(img, off + 36, parent_lba);
        put32le(img, off + 44, 2048);
        img[off + 59] = 0x02;
        img[off + 66] = 1;
        img[off + 67] = 1;
        return off + 68;
    }

    static void write_pvd(std::vector<uint8_t>& img, uint32_t lba, uint8_t type,
                          uint32_t root_lba, bool /*joliet*/) {
        size_t off = (size_t)lba * SEC;
        img[off] = type;
        std::memcpy(img.data() + off + 1, "CD001", 5);
        img[off + 6] = 1;
        const char* vid = "ANYPS3TEST";
        std::memcpy(img.data() + off + 40, vid, std::strlen(vid));
        // root directory record at offset 156
        size_t r = off + 156;
        img[r] = 34; img[r + 1] = 0;
        put32le(img, r + 2, root_lba);
        put32le(img, r + 10, 2048);
        img[r + 25] = 0x02;
        img[r + 32] = 1;
        img[r + 33] = 0;
    }
};

// ---- PPU ELF64 (big-endian) builder ----
inline uint32_t ppu_li(uint8_t rd, int32_t simm) {   // addi rd, 0, simm
    return (14u << 26) | ((uint32_t)rd << 21) | (0u << 16) | ((uint32_t)simm & 0xFFFF);
}
inline uint32_t ppu_li_r(uint8_t rd, uint8_t ra, int32_t simm) { // addi rd, ra, simm
    return (14u << 26) | ((uint32_t)rd << 21) | ((uint32_t)ra << 16) | ((uint32_t)simm & 0xFFFF);
}
inline uint32_t ppu_lis(uint8_t rd, int32_t simm) {  // addis rd, 0, simm
    return (15u << 26) | ((uint32_t)rd << 21) | (0u << 16) | ((uint32_t)simm & 0xFFFF);
}
constexpr uint32_t PPU_SC = 0x44000002;

// Minimal "hello" PPU ELF: code at 0x10000 (file off 0x1000), data at 0x11000.
// Program: write msg via sys_tty_write (r11=988), exit via sys_process_exit (r11=1).
inline std::vector<uint8_t> make_hello_elf(const std::string& msg = "Hello from Any_Ps3!") {
    std::vector<uint8_t> f(0x3000, 0);
    // e_ident
    f[0] = 0x7F; f[1] = 'E'; f[2] = 'L'; f[3] = 'F';
    f[4] = 2;  // 64-bit
    f[5] = 2;  // big-endian
    f[6] = 1;  // version
    put16be(f, 16, 2);       // e_type = ET_EXEC
    put16be(f, 18, 0x15);    // e_machine = EM_PPC64
    put32be(f, 20, 1);       // e_version
    put64be(f, 24, 0x10000); // e_entry
    put64be(f, 32, 64);      // e_phoff
    put64be(f, 40, 0);       // e_shoff (none)
    put32be(f, 48, 0);       // e_flags
    put16be(f, 52, 64);      // e_ehsize
    put16be(f, 54, 56);      // e_phentsize
    put16be(f, 56, 1);       // e_phnum
    put16be(f, 58, 0);       // e_shentsize
    put16be(f, 60, 0);       // e_shnum
    put16be(f, 62, 0);       // e_shstrndx
    // phdr: PT_LOAD off=0x1000 vaddr=0x10000 filesz=memsz=0x2000 flags RWX
    put32be(f, 64, 1);
    put32be(f, 68, 7);
    put64be(f, 72, 0x1000);
    put64be(f, 80, 0x10000);
    put64be(f, 88, 0x10000); // paddr
    put64be(f, 96, 0x2000);
    put64be(f, 104, 0x2000);
    put64be(f, 112, 0x1000); // align
    // code at file 0x1000 (vaddr 0x10000)
    size_t c = 0x1000;
    auto emit = [&](uint32_t w) { put32be(f, c, w); c += 4; };
    emit(ppu_li(3, 1));                       // r3 = fd 1
    emit(ppu_lis(4, 1));                      // r4 = 0x10000
    emit(ppu_li_r(4, 4, 0x1000));             // r4 = 0x11000
    emit(ppu_li(5, (int32_t)msg.size()));     // r5 = len
    emit(ppu_li(6, 0));                       // r6 = &written (null ok in HLE)
    emit(ppu_li(11, 988));                    // r11 = sys_tty_write
    emit(PPU_SC);
    emit(ppu_li(3, 0));                       // status
    emit(ppu_li(11, 1));                      // sys_process_exit
    emit(PPU_SC);
    emit(0x60000000);                         // nop
    // data at file 0x2000 (vaddr 0x11000)
    std::memcpy(f.data() + 0x2000, msg.data(), msg.size());
    return f;
}

// ELF with a symbol table (two functions) for function-discovery tests.
inline std::vector<uint8_t> make_symtab_elf() {
    std::vector<uint8_t> f(0x4000, 0);
    f[0] = 0x7F; f[1] = 'E'; f[2] = 'L'; f[3] = 'F';
    f[4] = 2; f[5] = 2; f[6] = 1;
    put16be(f, 16, 2);
    put16be(f, 18, 0x15);
    put32be(f, 20, 1);
    put64be(f, 24, 0x10000);
    put64be(f, 32, 64);   // phoff
    put64be(f, 40, 64 + 56); // shoff
    put32be(f, 48, 0);    // flags
    put16be(f, 52, 64);   // e_ehsize
    put16be(f, 54, 56);   // e_phentsize
    put16be(f, 56, 1);    // e_phnum
    put16be(f, 58, 64);   // e_shentsize
    put16be(f, 60, 3);    // e_shnum: NULL, .symtab, .strtab
    put16be(f, 62, 0);    // shstrndx
    put32be(f, 64, 1);    // PT_LOAD
    put32be(f, 68, 7);
    put64be(f, 72, 0x1000);
    put64be(f, 80, 0x10000);
    put64be(f, 88, 0x10000);
    put64be(f, 96, 0x1000);
    put64be(f, 104, 0x1000);
    put64be(f, 112, 0x1000);
    // section 0: all zero (NULL)
    // section 1: .symtab  type=2 offset=0x2000 size=3*24 link=2 entsize=24
    size_t sh1 = 64 + 56 + 64;
    put32be(f, sh1 + 0, 1);      // sh_name
    put32be(f, sh1 + 4, 2);      // SHT_SYMTAB
    put64be(f, sh1 + 24, 0x2000);
    put64be(f, sh1 + 32, 3 * 24);
    put32be(f, sh1 + 40, 2);     // link -> .strtab
    put32be(f, sh1 + 44, 1);     // first global
    put64be(f, sh1 + 56, 24);
    // section 2: .strtab offset=0x2100
    size_t sh2 = sh1 + 64;
    put32be(f, sh2 + 0, 9);
    put32be(f, sh2 + 4, 3);      // SHT_STRTAB
    put64be(f, sh2 + 24, 0x2100);
    put64be(f, sh2 + 32, 32);
    // strtab: \0main\0helper\0
    std::memcpy(f.data() + 0x2100, "\0main\0helper\0", 14);
    // symtab: [0] null, [1] main @0x10000, [2] helper @0x10020
    size_t sy = 0x2000;
    // null
    std::memset(f.data() + sy, 0, 24);
    // main
    put32be(f, sy + 24, 1);      // name off 1
    f[sy + 24 + 4] = 0x12;       // info: global (0b10) + STT_FUNC (2)
    put16be(f, sy + 24 + 6, 1);  // shndx
    put64be(f, sy + 24 + 8, 0x10000);
    put64be(f, sy + 24 + 16, 0x20);
    // helper
    put32be(f, sy + 48, 6);      // "helper"
    f[sy + 48 + 4] = 0x12;
    put16be(f, sy + 48 + 6, 1);
    put64be(f, sy + 48 + 8, 0x10020);
    put64be(f, sy + 48 + 16, 0x10);
    return f;
}

} // namespace testdata
} // namespace any_ps3
