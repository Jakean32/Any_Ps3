// Any_Ps3 — ELF64 (PPC64/LV2) and SELF loaders.
#pragma once

#include "anyps3/types.h"

#include <string>
#include <vector>

namespace anyps3 {

struct ElfSegment {
    u64 vaddr = 0;
    u64 file_size = 0;
    u64 mem_size = 0;
    u32 flags = 0;  // PF_R|PF_W|PF_X
    std::vector<u8> bytes;
};

struct ElfImage {
    u64 entry = 0;
    u64 base = 0;  // lowest vaddr of PT_LOAD segments
    std::vector<ElfSegment> segments;
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

// ELF64 header writer/builder used by tests and tools (mkhomebrew).
class Elf64Builder {
public:
    Elf64Builder();
    void AddSegment(u64 vaddr, u32 flags, std::vector<u8> bytes, u64 mem_size = 0);
    void SetEntry(u64 entry) { entry_ = entry; }
    std::vector<u8> Build() const;

private:
    u64 entry_ = 0x10000;
    struct Seg {
        u64 vaddr;
        u32 flags;
        std::vector<u8> bytes;
        u64 mem_size;
    };
    std::vector<Seg> segs_;
};

}  // namespace anyps3
