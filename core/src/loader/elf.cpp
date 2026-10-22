#include "anyps3/loader/elf.h"

#include <algorithm>
#include <cstring>

#include "anyps3/log.h"

namespace anyps3 {

// ELF64 (big-endian) header offsets:
//  e_ident[16]  e_type u16  e_machine u16  e_version u32
//  e_entry u64  e_phoff u64  e_shoff u64  e_flags u32
//  e_ehsize u16  e_phentsize u16  e_phnum u16
//  e_shentsize u16  e_shnum u16  e_shstrndx u16
// Program header (Phdr, 56 bytes):
//  p_type u32  p_flags u32  p_offset u64  p_vaddr u64  p_paddr u64
//  p_filesz u64  p_memsz u64  p_align u64

static constexpr u16 kEtExec = 2;
static constexpr u32 kPtLoad = 1;
static constexpr u32 kPfX = 1, kPfW = 2, kPfR = 4;

bool LoadElf64(const u8* data, size_t size, ElfImage* out, std::string* err) {
    if (size < 64) {
        *err = "file too small for ELF64 header";
        return false;
    }
    if (std::memcmp(data, "\x7f""ELF", 4) != 0) {
        *err = "not an ELF file (bad magic)";
        return false;
    }
    if (data[4] != 2 /*ELFCLASS64*/) {
        *err = "not a 64-bit ELF (PS3 PPU executables are ELF64)";
        return false;
    }
    if (data[5] != 2 /*ELFDATA2MSB*/) {
        *err = "not big-endian (PS3 PPU executables are big-endian ELF64)";
        return false;
    }
    u8 osabi = data[7];
    if (osabi != 0 && osabi != 0x66) {
        AP_WARN("ELF: unusual OS-ABI byte 0x", static_cast<unsigned>(osabi),
                " (expected 0x66 CELL_LV2 or 0) — loading anyway");
    }
    u16 e_type = ReadBE16(data + 16);
    u16 e_machine = ReadBE16(data + 18);
    if (e_machine != kEmPpc64) {
        *err = "machine is not EM_PPC64";
        return false;
    }
    if (e_type != kEtExec && e_type != 0xfe04 /*SCE exec*/) {
        AP_WARN("ELF: e_type 0x", static_cast<unsigned>(e_type), " (PS3 uses ET_EXEC=2)");
    }
    u64 e_entry = ReadBE64(data + 24);
    u64 e_phoff = ReadBE64(data + 32);
    u16 e_phentsize = ReadBE16(data + 54);
    u16 e_phnum = ReadBE16(data + 56);
    if (e_phnum == 0 || e_phoff == 0) {
        *err = "no program headers";
        return false;
    }
    if (e_phentsize < 56) {
        *err = "bad phdr entry size";
        return false;
    }
    if (e_phoff + static_cast<u64>(e_phnum) * e_phentsize > size) {
        *err = "program header table out of bounds";
        return false;
    }
    out->segments.clear();
    out->entry = e_entry;
    out->base = ~0ULL;
    for (u16 i = 0; i < e_phnum; i++) {
        const u8* ph = data + e_phoff + static_cast<size_t>(i) * e_phentsize;
        u32 p_type = ReadBE32(ph);
        if (p_type != kPtLoad) continue;
        u32 p_flags = ReadBE32(ph + 4);
        u64 p_offset = ReadBE64(ph + 8);
        u64 p_vaddr = ReadBE64(ph + 16);
        u64 p_filesz = ReadBE64(ph + 32);
        u64 p_memsz = ReadBE64(ph + 40);
        if (p_offset + p_filesz > size) {
            *err = "PT_LOAD file range out of bounds";
            return false;
        }
        ElfSegment seg;
        seg.vaddr = p_vaddr;
        seg.file_size = p_filesz;
        seg.mem_size = p_memsz;
        seg.flags = p_flags;
        seg.bytes.assign(data + p_offset, data + p_offset + p_filesz);
        out->segments.push_back(std::move(seg));
        out->base = std::min(out->base, p_vaddr);
    }
    if (out->segments.empty()) {
        *err = "no PT_LOAD segments";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// SELF (SCE) container
// ---------------------------------------------------------------------------

static constexpr u32 kSceMagic = 0x53434500;  // "SCE\0"

bool LoadSelf(const u8* data, size_t size, SelfInfo* info, ElfImage* elf_out,
              std::string* err) {
    info->status = SelfStatus::NotSelf;
    if (size < 32) {
        *err = "SELF: file too small";
        return false;
    }
    u32 magic = ReadBE32(data);
    if (magic != kSceMagic) {
        info->status = SelfStatus::NotSelf;
        *err = "not a SELF file";
        return false;
    }
    info->sce_version = ReadBE32(data + 4);
    u16 flags = ReadBE16(data + 8);
    u32 meta_off = ReadBE32(data + 12);
    u64 hdr_size = ReadBE64(data + 16);
    u64 file_size = ReadBE64(data + 24);
    (void)flags;
    (void)file_size;
    if (hdr_size + 80 > size) {
        *err = "SELF: header out of bounds";
        info->status = SelfStatus::BadSelf;
        return false;
    }
    // Extension header (10 x u64) at `hdr_size`.
    const u8* ex = data + hdr_size;
    u64 app_info_off = ReadBE64(ex + 8);
    u64 ehdr_off = ReadBE64(ex + 16);
    u64 phdr_off = ReadBE64(ex + 24);
    u64 seg_info_off = ReadBE64(ex + 40);

    if (app_info_off + 32 <= size) {
        info->app_auth_id = ReadBE64(data + app_info_off);
        info->app_vendor_id = ReadBE32(data + app_info_off + 8);
    }

    if (meta_off == 0) {
        // Plaintext/fake SELF (homebrew, debug): the ELF header and phdrs are stored
        // verbatim; segment data follows per segment ext headers.
        info->status = SelfStatus::PlaintextSelf;
        u16 phnum = 0;
        {
            // e_phnum at ehdr + 56
            if (ehdr_off + 64 > size) {
                *err = "SELF: embedded ELF header out of bounds";
                return false;
            }
            const u8* eh = data + ehdr_off;
            if (std::memcmp(eh, "\x7f""ELF", 4) != 0) {
                *err = "SELF: embedded ELF magic mismatch";
                return false;
            }
            phnum = ReadBE16(eh + 56);
            elf_out->entry = ReadBE64(eh + 24);
            elf_out->base = ~0ULL;
        }
        u16 phentsize = 56;
        for (u16 i = 0; i < phnum; i++) {
            // segment ext headers are 32 bytes: u64 offset, u64 size, u32 compression,
            // u32 unknown, u64 encryption
            const u8* seg_hdr = data + seg_info_off + static_cast<size_t>(i) * 32;
            const u8* ph = data + phdr_off + static_cast<size_t>(i) * phentsize;
            if (static_cast<size_t>(ph + 56 - data) > size ||
                static_cast<size_t>(seg_hdr + 32 - data) > size) {
                *err = "SELF: phdr/segment header out of bounds";
                return false;
            }
            u32 p_type = ReadBE32(ph);
            if (p_type != kPtLoad) continue;
            u64 p_offset = ReadBE64(seg_hdr);       // offset of data in file
            u64 p_filesz = ReadBE64(seg_hdr + 8);    // stored (maybe compressed) size
            u32 compression = ReadBE32(seg_hdr + 16);
            u64 encryption = ReadBE64(seg_hdr + 24);
            u64 p_vaddr = ReadBE64(ph + 16);
            u64 p_memsz = ReadBE64(ph + 40);
            u32 p_flags = ReadBE32(ph + 4);
            if (compression != 1 /* plain */ || encryption != 0 /* not encrypted */) {
                *err = "SELF: compressed/encrypted segment needs keys Any_Ps3 does not "
                       "ship (plaintext fSELF executables are supported)";
                info->status = SelfStatus::EncryptedSelf;
                return false;
            }
            if (p_offset + p_filesz > size) {
                *err = "SELF: segment data out of bounds";
                return false;
            }
            ElfSegment seg;
            seg.vaddr = p_vaddr;
            seg.file_size = p_filesz;
            seg.mem_size = p_memsz;
            seg.flags = p_flags;
            seg.bytes.assign(data + p_offset, data + p_offset + p_filesz);
            elf_out->segments.push_back(std::move(seg));
            elf_out->base = std::min(elf_out->base, p_vaddr);
        }
        if (elf_out->segments.empty()) {
            *err = "SELF: no PT_LOAD segments";
            return false;
        }
        return true;
    }

    *err = "SELF: encrypted retail executable — Any_Ps3 ships no keys by design; "
           "provide a plaintext/fSELF executable";
    info->status = SelfStatus::EncryptedSelf;
    return false;
}

bool LoadExecutable(const u8* data, size_t size, const std::string& name, ElfImage* out,
                    SelfInfo* self_info, std::string* err) {
    out->name = name;
    bool is_self = size >= 4 && ReadBE32(data) == kSceMagic;
    if (is_self) {
        return LoadSelf(data, size, self_info, out, err);
    }
    self_info->status = SelfStatus::NotSelf;
    return LoadElf64(data, size, out, err);
}

// ---------------------------------------------------------------------------
// Test/tool builder
// ---------------------------------------------------------------------------

Elf64Builder::Elf64Builder() = default;

void Elf64Builder::AddSegment(u64 vaddr, u32 flags, std::vector<u8> bytes, u64 mem_size) {
    Seg s;
    s.vaddr = vaddr;
    s.flags = flags;
    s.bytes = std::move(bytes);
    s.mem_size = mem_size ? mem_size : s.bytes.size();
    segs_.push_back(std::move(s));
}

std::vector<u8> Elf64Builder::Build() const {
    const u64 e_phoff = 64;
    const u16 phentsize = 56;
    u64 data_off = e_phoff + static_cast<u64>(segs_.size()) * phentsize;

    // Build the three pieces separately (the final vector must not move under us).
    std::vector<u8> ehdr(64, 0);
    {
        u8* p = ehdr.data();
        std::memcpy(p, "\x7f""ELF", 4);
        p[4] = 2;     // ELF64
        p[5] = 2;     // big-endian
        p[6] = 1;     // EV_CURRENT
        p[7] = 0x66;  // CELL LV2 OS-ABI
        WriteBE16(p + 16, kEtExec);
        WriteBE16(p + 18, kEmPpc64);
        WriteBE32(p + 20, 1);   // EV_CURRENT
        WriteBE64(p + 24, entry_);
        WriteBE64(p + 32, e_phoff);
        WriteBE64(p + 40, 0);   // no section headers
        WriteBE32(p + 48, 0);   // e_flags
        WriteBE16(p + 52, 64);
        WriteBE16(p + 54, phentsize);
        WriteBE16(p + 56, static_cast<u16>(segs_.size()));
        WriteBE16(p + 58, 0);
        WriteBE16(p + 60, 0);
        WriteBE16(p + 62, 0);
    }
    std::vector<u8> phdrs(static_cast<size_t>(segs_.size()) * phentsize, 0);
    std::vector<u8> data;
    for (size_t i = 0; i < segs_.size(); i++) {
        const Seg& s = segs_[i];
        u8* ph = phdrs.data() + i * phentsize;
        WriteBE32(ph, kPtLoad);
        WriteBE32(ph + 4, s.flags);
        WriteBE64(ph + 8, data_off);
        WriteBE64(ph + 16, s.vaddr);
        WriteBE64(ph + 24, s.vaddr);
        WriteBE64(ph + 32, s.bytes.size());
        WriteBE64(ph + 40, s.mem_size);
        WriteBE64(ph + 48, 0x10000);  // align
        data.insert(data.end(), s.bytes.begin(), s.bytes.end());
        data_off += s.bytes.size();
    }
    std::vector<u8> out;
    out.reserve(ehdr.size() + phdrs.size() + data.size());
    out.insert(out.end(), ehdr.begin(), ehdr.end());
    out.insert(out.end(), phdrs.begin(), phdrs.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

}  // namespace anyps3
