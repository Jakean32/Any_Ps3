// ELF64 / SELF loader tests, including a synthetic plaintext fSELF (PSL1GHT-style)
// and an "encrypted" SELF (meta != 0) which must be rejected with a clear error.
#include "framework.h"

#include <cstring>
#include <string>

#include "anyps3/loader/elf.h"

using namespace anyps3;

TEST(elf_loads_builder_output) {
    // PT_LOAD #1: code @ 0x10000, PT_LOAD #2: data @ 0x11000
    Elf64Builder b;
    b.SetEntry(0x10000);
    b.AddSegment(0x10000, 0x5 /*R|X*/, {0x60, 0x00, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00});
    b.AddSegment(0x11000, 0x6 /*R|W*/, {0xDE, 0xAD, 0xBE, 0xEF}, 0x1000 /* memsz > filesz */);
    std::vector<u8> bytes = b.Build();

    ElfImage img;
    SelfInfo self_info;
    std::string err;
    CHECK_TRUE(LoadExecutable(bytes, "test.elf", &img, &self_info, &err));
    CHECK_STR_EQ(err, "");
    CHECK_EQ(img.entry, 0x10000ULL);
    CHECK_EQ(img.base, 0x10000ULL);
    CHECK_EQ(img.segments.size(), 2);
    CHECK_EQ(img.segments[0].bytes.size(), 8);
    CHECK_EQ(img.segments[1].mem_size, 0x1000);
    CHECK_EQ(img.segments[1].file_size, 4);
    CHECK_EQ(img.segments[1].bytes[3], 0xEF);
    CHECK_TRUE(self_info.status == SelfStatus::NotSelf);
}

TEST(elf_rejects_garbage) {
    std::vector<u8> junk(128, 0x00);
    ElfImage img;
    std::string err;
    CHECK_TRUE(!LoadElf64(junk, &img, &err));
    CHECK_TRUE(!err.empty());
}

TEST(elf_rejects_little_endian) {
    // ELF64 LE magic would parse; simulate by patching EI_DATA
    Elf64Builder b;
    b.AddSegment(0x10000, 0x5, {1, 2, 3});
    std::vector<u8> bytes = b.Build();
    bytes[5] = 1;  // ELFDATA2LSB
    ElfImage img;
    std::string err;
    CHECK_TRUE(!LoadElf64(bytes, &img, &err));
}

// Builds a plaintext fSELF container (like PSL1GHT make_fself output):
//   [SceHeader 32B][ext_hdr 80B][app_info 32B][embedded ELF...]
// with se_meta = 0 (plaintext marker) and per-segment ext headers.
static std::vector<u8> BuildFself(const ElfImage& elf, u64 auth_id, u32 vendor) {
    std::vector<u8> out;
    auto push = [&out](const void* p, size_t n) {
        const u8* b = static_cast<const u8*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto push64 = [&out](u64 v) {
        u8 t[8]; WriteBE64(t, v); out.insert(out.end(), t, t + 8);
    };
    auto push32 = [&out](u32 v) {
        u8 t[4]; WriteBE32(t, v); out.insert(out.end(), t, t + 4);
    };

    // Rebuild embedded ELF via builder
    Elf64Builder b;
    b.SetEntry(elf.entry);
    for (const auto& s : elf.segments) {
        // keep vaddr/flags; file contents = segment bytes
        b.AddSegment(s.vaddr, s.flags, s.bytes, s.mem_size);
    }
    std::vector<u8> embedded = b.Build();
    const u64 hdr_size = 32;
    const u64 app_info_off = hdr_size + 80;
    const u64 ehdr_off = app_info_off + 32;
    const u64 phnum = elf.segments.size();
    const u64 phentsize = 56;
    const u64 phdr_off = ehdr_off + 64;
    const u64 seg_info_off = phdr_off + phnum * phentsize;
    u64 data_off = seg_info_off + phnum * 32;  // segment ext headers are 32 bytes

    // SceHeader
    push32(0x53434500);   // SCE\0
    push32(3);            // version
    u8 t2[2]; WriteBE16(t2, 0); push(t2, 2);  // flags
    WriteBE16(t2, 1); push(t2, 2);             // type = 1 (app)
    push32(0);            // meta offset == 0 -> plaintext fSELF
    push64(hdr_size);     // header size
    push64(0);            // file size (patched later)

    // ext_hdr (10 x u64)
    push64(1);            // version
    push64(app_info_off); // program identification hdr offset
    push64(ehdr_off);     // ELF header offset
    push64(phdr_off);     // phdr offset
    push64(0);            // shdr offset (none)
    push64(seg_info_off); // segment ext hdr offset
    push64(0);            // version hdr offset
    push64(0);            // supplemental offset
    push64(0);            // supplemental size
    push64(0);            // padding

    // app info
    push64(auth_id);
    push32(vendor);
    push32(8);   // program type (app)
    push64(0);   // sce version
    push64(0);   // padding

    // embedded ELF header + phdrs
    std::vector<u8> embedded_elf = embedded;  // full elf; we copy pieces
    push(embedded_elf.data(), 64);            // ehdr
    // patch embedded phnum phdrs: use original phoff entries, then rewrite offsets
    // For the test we simply copy the phdr table and rewrite p_offset per segment below.
    std::vector<u8> phdrs(embedded_elf.begin() + 64,
                          embedded_elf.begin() + 64 + static_cast<long>(phnum) * 56);
    std::vector<u8> bodies;
    u64 cur = data_off;
    for (u64 i = 0; i < phnum; i++) {
        u8* ph = phdrs.data() + i * 56;
        u64 filesz = ReadBE64(ph + 32);
        WriteBE64(ph + 8, cur);  // p_offset -> within-SELF offset
        (void)filesz;
        cur += ReadBE64(ph + 32);
    }
    push(phdrs.data(), phdrs.size());
    // segment ext headers
    for (u64 i = 0; i < phnum; i++) {
        const u8* ph = phdrs.data() + i * 56;
        u64 p_offset = ReadBE64(ph + 8);
        u64 filesz = ReadBE64(ph + 32);
        push64(p_offset);
        push64(filesz);
        push32(1);   // compression: plain
        push32(0);
        push64(0);   // encryption: none
    }
    // data
    push(embedded_elf.data() + 64 + static_cast<long>(phnum) * 56,
         embedded_elf.size() - 64 - static_cast<size_t>(phnum) * 56);
    return out;
}

TEST(self_loads_plaintext_fself) {
    ElfImage src;
    src.entry = 0x10000;
    ElfSegment code;
    code.vaddr = 0x10000;
    code.flags = 0x5;
    code.bytes = {0x38, 0x60, 0x00, 0x07, 0x38, 0x80, 0x00, 0x08};
    code.file_size = code.bytes.size();
    code.mem_size = code.bytes.size();
    src.segments.push_back(code);
    std::vector<u8> fself = BuildFself(src, 0x1010000000000001ULL, 1);

    SelfInfo info;
    ElfImage out;
    std::string err;
    CHECK_TRUE(LoadSelf(fself, &info, &out, &err));
    CHECK_TRUE(info.status == SelfStatus::PlaintextSelf);
    CHECK_EQ(info.app_auth_id, 0x1010000000000001ULL);
    CHECK_EQ(info.app_vendor_id, 1u);
    CHECK_EQ(out.entry, 0x10000);
    CHECK_EQ(out.segments.size(), 1);
    CHECK_EQ(out.segments[0].bytes.size(), 8);
    CHECK_EQ(out.segments[0].bytes[3], 0x07);
}

TEST(self_rejects_encrypted) {
    // meta != 0 -> encrypted SELF -> must be refused (no keys shipped)
    std::vector<u8> fself(512, 0);
    WriteBE32(fself.data(), 0x53434500);
    WriteBE32(fself.data() + 4, 3);
    WriteBE64(fself.data() + 16, 32);    // header size
    WriteBE32(fself.data() + 12, 0x100); // meta offset != 0
    SelfInfo info;
    ElfImage out;
    std::string err;
    CHECK_TRUE(!LoadSelf(fself, &info, &out, &err));
    CHECK_TRUE(info.status == SelfStatus::EncryptedSelf);
    CHECK_TRUE(err.find("keys") != std::string::npos);
}

TEST(executable_dispatches_self_vs_elf) {
    ElfImage src;
    src.entry = 0x10000;
    ElfSegment s;
    s.vaddr = 0x10000;
    s.flags = 0x5;
    s.bytes = {1, 2, 3, 4};
    s.file_size = 4;
    s.mem_size = 4;
    src.segments.push_back(s);
    std::vector<u8> fself = BuildFself(src, 1, 1);

    SelfInfo info;
    ElfImage out;
    std::string err;
    CHECK_TRUE(LoadExecutable(fself, "EBOOT.BIN", &out, &info, &err));
    CHECK_TRUE(info.status == SelfStatus::PlaintextSelf);
    CHECK_STR_EQ(out.name, "EBOOT.BIN");
}
