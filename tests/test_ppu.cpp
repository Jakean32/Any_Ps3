// PPU decoder + interpreter tests. All machine code below is REAL output of the
// GNU PPC64 big-endian assembler (see fixtures/ppu_fixtures.h + fixtures/*.s) —
// the decoder table is validated against binutils ground truth instruction by
// instruction.
#include "framework.h"

#include <cstring>
#include <string>

#include "anyps3/cpu/ppu/decode.h"
#include "anyps3/cpu/ppu/state.h"
#include "anyps3/hle/lv2.h"
#include "anyps3/loader/elf.h"

#include "fixtures/ppu_fixtures.h"

using namespace anyps3;

// Map binutils mnemonics onto our Mn enum (variants collapse where our decoder
// tracks Rc/lk in separate fields).
static Mn ExpectedMn(const char* name) {
    struct N { const char* s; Mn mn; };
    static const N table[] = {
        {"nop", Mn::NOP},
        {"li", Mn::ADDI}, {"lis", Mn::ADDIS}, {"addi", Mn::ADDI}, {"addis", Mn::ADDIS},
        {"addic", Mn::ADDIC}, {"addic.", Mn::ADDIC_RC}, {"subfic", Mn::SUBFIC},
        {"mulli", Mn::MULLI},
        {"cmpwi", Mn::CMPI}, {"cmpdi", Mn::CMPI}, {"cmplwi", Mn::CMPLI}, {"cmpldi", Mn::CMPLI},
        {"ori", Mn::ORI}, {"oris", Mn::ORIS}, {"xori", Mn::XORI}, {"xoris", Mn::XORIS},
        {"andi.", Mn::ANDI_RC}, {"andis.", Mn::ANDIS_RC},
        {"add", Mn::ADD}, {"add.", Mn::ADD}, {"subf", Mn::SUBF}, {"subf.", Mn::SUBF},
        {"neg", Mn::NEG}, {"mullw", Mn::MULLW}, {"mulld", Mn::MULLD},
        {"divw", Mn::DIVW}, {"divwu", Mn::DIVWU}, {"divd", Mn::DIVD}, {"divdu", Mn::DIVDU},
        {"mulhw", Mn::MULHW}, {"mulhwu", Mn::MULHWU},
        {"or", Mn::OR}, {"or.", Mn::OR}, {"xor", Mn::XOR}, {"and", Mn::AND},
        {"nand", Mn::NAND}, {"nor", Mn::NOR}, {"andc", Mn::ANDC}, {"orc", Mn::ORC},
        {"eqv", Mn::EQV},
        {"extsb", Mn::EXTSB}, {"extsh", Mn::EXTSH}, {"extsw", Mn::EXTSW},
        {"cntlzw", Mn::CNTLZW}, {"cntlzd", Mn::CNTLZD},
        {"slw", Mn::SLW}, {"srw", Mn::SRW}, {"sraw", Mn::SRAW}, {"srawi", Mn::SRAWI},
        {"slwi", Mn::RLWINM},  // alias emitted by as
        {"srwi", Mn::RLWINM},
        {"sld", Mn::SLD}, {"srd", Mn::SRD}, {"srad", Mn::SRAD}, {"sradi", Mn::SRADI},
        {"cmpw", Mn::CMP}, {"cmpd", Mn::CMP}, {"cmplw", Mn::CMPL}, {"cmpld", Mn::CMPL},
        {"rlwinm", Mn::RLWINM}, {"rlwimi", Mn::RLWIMI}, {"rlwnm", Mn::RLWNM},
        {"rldicl", Mn::RLDICL}, {"rldicr", Mn::RLDICR}, {"rldic", Mn::RLDIC},
        {"rldimi", Mn::RLDIMI}, {"rldcl", Mn::RLDCL}, {"rldcr", Mn::RLDCR},
        {"rotldi", Mn::RLDICL},  // alias: rldicl with mb=0
        {"crand", Mn::CRAND}, {"crandc", Mn::CRANDC}, {"creqv", Mn::CREQV},
        {"crnand", Mn::CRNAND}, {"crnor", Mn::CRNOR}, {"cror", Mn::CROR},
        {"crorc", Mn::CRORC}, {"crxor", Mn::CRXOR},
        {"mfcr", Mn::MFCR}, {"mtcr", Mn::MTCRF}, {"mcrf", Mn::MCRF},
        {"mflr", Mn::MFSPR}, {"mtlr", Mn::MTSPR}, {"mfxer", Mn::MFSPR},
        {"mtxer", Mn::MTSPR}, {"mfctr", Mn::MFSPR}, {"mtctr", Mn::MTSPR},
        {"mfspr", Mn::MFSPR}, {"mtspr", Mn::MTSPR},
        {"lwz", Mn::LWZ}, {"lwzu", Mn::LWZU}, {"lbz", Mn::LBZ}, {"lbzu", Mn::LBZU},
        {"lhz", Mn::LHZ}, {"lhzu", Mn::LHZU}, {"lha", Mn::LHA}, {"lhau", Mn::LHAU},
        {"stw", Mn::STW}, {"stwu", Mn::STWU}, {"stb", Mn::STB}, {"stbu", Mn::STBU},
        {"sth", Mn::STH}, {"sthu", Mn::STHU},
        {"lwzx", Mn::LWZX}, {"lbzx", Mn::LBZX}, {"lhzx", Mn::LHZX}, {"lhax", Mn::LHAX},
        {"stwx", Mn::STWX}, {"stbx", Mn::STBX}, {"sthx", Mn::STHX}, {"lwax", Mn::LWAX},
        {"lfs", Mn::LFS}, {"lfd", Mn::LFD}, {"stfs", Mn::STFS}, {"stfd", Mn::STFD},
        {"ld", Mn::LD}, {"ldu", Mn::LDU}, {"lwa", Mn::LWA}, {"std", Mn::STD}, {"stdu", Mn::STDU},
        {"ldx", Mn::LDX}, {"ldux", Mn::LDUX}, {"stdx", Mn::STDX}, {"stdux", Mn::STDUX},
        {"lwbrx", Mn::LWBRX}, {"stwbrx", Mn::STWBRX},
        {"dcbz", Mn::DCBZ}, {"dcbst", Mn::DCBS}, {"dcbf", Mn::DCBF},
        {"dcbtct", Mn::DCBT}, {"dcbt", Mn::DCBT},
        {"eieio", Mn::EIEIO}, {"hwsync", Mn::SYNC}, {"lwsync", Mn::LWSYNC}, {"isync", Mn::ISYNC},
        {"b", Mn::B}, {"bl", Mn::BL}, {"ba", Mn::B}, {"bla", Mn::BL},
        {"beq", Mn::BC}, {"bne", Mn::BC}, {"blt", Mn::BC}, {"bgt", Mn::BC},
        {"bdz", Mn::BC}, {"bdnz", Mn::BC},
        {"bctr", Mn::BCTR}, {"bctrl", Mn::BCTRL}, {"blr", Mn::BCLR}, {"blrl", Mn::BCLRL},
        {"sc", Mn::SC},
        {"twu", Mn::TW}, {"twui", Mn::TWI}, {"tdu", Mn::TD}, {"tdui", Mn::TDI},
    };
    for (const auto& e : table) {
        if (std::strcmp(e.s, name) == 0) return e.mn;
    }
    return Mn::UNK;
}

TEST(ppu_decode_ground_truth_inventory) {
    int checked = 0;
    for (size_t i = 0; i < kPpuDecodeInventoryCount; i++) {
        const auto& row = kPpuDecodeInventory[i];
        PpuInstr in;
        bool dec = PpuDecode(row.word, 0x10000, &in);
        Mn expect = ExpectedMn(row.name);
        if (expect == Mn::UNK) {
            std::string msg = "no Mn mapping for binutils mnemonic '" + std::string(row.name) +
                              "'";
            anyps3test::ReportFail(__FILE__, __LINE__, msg);
            continue;
        }
        CHECK_TRUE(dec);
        CHECK_EQ(in.mn, expect);
        if (std::strcmp(row.name, "ba") == 0) CHECK_TRUE(in.aa);
        if (std::strcmp(row.name, "bla") == 0) CHECK_TRUE(in.aa && in.lk);
        checked++;
    }
    CHECK_TRUE(checked > 140);
}

// ---------------------------------------------------------------------------
// Interpreter fixtures
// ---------------------------------------------------------------------------

static void MapFixture(Vm* vm, const unsigned char* text, size_t tsize, u32 taddr,
                       const unsigned char* data, size_t dsize, u32 daddr) {
    vm->PageAlloc(taddr, (tsize + 0xFFFF) & ~0xFFFFULL);
    vm->WriteBytes(taddr, text, static_cast<u32>(tsize));
    vm->PageSetExec(taddr, true);
    if (dsize) {
        vm->PageAlloc(daddr, (dsize + 0xFFFF) & ~0xFFFFULL);
        vm->WriteBytes(daddr, data, static_cast<u32>(dsize));
    }
}

static int RunFixture(Vm* vm, Lv2* lv2, PpuContext* ctx, u32 entry) {
    ctx->vm = vm;
    ctx->lv2 = lv2;
    ctx->syscall_fn = [](PpuContext* c) {
        static_cast<Lv2*>(c->lv2)->Syscall(c);
    };
    ctx->pc = entry;
    ctx->gpr[1] = kMainStackTop - 0x100;
    vm->PageAlloc(kMainStackTop - kMainStackSize, kMainStackSize);
    return PpuRun(ctx);
}

TEST(ppu_interp_alu) {
    Vm vm;
    Lv2 lv2(vm);
    MapFixture(&vm, kalu_text, kalu_text_size, kalu_text_addr, kalu_data,
               sizeof(kalu_data), kalu_data_addr);
    PpuContext ctx;
    int code = RunFixture(&vm, &lv2, &ctx, kalu_text_addr);
    CHECK_TRUE(!ctx.error);
    if (ctx.error) return;
    CHECK_EQ(code, 7);
    CHECK_EQ(vm.Read32(0x11000), 123);          // addi
    CHECK_EQ(vm.Read32(0x11004), 30);           // mullw
    CHECK_EQ(vm.Read32(0x11008), 0);            // srawi 1>>1
    CHECK_EQ(vm.Read32(0x1100C), 15);           // srwi -1>>28
    CHECK_EQ(vm.Read32(0x11010), 256);          // slwi 16<<4
    CHECK_EQ(vm.Read32(0x11014), 0x00123456);   // sradi
    CHECK_EQ(vm.Read64(0x11018), 0x00007F0000000000ULL);  // rotldi 0x7F<<<40
    CHECK_EQ(vm.Read32(0x11020), static_cast<u32>(-0x33));  // subf
}

TEST(ppu_interp_branch) {
    Vm vm;
    Lv2 lv2(vm);
    MapFixture(&vm, kbranch_text, kbranch_text_size, kbranch_text_addr, kbranch_data,
               sizeof(kbranch_data), kbranch_data_addr);
    PpuContext ctx;
    int code = RunFixture(&vm, &lv2, &ctx, kbranch_text_addr);
    if (ctx.error) {
        anyps3test::ReportFail(__FILE__, __LINE__, "interp error: " + ctx.error_msg);
        return;
    }
    CHECK_EQ(code, 3);
    CHECK_EQ(vm.Read32(0x11000), 15);  // sum 5+4+3+2+1
    CHECK_EQ(vm.Read32(0x11004), 19);  // +4 via bdnz
}

TEST(ppu_full_pipeline_hello) {
    // The complete GNU-produced hello.elf: my ELF64 loader -> VM -> interpreter
    // -> LV2 HLE -> sys_tty_write + _sys_process_exit.
    ElfImage img;
    SelfInfo self_info;
    std::string err;
    CHECK_TRUE(LoadExecutable(std::vector<u8>(khello_elf, khello_elf + khello_elf_size),
                              "hello.elf", &img, &self_info, &err));
    CHECK_STR_EQ(err, "");
    CHECK_TRUE(self_info.status == SelfStatus::NotSelf);

    Vm vm;
    Lv2 lv2(vm);
    for (const auto& seg : img.segments) {
        CHECK_TRUE(vm.PageAlloc(static_cast<u32>(seg.vaddr),
                                (seg.mem_size + 0xFFFF) & ~0xFFFFULL));
        CHECK_TRUE(vm.WriteBytes(static_cast<u32>(seg.vaddr), seg.bytes.data(),
                                 static_cast<u32>(seg.bytes.size())));
    }
    PpuContext ctx;
    int code = RunFixture(&vm, &lv2, &ctx, static_cast<u32>(img.entry));
    CHECK_TRUE(!ctx.error);
    if (ctx.error) return;
    CHECK_EQ(code, 42);
    CHECK_STR_EQ(lv2.tty(), "Hello from Any_Ps3!\n");
    CHECK_TRUE(ctx.scount == 2);
}
