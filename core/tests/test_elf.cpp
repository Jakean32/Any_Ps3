// Tests: ELF64 loader, PPU/SPU decoders.
#include "harness.h"
#include "testdata.h"
#include "any_ps3/elf.h"
#include "any_ps3/ppu.h"
#include "any_ps3/spu.h"

namespace any_ps3 {

using testdata::put16be;
using testdata::ppu_li;
using testdata::ppu_li_r;
using testdata::ppu_lis;

static void elf_parses_hello(TestState& st) {
    auto f = testdata::make_hello_elf();
    try {
        ElfImage img = elf_parse(f.data(), f.size());
        AP_CHECK_EQ(img.entry, (uint64_t)0x10000);
        AP_CHECK_EQ(img.segments.size(), (size_t)1);
        AP_CHECK_EQ(img.segments[0].vaddr, (uint64_t)0x10000);
        AP_CHECK_EQ(img.segments[0].filesz, (uint64_t)0x2000);
        // no symtab -> entry function placeholder
        AP_CHECK_EQ(img.functions.size(), (size_t)1);
        AP_CHECK_EQ(img.functions[0].addr, (uint64_t)0x10000);
    } catch (const Error& e) { st.fail(e.what(), __FILE__, __LINE__); }
}

static void elf_memory_load_read_write(TestState& st) {
    auto f = testdata::make_hello_elf();
    ElfImage img;
    try { img = elf_parse(f.data(), f.size()); }
    catch (const Error& e) { st.fail(e.what(), __FILE__, __LINE__); return; }
    PpuMemory mem;
    AP_CHECK(mem.load(img, f.data(), f.size()));
    AP_CHECK_EQ(mem.total_mapped(), (uint64_t)0x2000);
    uint32_t word = 0;
    AP_CHECK(mem.read(0x10000, &word, 4)); // first insn: li r3,1 = 0x38600001 (BE in memory)
    uint8_t b[4];
    AP_CHECK(mem.read(0x10000, b, 4));
    AP_CHECK_EQ(b[0], 0x38); AP_CHECK_EQ(b[1], 0x60); AP_CHECK_EQ(b[2], 0x00); AP_CHECK_EQ(b[3], 0x01);
    AP_CHECK(mem.write(0x10004, b, 4));
    AP_CHECK(!mem.read(0x20000, b, 4));    // outside segments -> fault
    AP_CHECK(!mem.read(0x10000 - 1, b, 4)); // unaligned across segment edge -> fault
}

static void elf_symtab_functions(TestState& st) {
    auto f = testdata::make_symtab_elf();
    try {
        ElfImage img = elf_parse(f.data(), f.size());
        AP_CHECK(img.has_symtab);
        AP_CHECK_EQ(img.functions.size(), (size_t)2);
        AP_CHECK_EQ(img.functions[0].name, std::string("main"));
        AP_CHECK_EQ(img.functions[0].addr, (uint64_t)0x10000);
        AP_CHECK_EQ(img.functions[1].name, std::string("helper"));
        AP_CHECK_EQ(img.functions[1].addr, (uint64_t)0x10020);
    } catch (const Error& e) { st.fail(e.what(), __FILE__, __LINE__); }
}

static void elf_rejects_encrypted_self(TestState& st) {
    std::vector<uint8_t> self = {'S', 'E', 'L', 'F', 0, 0, 0, 0};
    AP_CHECK(looks_like_sony_encrypted(self.data(), self.size()));
    bool threw = false;
    std::string msg;
    try { elf_parse(self.data(), self.size()); }
    catch (const Error& e) { threw = true; msg = e.what(); }
    AP_CHECK(threw);
    AP_CHECK(msg.find("decryption") != std::string::npos);
    AP_CHECK(msg.find("keys") != std::string::npos);
}

static void elf_rejects_wrong_format(TestState& st) {
    auto f = testdata::make_hello_elf();
    // little-endian variant
    std::vector<uint8_t> le = f;
    le[5] = 1;
    bool threw = false;
    try { elf_parse(le.data(), le.size()); } catch (const Error&) { threw = true; }
    AP_CHECK(threw);
    // wrong machine
    std::vector<uint8_t> mach = f;
    put16be(mach, 18, 0x3E); // x86-64
    threw = false;
    try { elf_parse(mach.data(), mach.size()); } catch (const Error&) { threw = true; }
    AP_CHECK(threw);
}

// ---- PPU ----
static void ppu_decode_immediates(TestState& st) {
    PpuInsn i = ppu_decode(0, ppu_li(3, 1));            // li r3,1
    AP_CHECK(i.valid);
    AP_CHECK_EQ(i.m, PpuMnemonic::Addi);
    AP_CHECK(std::string(i.name) == "li");
    AP_CHECK_EQ(i.imm, 1);

    i = ppu_decode(0, ppu_lis(4, 1));                   // lis r4,1
    AP_CHECK_EQ(i.m, PpuMnemonic::Addis);
    AP_CHECK(std::string(i.name) == "lis");
    AP_CHECK_EQ(i.imm, 1);

    i = ppu_decode(0, 0x60000000);                      // ori r0,r0,0 = nop
    AP_CHECK(i.valid);
    AP_CHECK_EQ(i.m, PpuMnemonic::Nop);
    AP_CHECK(std::string(i.name) == "nop");

    i = ppu_decode(0, (24u << 26) | (5u << 21) | (6u << 16) | 0xBEEF); // ori r5,r6,0xBEEF
    AP_CHECK_EQ(i.m, PpuMnemonic::Ori);
    AP_CHECK_EQ(i.rd, 5); AP_CHECK_EQ(i.ra, 6);
    AP_CHECK_EQ(i.imm, 0xBEEF);

    // negative immediate sign-extension
    i = ppu_decode(0, (14u << 26) | (7u << 21) | (8u << 16) | 0x8000); // addi r7,r8,-32768
    AP_CHECK_EQ(i.imm, -32768);
}

static void ppu_decode_branches(TestState& st) {
    // b .+0x40  => 18<<26 | (LI=0x10)<<2
    PpuInsn i = ppu_decode(0x100, (18u << 26) | (0x10u << 2));
    AP_CHECK(i.valid);
    AP_CHECK_EQ(i.m, PpuMnemonic::B);
    AP_CHECK_EQ(i.imm, 0x40);
    // bl .-0x100  => LI = -0x40 (24-bit two's complement 0xFFFFC0), LK=1
    i = ppu_decode(0x200, (18u << 26) | 1u | ((0xFFFFC0u & 0xFFFFFFu) << 2));
    AP_CHECK_EQ(i.m, PpuMnemonic::Bl);
    AP_CHECK_EQ(i.imm, -0x100);
    // bclr (blr)
    i = ppu_decode(0, (19u << 26) | (0u << 21) | (0u << 16) | (16u << 1));
    AP_CHECK_EQ(i.m, PpuMnemonic::Bclr);
    // bctr
    i = ppu_decode(0, (19u << 26) | (0u << 21) | (0u << 16) | (528u << 1));
    AP_CHECK_EQ(i.m, PpuMnemonic::Bcctr);
    // sc
    i = ppu_decode(0, 0x44000002);
    AP_CHECK_EQ(i.m, PpuMnemonic::Sc);
}

static void ppu_decode_loads_stores(TestState& st) {
    PpuInsn i = ppu_decode(0, (32u << 26) | (3u << 21) | (4u << 16) | 8); // lwz r3,8(r4)
    AP_CHECK_EQ(i.m, PpuMnemonic::Lwz);
    AP_CHECK_EQ(i.imm, 8);
    i = ppu_decode(0, (58u << 26) | (3u << 21) | (4u << 16) | (8u << 2) | 0); // ld r3,8(r4)
    AP_CHECK_EQ(i.m, PpuMnemonic::Ld);
    AP_CHECK_EQ(i.imm, 8);
    i = ppu_decode(0, (62u << 26) | (3u << 21) | (4u << 16) | (8u << 2) | 0); // std r3,8(r4)
    AP_CHECK_EQ(i.m, PpuMnemonic::Std);
    AP_CHECK_EQ(i.imm, 8);
    // unknown 31-subform stays honest
    i = ppu_decode(0, (31u << 26) | 0x7FF);
    AP_CHECK(!i.valid);
    AP_CHECK_EQ(i.m, PpuMnemonic::Unknown);
}

// ---- SPU (encodings cross-checked against binutils spu-opc.c) ----
static void spu_decode_common_set(TestState& st) {
    AP_CHECK_EQ(spu_decode(0, spu_enc_stop()).m, SpuMnemonic::Stop);
    AP_CHECK_EQ(spu_decode(0, spu_enc_lnop()).m, SpuMnemonic::Lnop);
    AP_CHECK_EQ(spu_decode(0, spu_enc_nop()).m, SpuMnemonic::Nop);
    AP_CHECK_EQ(spu_decode(0, spu_enc_nop()).word, 0x40200000u); // binutils: nop opcode 0x201

    SpuInsn i = spu_decode(0, spu_enc_ila(5, 0x3ABCD));
    AP_CHECK_EQ(i.m, SpuMnemonic::Ila);
    AP_CHECK_EQ(i.rt, 5);
    AP_CHECK_EQ(i.u18, 0x3ABCDu);

    i = spu_decode(0, spu_enc_ai(3, 4, -7));
    AP_CHECK_EQ(i.m, SpuMnemonic::Ai);
    AP_CHECK_EQ(i.rt, 3); AP_CHECK_EQ(i.ra, 4);
    AP_CHECK_EQ(i.s10, -7);

    i = spu_decode(0, spu_enc_a(1, 2, 3));
    AP_CHECK_EQ(i.m, SpuMnemonic::A);
    AP_CHECK_EQ(i.rt, 1); AP_CHECK_EQ(i.ra, 2); AP_CHECK_EQ(i.rb, 3);

    i = spu_decode(0, spu_enc_lqd(6, 16, 10));
    AP_CHECK_EQ(i.m, SpuMnemonic::Lqd);
    AP_CHECK_EQ(i.s10, 16); AP_CHECK_EQ(i.ra, 10);

    i = spu_decode(0, spu_enc_stqd(6, -3, 10));
    AP_CHECK_EQ(i.m, SpuMnemonic::Stqd);
    AP_CHECK_EQ(i.s10, -3);

    i = spu_decode(0, spu_enc_br(-16));
    AP_CHECK_EQ(i.m, SpuMnemonic::Br);
    AP_CHECK_EQ(i.s16, -16);

    i = spu_decode(0, spu_enc_brsl(2, 44));
    AP_CHECK_EQ(i.m, SpuMnemonic::Brsl);
    AP_CHECK_EQ(i.rt, 2);
    AP_CHECK_EQ(i.s16, 44);
}

static void spu_decode_unknown(TestState& st) {
    // words whose opcode matches none of our (format-masked) entries decode
    // honestly as Unknown
    SpuInsn i = spu_decode(0, 0xFFFFFFFF);
    AP_CHECK(!i.valid);
    AP_CHECK_EQ(i.m, SpuMnemonic::Unknown);
    i = spu_decode(0, 0x00E00000); // opcode 0x007: not in our subset
    AP_CHECK(!i.valid);
    AP_CHECK_EQ(i.m, SpuMnemonic::Unknown);
}

static const TestCase kTests[] = {
    AP_TEST(elf_parses_hello),
    AP_TEST(elf_memory_load_read_write),
    AP_TEST(elf_symtab_functions),
    AP_TEST(elf_rejects_encrypted_self),
    AP_TEST(elf_rejects_wrong_format),
    AP_TEST(ppu_decode_immediates),
    AP_TEST(ppu_decode_branches),
    AP_TEST(ppu_decode_loads_stores),
    AP_TEST(spu_decode_common_set),
    AP_TEST(spu_decode_unknown),
};

const TestCase* elf_tests() { return kTests; }
size_t elf_tests_count() { return sizeof kTests / sizeof kTests[0]; }

} // namespace any_ps3
