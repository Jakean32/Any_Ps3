// AOT backend tests: the same GNU-assembled fixtures, but executed through the
// MiniAOT x86-64 backend (PPC64 -> IR -> native code). The hello and branch
// fixtures lift 100% natively; the alu fixture contains unlift-able ops
// (srawi/rldicl) and exercises the interpreter-fallback path.
#include "framework.h"

#include <string>

#include "anyps3/cpu/ppu/aot.h"
#include "anyps3/cpu/ppu/ir.h"
#include "anyps3/hle/lv2.h"

#include "fixtures/ppu_fixtures.h"

using namespace anyps3;

static int RunAotFixture(Vm* vm, Lv2* lv2, mini_aot::BlockCache* cache, PpuContext* ctx,
                        u32 entry) {
    ctx->vm = vm;
    ctx->lv2 = lv2;
    ctx->syscall_fn = [](PpuContext* c) { static_cast<Lv2*>(c->lv2)->Syscall(c); };
    ctx->pc = entry;
    ctx->gpr[1] = kMainStackTop - 0x100;
    ctx->mem_base = vm->mutable_data();
    vm->PageAlloc(kMainStackTop - kMainStackSize, kMainStackSize);
    return mini_aot::RunAot(ctx, *cache);
}

TEST(aot_lifts_and_compiles_hello_block) {
    // The hello fixture is one basic block (0x10000..0x10028).
    Vm vm;
    vm.PageAlloc(khello_text_addr, (khello_text_size + 0xFFFF) & ~0xFFFFULL);
    vm.WriteBytes(khello_text_addr, khello_text, khello_text_size);
    vm.PageAlloc(khello_data_addr, 0x10000);
    std::vector<AIROp> ir;
    CHECK_TRUE(PpuLiftBlock(vm, 0x10000, 128, &ir));
    CHECK_TRUE(ir.size() >= 11);  // 10 GprImm/AddImm ops + Syscall(s) + branch
    auto code = mini_aot::Compile(ir);
    CHECK_TRUE(code.fn != nullptr);
    CHECK_TRUE(code.emitted > 32);
    mini_aot::Release(&code);
}

TEST(aot_hello_runs_natively) {
    Vm vm;
    Lv2 lv2(vm);
    vm.PageAlloc(khello_text_addr, (khello_text_size + 0xFFFF) & ~0xFFFFULL);
    vm.WriteBytes(khello_text_addr, khello_text, khello_text_size);
    vm.PageAlloc(khello_data_addr, 0x10000);
    vm.WriteBytes(khello_data_addr, khello_data, sizeof(khello_data));
    mini_aot::BlockCache cache(vm);
    PpuContext ctx;
    int code = RunAotFixture(&vm, &lv2, &cache, &ctx, khello_text_addr);
    CHECK_TRUE(!ctx.error);
    CHECK_EQ(code, 42);
    CHECK_STR_EQ(lv2.tty(), "Hello from Any_Ps3!\n");
    // Full native execution: at least one compiled block, zero interpreter steps.
    CHECK_TRUE(cache.block_count() >= 1);
    CHECK_EQ(cache.fallback_count(), 0);
    CHECK_EQ(ctx.icount, 0);  // no interpreter instruction was executed
}

TEST(aot_branch_loops_run_natively) {
    // Conditional loop (cmpwi/bgt) and a CTR loop (mtctr/bdnz), all lifted.
    Vm vm;
    Lv2 lv2(vm);
    vm.PageAlloc(kbranch_text_addr, (kbranch_text_size + 0xFFFF) & ~0xFFFFULL);
    vm.WriteBytes(kbranch_text_addr, kbranch_text, kbranch_text_size);
    vm.PageAlloc(kbranch_data_addr, 0x10000);
    mini_aot::BlockCache cache(vm);
    PpuContext ctx;
    int code = RunAotFixture(&vm, &lv2, &cache, &ctx, kbranch_text_addr);
    if (ctx.error) {
        anyps3test::ReportFail(__FILE__, __LINE__, "aot error: " + ctx.error_msg);
        return;
    }
    CHECK_EQ(code, 3);
    CHECK_EQ(vm.Read32(0x11000), 15);   // cmpwi/bgt loop
    CHECK_EQ(vm.Read32(0x11004), 19);   // mtctr/bdnz loop
    CHECK_TRUE(cache.block_count() >= 3);
    CHECK_EQ(cache.fallback_count(), 0);
    CHECK_EQ(ctx.icount, 0);  // fully native
}

TEST(aot_alu_runs_mixed_mode) {
    // The alu fixture contains srawi/rldicl which the lifter refuses: the whole
    // block falls back to the interpreter. Results must match the interpreter
    // test exactly — this validates the fallback path end-to-end.
    Vm vm;
    Lv2 lv2(vm);
    vm.PageAlloc(kalu_text_addr, (kalu_text_size + 0xFFFF) & ~0xFFFFULL);
    vm.WriteBytes(kalu_text_addr, kalu_text, kalu_text_size);
    vm.PageAlloc(kalu_data_addr, 0x10000);
    mini_aot::BlockCache cache(vm);
    PpuContext ctx;
    int code = RunAotFixture(&vm, &lv2, &cache, &ctx, kalu_text_addr);
    if (ctx.error) {
        anyps3test::ReportFail(__FILE__, __LINE__, "aot error: " + ctx.error_msg);
        return;
    }
    CHECK_EQ(code, 7);
    CHECK_EQ(vm.Read32(0x11000), 123);
    CHECK_EQ(vm.Read32(0x11004), 30);
    CHECK_EQ(vm.Read32(0x11008), 0);
    CHECK_EQ(vm.Read32(0x1100C), 15);
    CHECK_EQ(vm.Read32(0x11010), 256);
    CHECK_EQ(vm.Read32(0x11014), 0x00123456);
    CHECK_EQ(vm.Read64(0x11018), 0x00007F0000000000ULL);
    CHECK_EQ(vm.Read32(0x11020), static_cast<u32>(-0x33));
    // The containing block is known-unlift-able -> interpreter fallback used.
    CHECK_TRUE(cache.fallback_count() >= 1);
    CHECK_TRUE(ctx.icount > 0);
}

TEST(aot_fault_on_bad_memory_access) {
    // A compiled store to an unmapped guest address must set ctx->error, not
    // corrupt host memory. Program: lis r4, 0x2000 (addr 0x20000000, out of
    // guest window); stw r3, 0(r4); then exit.
    Vm vm;
    Lv2 lv2(vm);
    // clang-format off
    // lis r4, 0x2000     -> 0x3D802000   (r4 = 0x20000000)
    // li r3, 1           -> 0x38600001
    // stw r3, 0(r4)      -> 0x90640000
    // li r11, 22         -> 0x39600016
    // li r3, 5           -> 0x38600005
    // sc                 -> 0x44000002
    // b .                -> 0x48000000
    // clang-format on
    const u32 code[] = {0x3D802000, 0x38600001, 0x90640000, 0x39600016,
                        0x38600005, 0x44000002, 0x48000000};
    vm.PageAlloc(0x10000, 0x10000);
    u8 buf[32];
    for (size_t i = 0; i < 7; i++) WriteBE32(buf + i * 4, code[i]);
    vm.WriteBytes(0x10000, buf, 28);
    mini_aot::BlockCache cache(vm);
    PpuContext ctx;
    ctx.vm = &vm;
    ctx.lv2 = &lv2;
    ctx.syscall_fn = [](PpuContext* c) { static_cast<Lv2*>(c->lv2)->Syscall(c); };
    ctx.pc = 0x10000;
    ctx.gpr[1] = kMainStackTop - 0x100;
    ctx.mem_base = vm.mutable_data();
    vm.PageAlloc(kMainStackTop - kMainStackSize, kMainStackSize);
    mini_aot::RunAot(&ctx, cache);
    CHECK_TRUE(ctx.error);  // the store faulted cleanly
    CHECK_EQ(ctx.error_pc, 0x10008);
}
