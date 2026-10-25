#include "any_ps3/elf.h"
#include "any_ps3/bytes.h"

#include <cstring>

namespace any_ps3 {

bool looks_like_sony_encrypted(const uint8_t* d, size_t n) {
    if (n < 8) return false;
    // "SELF" (0x53 0x45 0x4C 0x46), "S\0ELF" seen in some containers, "SCE\0".
    if (d[0] == 'S' && d[1] == 'E' && d[2] == 'L' && d[3] == 'F') return true;
    if (d[0] == 'S' && d[1] == 0x00 && d[2] == 'E' && d[3] == 'L' && d[4] == 'F') return true;
    if (d[0] == 'S' && d[1] == 'C' && d[2] == 'E' && d[3] == 0x00) return true;
    return false;
}

std::string sony_encrypted_message() {
    return "encrypted Sony executable (SELF/SCE) — decryption is NOT supported. "
           "Any_Ps3 does not ship console-derived keys. Use a decrypted ELF "
           "(e.g. homebrew) or supply your own legally obtained, decrypted executable.";
}

ElfImage elf_parse(const uint8_t* data, size_t len) {
    if (looks_like_sony_encrypted(data, len)) throw Error(sony_encrypted_message());
    if (len < 64) throw Error("file too small to be an ELF");
    if (data[0] != 0x7F || data[1] != 'E' || data[2] != 'L' || data[3] != 'F')
        throw Error("not an ELF executable");
    uint8_t ei_class = data[4];
    uint8_t ei_data = data[5];
    if (ei_class != 2) throw Error("32-bit ELF not supported (PS3 PPU executables are 64-bit)");
    if (ei_data != 2) throw Error("little-endian ELF — PPU executables are big-endian");

    ByteReader r(data, len);
    r.take(16); // e_ident
    uint16_t e_type = r.u16be();
    uint16_t machine = r.u16be();
    r.u32be(); // version
    uint64_t entry = r.u64be();
    uint64_t phoff = r.u64be();
    r.u64be(); // shoff
    r.u32be(); // flags
    r.u16be(); // ehsize
    r.u16be(); // phentsize
    uint16_t phnum = r.u16be();
    r.u16be(); // shentsize
    uint16_t shnum = r.u16be();
    r.u16be(); // shstrndx

    if (machine != 0x15)
        throw Error("unsupported ELF machine (expected PowerPC64 / e_machine 0x15)");

    ElfImage img;
    img.e_type = e_type;
    img.machine = machine;
    img.entry = entry;

    if (phnum > 96) throw Error("implausible program header count");
    for (uint16_t i = 0; i < phnum; i++) {
        if (phoff + (uint64_t)(i + 1) * 56 > len) throw Error("program header out of bounds");
        ByteReader ph(data + phoff + (uint64_t)i * 56, 56);
        ElfSegment seg;
        seg.type = ph.u32be();
        seg.flags = ph.u32be();
        seg.file_off = ph.u64be();
        seg.vaddr = ph.u64be();
        ph.u64be(); // paddr
        seg.filesz = ph.u64be();
        seg.memsz = ph.u64be();
        if (seg.type == 1 /*PT_LOAD*/) {
            if (seg.file_off + seg.filesz > len) throw Error("segment beyond file");
            img.segments.push_back(std::move(seg));
        }
    }
    if (img.segments.empty() && phnum > 0)
        throw Error("no PT_LOAD segments");

    // Optional symbol table scan for function discovery.
    if (shnum > 0 && shnum < 4096) {
        ByteReader eh(data, 64);
        eh.off = 40; // e_shoff
        uint64_t shoff = eh.u64be();
        for (uint16_t i = 0; i < shnum; i++) {
            ByteReader sh(data + shoff + (uint64_t)i * 64, 64);
            uint32_t name_off = sh.u32be();
            uint32_t type = sh.u32be();
            sh.u64be(); // flags
            sh.u64be(); // addr
            uint64_t offset = sh.u64be();
            uint64_t size = sh.u64be();
            uint32_t link = sh.u32be();
            sh.u32be(); // info
            sh.u64be();
            uint64_t entsize = sh.u64be();
            (void)name_off;
            if (type != 2 /*SHT_SYMTAB*/) continue;

            // string table = section 'link'
            if (link >= shnum) break;
            ByteReader sth(data + shoff + (uint64_t)link * 64, 64);
            sth.u32be(); sth.u32be(); sth.u64be(); sth.u64be();
            uint64_t strtab_off = sth.u64be();
            uint64_t strtab_size = sth.u64be();
            if (strtab_off + strtab_size > len) break;

            if (entsize != 24 || size / 24 > 200000) break;
            for (uint64_t s = 0; s + 24 <= size; s += 24) {
                ByteReader sym(data + offset + s, 24);
                uint32_t nm = sym.u32be();
                uint8_t info = sym.u8();
                sym.u8(); // other
                uint16_t shndx = sym.u16be();
                uint64_t value = sym.u64be();
                uint64_t sz = sym.u64be();
                if ((info & 0x0F) != 2 /*STT_FUNC*/ || shndx == 0 /*SHN_UNDEF*/) continue;
                if (nm >= strtab_size) continue;
                const char* np = (const char*)data + strtab_off + nm;
                size_t nl = strnlen(np, strtab_size - nm);
                ElfFunction f;
                f.name.assign(np, nl);
                f.addr = value;
                f.size = sz;
                img.functions.push_back(std::move(f));
            }
            img.has_symtab = !img.functions.empty();
            break;
        }
    }

    if (img.functions.empty())
        img.functions.push_back({"__entry__", img.entry, 0});

    return img;
}

bool PpuMemory::load(const ElfImage& img, const uint8_t* file, size_t filen) {
    segs_.clear();
    for (const auto& s : img.segments) {
        Segment seg;
        seg.vaddr = s.vaddr;
        seg.memsz = s.memsz;
        if (seg.memsz > 256ull * 1024 * 1024) return false; // sanity cap
        seg.data.assign((size_t)seg.memsz, 0);
        if (s.filesz > seg.memsz) return false;
        if (s.file_off + s.filesz > filen) return false;
        std::memcpy(seg.data.data(), file + s.file_off, (size_t)s.filesz);
        segs_.push_back(std::move(seg));
    }
    return true;
}

uint8_t* PpuMemory::mutable_bytes(uint64_t addr, size_t len) {
    for (auto& s : segs_) {
        if (addr >= s.vaddr && addr + len <= s.vaddr + s.memsz)
            return s.data.data() + (addr - s.vaddr);
    }
    return nullptr;
}

bool PpuMemory::read(uint64_t addr, void* out, size_t len) const {
    for (const auto& s : segs_) {
        if (addr >= s.vaddr && addr + len <= s.vaddr + s.memsz) {
            std::memcpy(out, s.data.data() + (addr - s.vaddr), len);
            return true;
        }
    }
    return false;
}

bool PpuMemory::write(uint64_t addr, const void* in, size_t len) {
    uint8_t* p = mutable_bytes(addr, len);
    if (!p) return false;
    std::memcpy(p, in, len);
    return true;
}

uint64_t PpuMemory::total_mapped() const {
    uint64_t t = 0;
    for (auto& s : segs_) t += s.memsz;
    return t;
}

} // namespace any_ps3
