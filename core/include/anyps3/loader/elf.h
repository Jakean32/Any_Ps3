// Any_Ps3 — ELF64 (PPC64/LV2) and SELF loaders.
#pragma once

#include "anyps3/types.h"

#include <string>
#include <vector>

namespace anyps3 {

struct Vm;

struct ElfSegment {
    u64 vaddr = 0;
    u64 file_size = 0;
    u64 mem_size = 0;
    u32 flags = 0;  // PF_R|PF_W|PF_X
    std::vector<u8> bytes;
};

// Raw program header (kept for PS3-specific segments the runtime needs).
struct Ps3Phdr {
    u32 type = 0;    // PT_LOAD(1) or PS3-specific (0x60000000 TLS-info, 0x60000002 proc param)
    u32 flags = 0;
    u64 offset = 0;
    u64 vaddr = 0;
    u64 paddr = 0;   // first PT_LOAD: virtual address of sceModuleInfo (PS3 trick)
    u64 filesz = 0;
    u64 memsz = 0;
    u64 align = 0;
};

// PS3 process parameter block (phdr type 0x60000002, magic 0x1b434cec).
struct Ps3ProcParam {
    bool present = false;
    u32 libent_start = 0, libent_end = 0;    // export tables
    u32 libstub_start = 0, libstub_end = 0;  // import tables (function of interest)
    u32 version = 0;
};

struct ElfImage {
    u64 entry = 0;
    u64 base = 0;  // lowest vaddr of PT_LOAD segments
    std::vector<ElfSegment> segments;
    std::vector<Ps3Phdr> phdrs;   // ALL program headers, file order
    u16 e_type = 0;
    // PS3 module facts (valid after LoadElf64/LoadSelf):
    u64 module_info_va = 0;  // first PT_LOAD p_paddr → sceModuleInfo (0 = none)
    Ps3ProcParam proc_param;
    u64 proc_param_va = 0;    // phdr 0x60000002 p_vaddr (parse post-map when 0x2C bytes not in-file)
    u64 tls_filesz = 0, tls_memsz = 0, tls_vaddr = 0;  // phdr 0x60000000
    std::string name;  // module/file name (diagnostics)
};

// Big-endian ELF64 loader. Accepts EM_PPC64 (21); OS-ABI is logged, not enforced.
bool LoadElf64(const u8* data, size_t size, ElfImage* out, std::string* err);
inline bool LoadElf64(const std::vector<u8>& data, ElfImage* out, std::string* err) {
    return LoadElf64(data.data(), data.size(), out, err);
}

// SELF (SCE ELF) container. Layout of the SCE header (u32 magic 'SCE\0' 0x53434500,
// u32 version, u16 flags, u16 type, u32 meta offset, u64 header size, u64 file size),
// followed by an 80-byte extension header at `header size`: version, app-info offset,
// ELF header offset, phdr offset, shdr offset, segment-info offset, version offset,
// supplemental offset/size. Each segment ext header: u64 offset, u64 size,
// u32 compression, u32 unknown, u64 encryption.
//
// Any_Ps3 supports **plaintext / fSELF** executables (meta == 0, typical for homebrew
// and debug builds). Encrypted retail executables cannot be decrypted because Any_Ps3
// ships no keys; loading them fails with a clear, user-actionable error.
enum class SelfStatus {
    NotSelf,        // plain ELF
    PlaintextSelf,  // fSELF, fully supported
    EncryptedSelf,  // rejected: would require user-supplied keys (not shipped)
    BadSelf,
};

struct SelfInfo {
    SelfStatus status = SelfStatus::NotSelf;
    u32 sce_version = 0;
    u64 app_auth_id = 0;
    u32 app_vendor_id = 0;
};

// Detect + parse SELF; on PlaintextSelf, extracts the embedded ELF into `elf_out`.
// On NotSelf, callers should use LoadElf64 directly.
bool LoadSelf(const u8* data, size_t size, SelfInfo* info, ElfImage* elf_out,
              std::string* err);
inline bool LoadSelf(const std::vector<u8>& data, SelfInfo* info, ElfImage* elf_out,
                     std::string* err) {
    return LoadSelf(data.data(), data.size(), info, elf_out, err);
}

// Unified executable entry point (SELF or ELF) used by the import pipeline and runtime.
bool LoadExecutable(const u8* data, size_t size, const std::string& name, ElfImage* out,
                    SelfInfo* self_info, std::string* err);
inline bool LoadExecutable(const std::vector<u8>& data, const std::string& name,
                           ElfImage* out, SelfInfo* self_info, std::string* err) {
    return LoadExecutable(data.data(), data.size(), name, out, self_info, err);
}

// PPU ELF constants
constexpr u16 kEmPpc64 = 21;
// PS3-specific e_type values (psdevwiki SELF/ELF):
constexpr u16 kEtSceExec = 0xFE00;        // static SCE executable (disc EBOOTs)
constexpr u16 kEtScePpuRelExec = 0xFFA4;  // relocatable PRX/SPRX
// PS3-specific phdr types:
constexpr u32 kPtSceTlsInfo = 0x60000000;      // TLS info (filesz/memsz of .tdata)
constexpr u32 kPtSceProcParam = 0x60000002;    // sys_proc_prx_param block
constexpr u32 kPtScePpuRela = 0x700000A4;      // PPURELA relocations (not yet applied)
constexpr u32 kProcParamMagic = 0x1B434CECu;

// PS3 sceModuleInfo (guest memory, at first PT_LOAD p_paddr). All big endian.
struct Ps3ModuleInfo {
    u16 modattr = 0;
    u8 modver[2] = {0, 0};
    char modname[28] = {0};   // 27 chars + infover byte (kept raw here)
    u32 gp = 0;
    u32 libent_top = 0, libent_btm = 0;
    u32 libstub_top = 0, libstub_btm = 0;
};
bool ParsePs3ModuleInfo(const u8* guest_mem, u32 module_info_va, Ps3ModuleInfo* out);

// Guest-side sys_proc_prx_param parse (after segments are mapped; used for
// fSELF images where the block is inside a mapped segment).
bool ParseProcParamGuest(Vm& vm, u32 va, Ps3ProcParam* out);

// Import descriptor for one imported function (resolved against HLE modules).
struct ModuleImport {
    std::string module_name;  // e.g. "sysPrxForUser", "cellSysutil"
    u32 nid = 0;
    u32 slot_va = 0;          // u32 slot the loader patches with a descriptor addr
    u32 use_va = 0;           // optional secondary use-address (attribute & 0x2000)
};

// Walk the module's import (libstub) tables in mapped guest memory. Returns
// false on structurally broken data. Fills `imports` and the module name.
bool ReadModuleImports(Vm& vm, const ElfImage& elf, std::vector<ModuleImport>* imports,
                       std::string* module_name, std::string* err);

// ELF64 header writer/builder used by tests and tools (mkhomebrew).
class Elf64Builder {
public:
    Elf64Builder();
    void AddSegment(u64 vaddr, u32 flags, std::vector<u8> bytes, u64 mem_size = 0);
    void SetEntry(u64 entry) { entry_ = entry; }
    // PS3 module facts for fixtures: first PT_LOAD p_paddr → module_info VA,
    // plus a 0x60000002 proc-param phdr pointing at `proc_param_va`.
    void SetPs3ModuleFacts(u64 module_info_va, u64 proc_param_va, u16 e_type) {
        ps3_module_info_va_ = module_info_va;
        ps3_proc_param_va_ = proc_param_va;
        e_type_ = e_type;
    }
    // Content of the 0x2C-byte proc-param block emitted into the file.
    void SetPs3ProcParamContent(u32 libent_start, u32 libent_end,
                                u32 libstub_start, u32 libstub_end) {
        proc_libent_start_ = libent_start;
        proc_libent_end_ = libent_end;
        proc_libstub_start_ = libstub_start;
        proc_libstub_end_ = libstub_end;
    }
    std::vector<u8> Build() const;

private:
    u64 entry_ = 0x10000;
    u64 ps3_module_info_va_ = 0;
    u64 ps3_proc_param_va_ = 0;
    u16 e_type_ = 2;
    u32 proc_libent_start_ = 0, proc_libent_end_ = 0;
    u32 proc_libstub_start_ = 0, proc_libstub_end_ = 0;
    struct Seg {
        u64 vaddr;
        u32 flags;
        std::vector<u8> bytes;
        u64 mem_size;
    };
    std::vector<Seg> segs_;
};

}  // namespace anyps3
