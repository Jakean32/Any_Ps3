#include "anyps3/loader/elf.h"

#include <algorithm>
#include <cstring>

#include "anyps3/log.h"
#include "anyps3/hle/modules.h"
#include "anyps3/memory/vm.h"

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
    if (e_type != kEtExec && e_type != kEtSceExec && e_type != kEtScePpuRelExec) {
        AP_WARN("ELF: e_type 0x", static_cast<unsigned>(e_type),
                " (PS3 uses ET_EXEC=2 / ET_SCE_EXEC=0xFE00 / PPURELEXEC=0xFFA4)");
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
    out->phdrs.clear();
    out->entry = e_entry;
    out->base = ~0ULL;
    out->e_type = e_type;
    out->module_info_va = 0;
    out->proc_param = {};
    out->tls_filesz = out->tls_memsz = out->tls_vaddr = 0;
    bool first_load = true;
    for (u16 i = 0; i < e_phnum; i++) {
        const u8* ph = data + e_phoff + static_cast<size_t>(i) * e_phentsize;
        Ps3Phdr raw;
        raw.type = ReadBE32(ph);
        raw.flags = ReadBE32(ph + 4);
        raw.offset = ReadBE64(ph + 8);
        raw.vaddr = ReadBE64(ph + 16);
        raw.paddr = ReadBE64(ph + 24);
        raw.filesz = ReadBE64(ph + 32);
        raw.memsz = ReadBE64(ph + 40);
        raw.align = ReadBE64(ph + 48);
        out->phdrs.push_back(raw);

        if (raw.type == kPtLoad) {
            if (first_load) {
                // PS3 trick: the first PT_LOAD's p_paddr holds the virtual
                // address of sceModuleInfo. Adopt it for SCE image types, and
                // for plain ET_EXEC only when p_paddr points INSIDE this
                // segment's file image (PS3 homebrew linkers do this); old
                // demo fixtures set p_paddr == p_vaddr (meaningless).
                bool is_sce = (e_type == kEtSceExec || e_type == kEtScePpuRelExec);
                if (is_sce || (raw.paddr >= raw.vaddr + 4 && raw.paddr < raw.vaddr + raw.filesz)) {
                    out->module_info_va = raw.paddr;
                }
                first_load = false;
            }
            if (raw.offset + raw.filesz > size) {
                *err = "PT_LOAD file range out of bounds";
                return false;
            }
            ElfSegment seg;
            seg.vaddr = raw.vaddr;
            seg.file_size = raw.filesz;
            seg.mem_size = raw.memsz;
            seg.flags = raw.flags;
            seg.bytes.assign(data + raw.offset, data + raw.offset + raw.filesz);
            out->segments.push_back(std::move(seg));
            out->base = std::min(out->base, raw.vaddr);
        } else if (raw.type == kPtSceTlsInfo) {
            out->tls_filesz = raw.filesz;
            out->tls_memsz = raw.memsz;
            out->tls_vaddr = raw.vaddr;
        } else if (raw.type == kPtSceProcParam) {
            out->proc_param_va = raw.vaddr;
            if (raw.filesz >= 0x2C && raw.offset + raw.filesz <= size) {
            // sys_proc_prx_param block (magic 0x1b434cec):
            //  u32 size, u32 magic, u32 version, u32 unk0,
            //  u32 libent_start, u32 libent_end, u32 libstub_start, u32 libstub_end,
            //  u16 ver, u16 unk1, u32 unk2
            const u8* pp = data + raw.offset;
            if (ReadBE32(pp + 4) == kProcParamMagic) {
                out->proc_param.present = true;
                out->proc_param.version = ReadBE32(pp + 8);
                out->proc_param.libent_start = ReadBE32(pp + 16);
                out->proc_param.libent_end = ReadBE32(pp + 20);
                out->proc_param.libstub_start = ReadBE32(pp + 24);
                out->proc_param.libstub_end = ReadBE32(pp + 28);
            } else {
                AP_WARN("ELF: proc-prx-param magic mismatch — ignored");
            }
            }
        }
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
            elf_out->e_type = ReadBE16(eh + 16);
            elf_out->module_info_va = 0;
            elf_out->proc_param = {};
            elf_out->proc_param_va = 0;
            elf_out->tls_filesz = elf_out->tls_memsz = elf_out->tls_vaddr = 0;
        }
        u16 phentsize = 56;
        bool first_load = true;
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
            Ps3Phdr raw;
            raw.type = ReadBE32(ph);
            raw.flags = ReadBE32(ph + 4);
            raw.offset = ReadBE64(ph + 8);
            raw.vaddr = ReadBE64(ph + 16);
            raw.paddr = ReadBE64(ph + 24);
            raw.filesz = ReadBE64(ph + 32);
            raw.memsz = ReadBE64(ph + 40);
            raw.align = ReadBE64(ph + 48);
            elf_out->phdrs.push_back(raw);
            if (raw.type == kPtLoad) {
                if (first_load) {
                    bool is_sce = (elf_out->e_type == kEtSceExec ||
                                   elf_out->e_type == kEtScePpuRelExec);
                    if (is_sce || (raw.paddr >= raw.vaddr + 4 &&
                                   raw.paddr < raw.vaddr + raw.filesz)) {
                        elf_out->module_info_va = raw.paddr;
                    }
                    first_load = false;
                }
            } else if (raw.type == kPtSceTlsInfo) {
                elf_out->tls_filesz = raw.filesz;
                elf_out->tls_memsz = raw.memsz;
                elf_out->tls_vaddr = raw.vaddr;
            } else if (raw.type == kPtSceProcParam) {
                elf_out->proc_param_va = raw.vaddr;
            }
            if (raw.type != kPtLoad) continue;
            u64 p_offset = ReadBE64(seg_hdr);       // offset of data in file
            u64 p_filesz = ReadBE64(seg_hdr + 8);    // stored (maybe compressed) size
            u32 compression = ReadBE32(seg_hdr + 16);
            u64 encryption = ReadBE64(seg_hdr + 24);
            u64 p_vaddr = raw.vaddr;
            u64 p_memsz = raw.memsz;
            u32 p_flags = raw.flags;
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
// PS3 module facts: sceModuleInfo + sys_proc_prx_param + libstub imports
// (parsed from MAPPED guest memory; layout per psdevwiki PRX page, format
// facts cross-checked against the PSL1GHT stub/linker sources).
// ---------------------------------------------------------------------------

bool ParsePs3ModuleInfo(const u8* guest_mem, u32 module_info_va, Ps3ModuleInfo* out) {
    // `guest_mem` is the VM backing base (backing[i] == guest address i), so
    // the module-info structure lives at backing offset `module_info_va`.
    const u8* p = guest_mem + module_info_va;
    *out = Ps3ModuleInfo{};
    out->modattr = ReadBE16(p + 0);
    out->modver[0] = p[2];
    out->modver[1] = p[3];
    std::memcpy(out->modname, p + 4, 27);
    // +0x1F: infover (byte 27 of modname buffer)
    out->gp = ReadBE32(p + 0x20);
    out->libent_top = ReadBE32(p + 0x24);
    out->libent_btm = ReadBE32(p + 0x28);
    out->libstub_top = ReadBE32(p + 0x2C);
    out->libstub_btm = ReadBE32(p + 0x30);
    return true;
}

bool ParseProcParamGuest(Vm& vm, u32 va, Ps3ProcParam* out) {
    *out = Ps3ProcParam{};
    if (!vm.Ptr(va, 0x2C)) return false;
    if (vm.Read32(va + 4) != kProcParamMagic) return false;
    out->present = true;
    out->version = vm.Read32(va + 8);
    out->libent_start = vm.Read32(va + 16);
    out->libent_end = vm.Read32(va + 20);
    out->libstub_start = vm.Read32(va + 24);
    out->libstub_end = vm.Read32(va + 28);
    return true;
}

bool ReadModuleImports(Vm& vm, const ElfImage& elf, std::vector<ModuleImport>* imports,
                       std::string* module_name, std::string* err) {
    imports->clear();
    module_name->clear();
    if (err) err->clear();

    // Import range: prefer sceModuleInfo's libstub window; fall back to the
    // sys_proc_prx_param block (phdr 0x60000002) parsed from guest memory.
    u32 top = 0, btm = 0;
    Ps3ModuleInfo mi{};
    bool have_mi = false;
    if (elf.module_info_va && vm.Ptr(static_cast<u32>(elf.module_info_va), 0x34)) {
        ParsePs3ModuleInfo(vm.mutable_data(), static_cast<u32>(elf.module_info_va), &mi);
        have_mi = true;
        module_name->assign(mi.modname, std::strlen(mi.modname));
        top = mi.libstub_top;
        btm = mi.libstub_btm;
    }
    if (!vm.IsValid(top) || !vm.IsValid(btm) || top >= btm || btm - top > 0x10000) {
        // fall back to proc-prx-param (in-file or guest-side)
        Ps3ProcParam pp = elf.proc_param;
        if (!pp.present && elf.proc_param_va) {
            ParseProcParamGuest(vm, static_cast<u32>(elf.proc_param_va), &pp);
        }
        top = pp.libstub_start;
        btm = pp.libstub_end;
    }
    if (!vm.IsValid(top) || !vm.IsValid(btm) || top >= btm || btm - top > 0x10000) {
        if (have_mi || elf.proc_param_va) {
            AP_WARN("module imports: libstub window ", top, "..", btm,
                    " out of guest range — treating as no imports");
        }
        return true;  // no import tables — fine (e.g. plain demo ELFs)
    }

    u32 skipped_refs = 0;
    for (u32 addr = top; addr < btm;) {
        if (!vm.Ptr(addr, 0x2C)) {
            if (err) *err = "libstub entry out of range";
            return false;
        }
        u16 size = vm.Read16(addr);
        if (size < 0x2C) {
            if (err) *err = "libstub entry size < 0x2C";
            return false;
        }
        u16 attribute = vm.Read16(addr + 4);
        u16 nfunc = vm.Read16(addr + 6);
        u16 nvar = vm.Read16(addr + 8);
        u16 ntls = vm.Read16(addr + 10);
        u32 libname = vm.Read32(addr + 16);
        u32 fnid_table = vm.Read32(addr + 20);
        u32 fent_table = vm.Read32(addr + 24);
        if (ntls) {
            AP_WARN("module imports: ", static_cast<unsigned>(ntls),
                    " TLS import(s) present — not supported yet");
        }
        std::string lib = HleReadStr(vm, libname, 64);
        for (u16 i = 0; i < nfunc; i++) {
            if (!vm.Ptr(fnid_table + i * 4, 4) || !vm.Ptr(fent_table + i * 4, 4)) {
                if (err) *err = "NID/entry table out of range";
                return false;
            }
            ModuleImport imp;
            imp.module_name = lib;
            imp.nid = vm.Read32(fnid_table + i * 4);
            imp.slot_va = vm.Read32(fent_table + i * 4);
            // attribute & 0x2000: an extra "use address" array follows the NIDs
            if (attribute & 0x2000 && nfunc) {
                u32 use = vm.Read32(fnid_table + static_cast<u32>(nfunc) * 4 + i * 4);
                if (use) {
                    imp.use_va = use;
                    skipped_refs++;  // patched like the slot (see linker)
                }
            }
            imports->push_back(imp);
        }
        if (nvar) {
            AP_WARN("module imports: ", static_cast<unsigned>(nvar), " variable import(s) from '",
                    lib, "' — not supported yet");
        }
        addr += size;
    }
    (void)skipped_refs;
    return true;
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
    const bool with_proc = ps3_proc_param_va_ != 0;
    const u16 phnum = static_cast<u16>(segs_.size() + (with_proc ? 1 : 0));
    u64 data_off = e_phoff + static_cast<u64>(phnum) * phentsize;

    // Build the pieces separately (the final vector must not move under us).
    std::vector<u8> ehdr(64, 0);
    {
        u8* p = ehdr.data();
        std::memcpy(p, "\x7f""ELF", 4);
        p[4] = 2;     // ELF64
        p[5] = 2;     // big-endian
        p[6] = 1;     // EV_CURRENT
        p[7] = 0x66;  // CELL LV2 OS-ABI
        WriteBE16(p + 16, e_type_);
        WriteBE16(p + 18, kEmPpc64);
        WriteBE32(p + 20, 1);   // EV_CURRENT
        WriteBE64(p + 24, entry_);
        WriteBE64(p + 32, e_phoff);
        WriteBE64(p + 40, 0);   // no section headers
        WriteBE32(p + 48, 0);   // e_flags
        WriteBE16(p + 52, 64);
        WriteBE16(p + 54, phentsize);
        WriteBE16(p + 56, phnum);
        WriteBE16(p + 58, 0);
        WriteBE16(p + 60, 0);
        WriteBE16(p + 62, 0);
    }
    // proc-param content (0x2C bytes) follows the LOAD segment data in-file
    u64 proc_param_off = data_off;
    for (const Seg& s : segs_) proc_param_off += s.bytes.size();
    std::vector<u8> proc_data;
    if (with_proc) {
        proc_data.assign(0x2C, 0);
        WriteBE32(proc_data.data() + 0, 0x2C);
        WriteBE32(proc_data.data() + 4, kProcParamMagic);
        WriteBE32(proc_data.data() + 8, 1);  // version
        WriteBE32(proc_data.data() + 16, proc_libent_start_);
        WriteBE32(proc_data.data() + 20, proc_libent_end_);
        WriteBE32(proc_data.data() + 24, proc_libstub_start_);
        WriteBE32(proc_data.data() + 28, proc_libstub_end_);
    }

    std::vector<u8> phdrs(static_cast<size_t>(phnum) * phentsize, 0);
    std::vector<u8> data;
    bool first_load = true;
    size_t ph_i = 0;
    for (size_t i = 0; i < segs_.size(); i++, ph_i++) {
        const Seg& s = segs_[i];
        u8* ph = phdrs.data() + ph_i * phentsize;
        WriteBE32(ph, kPtLoad);
        WriteBE32(ph + 4, s.flags);
        WriteBE64(ph + 8, data_off);
        WriteBE64(ph + 16, s.vaddr);
        // PS3: first PT_LOAD p_paddr carries the sceModuleInfo VA (0 = none)
        WriteBE64(ph + 24, first_load ? ps3_module_info_va_ : s.vaddr);
        WriteBE64(ph + 32, s.bytes.size());
        WriteBE64(ph + 40, s.mem_size);
        WriteBE64(ph + 48, 0x10000);  // align
        data.insert(data.end(), s.bytes.begin(), s.bytes.end());
        data_off += s.bytes.size();
        first_load = false;
    }
    if (with_proc) {
        u8* ph = phdrs.data() + ph_i * phentsize;
        WriteBE32(ph, kPtSceProcParam);
        WriteBE32(ph + 4, 4);  // PF_R
        WriteBE64(ph + 8, proc_param_off);
        WriteBE64(ph + 16, ps3_proc_param_va_);
        WriteBE64(ph + 24, 0);
        WriteBE64(ph + 32, 0x2C);
        WriteBE64(ph + 40, 0x2C);
        WriteBE64(ph + 48, 4);
    }
    std::vector<u8> out;
    out.reserve(ehdr.size() + phdrs.size() + data.size() + proc_data.size());
    out.insert(out.end(), ehdr.begin(), ehdr.end());
    out.insert(out.end(), phdrs.begin(), phdrs.end());
    out.insert(out.end(), data.begin(), data.end());
    out.insert(out.end(), proc_data.begin(), proc_data.end());
    return out;
}

}  // namespace anyps3
