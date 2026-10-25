// Any_Ps3 — ELF64 loader for PPU executables (big-endian PowerPC64).
// Encrypted Sony containers (SELF/SCE) are DETECTED and honestly rejected:
// Any_Ps3 does not ship decryption keys — users must provide their own
// decrypted ELF (e.g. from homebrew or their own legally dumped, decrypted
// executables).
#pragma once

#include "types.h"

namespace any_ps3 {

struct ElfSegment {
    uint32_t type = 0;
    uint32_t flags = 0;
    uint64_t file_off = 0;
    uint64_t vaddr = 0;
    uint64_t filesz = 0;
    uint64_t memsz = 0;
};

struct ElfFunction {
    std::string name;
    uint64_t addr = 0;
    uint64_t size = 0;
};

struct ElfImage {
    uint16_t e_type = 0;
    uint16_t machine = 0;
    uint64_t entry = 0;
    std::vector<ElfSegment> segments;
    std::vector<ElfFunction> functions; // from .symtab when present; else entry only

    bool has_symtab = false;
};

// Throws any_ps3::Error with an honest reason on rejection.
ElfImage elf_parse(const uint8_t* data, size_t len);

// Heuristic detection of encrypted Sony executables (SELF and friends).
bool looks_like_sony_encrypted(const uint8_t* data, size_t len);
std::string sony_encrypted_message();

// Sparse guest memory (PPU 64-bit address space) used by the fallback
// interpreter. Only mapped segments are accessible; everything else faults.
class PpuMemory {
public:
    struct Segment {
        uint64_t vaddr, memsz;
        std::vector<uint8_t> data;
    };

    bool load(const ElfImage& img, const uint8_t* file, size_t filen);

    bool read(uint64_t addr, void* out, size_t len) const;
    bool write(uint64_t addr, const void* in, size_t len);
    uint8_t* mutable_bytes(uint64_t addr, size_t len);

    uint64_t total_mapped() const;

private:
    std::vector<Segment> segs_;
};

} // namespace any_ps3
