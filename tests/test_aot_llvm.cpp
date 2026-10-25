// LLVM AOT backend tests — only meaningful (and only registered) when the
// build links LLVM (-DANYPS3_LLVM=ON). These prove the TASK-2 milestone-1
// contract end to end: PPC64 (binutils-assembled) -> AOT-IR -> LLVM IR ->
//   * native execution through ORC (semantics identical to the interpreter,
//     zero interpreter fallbacks for fully-liftable programs), and
//   * real AOT artifacts: .ll text + x86-64 and aarch64 object files,
//     verified with llvm-objdump when available on the PATH.
#ifdef ANYPS3_WITH_LLVM

#include "framework.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "anyps3/cpu/ppu/aot.h"
#include "anyps3/cpu/ppu/ir.h"
#include "anyps3/hle/lv2.h"

#include "fixtures/ppu_fixtures.h"

using namespace anyps3;

// A tiny LLVM-block runner: lift + compile per pc, run until guest exit.
// Falls back to the interpreter for blocks the lifter refuses (mirrors
// mini_aot::RunAot's contract).
struct LlvmRunner {
    Vm* vm;
    Lv2* lv2;
    std::unordered_map<u32, mini_aot::BlockFn> cache;
    std::unordered_map<u32, bool> fallbacks;
    bool llvm_ok = false;

    int Run(PpuContext* ctx) {
        ctx->vm = vm;
        ctx->lv2 = lv2;
        ctx->syscall_fn = [](PpuContext* c) { static_cast<Lv2*>(c->lv2)->Syscall(c); };
        ctx->mem_base = vm->mutable_data();
        vm->PageAlloc(kMainStackTop - kMainStackSize, kMainStackSize);
        u64 budget = 200000000ULL;
        while (!ctx->exit && !ctx->error) {
            if (budget-- == 0) {
                PpuRaise(ctx, "LLVM runner: budget exceeded");
                break;
            }
            auto it = cache.find(ctx->pc);
            mini_aot::BlockFn fn = nullptr;
            if (it != cache.end()) {
                fn = it->second;
            } else {
                auto fb = fallbacks.find(ctx->pc);
                if (fb != fallbacks.end()) {
                    fn = nullptr;
                } else {
                    std::vector<AIROp> ir;
                    if (PpuLiftBlock(*vm, ctx->pc, 64, &ir)) {
                        fn = mini_aot::CompileLlvmBlock(ctx->pc, ir);
                    }
                    if (fn) {
                        cache[ctx->pc] = fn;
                    } else {
                        fallbacks[ctx->pc] = true;
                    }
                }
            }
            if (fn) {
                fn(ctx);
            } else {
                if (!PpuStep(ctx)) break;
            }
        }
        return static_cast<int>(ctx->exit_code);
    }
};

static void MapFixture(Vm* vm, const unsigned char* text, size_t tsize, u32 taddr,
                       const unsigned char* data, size_t dsize, u32 daddr) {
    vm->PageAlloc(taddr, (tsize + 0xFFFF) & ~0xFFFFULL);
    vm->WriteBytes(taddr, text, tsize);
    vm->PageAlloc(daddr, 0x10000);
    if (dsize) vm->WriteBytes(daddr, data, dsize);
}

TEST(llvm_backend_available) {
    CHECK_TRUE(mini_aot::LlvmAvailable());
}

TEST(llvm_hello_runs_natively) {
    Vm vm;
    Lv2 lv2(vm);
    MapFixture(&vm, khello_text, khello_text_size, khello_text_addr, khello_data,
               sizeof(khello_data), khello_data_addr);
    LlvmRunner r{&vm, &lv2};
    PpuContext ctx;
    ctx.pc = khello_text_addr;
    ctx.gpr[1] = kMainStackTop - 0x100;
    int code = r.Run(&ctx);
    CHECK_TRUE(!ctx.error);
    CHECK_EQ(code, 42);
    CHECK_STR_EQ(lv2.tty(), "Hello from Any_Ps3!\n");
    CHECK_EQ(ctx.icount, 0);  // 100% native — no interpreter step
    CHECK_TRUE(r.cache.size() >= 1);
}

TEST(llvm_branch_loops_run_natively) {
    Vm vm;
    Lv2 lv2(vm);
    MapFixture(&vm, kbranch_text, kbranch_text_size, kbranch_text_addr, kbranch_data,
               sizeof(kbranch_data), kbranch_data_addr);
    LlvmRunner r{&vm, &lv2};
    PpuContext ctx;
    ctx.pc = kbranch_text_addr;
    ctx.gpr[1] = kMainStackTop - 0x100;
    int code = r.Run(&ctx);
    CHECK_TRUE(!ctx.error);
    CHECK_EQ(code, 3);
    CHECK_EQ(vm.Read32(0x11000), 15);
    CHECK_EQ(vm.Read32(0x11004), 19);
    CHECK_EQ(ctx.icount, 0);  // fully native
    CHECK_TRUE(r.cache.size() >= 3);
}

TEST(llvm_alu_mixed_mode_fallback) {
    // The alu fixture contains unlift-able ops: the lifter refuses, the runner
    // falls back to the interpreter, and the results match the interpreter.
    Vm vm;
    Lv2 lv2(vm);
    MapFixture(&vm, kalu_text, kalu_text_size, kalu_text_addr, kalu_data,
               sizeof(kalu_data), kalu_data_addr);
    LlvmRunner r{&vm, &lv2};
    PpuContext ctx;
    ctx.pc = kalu_text_addr;
    ctx.gpr[1] = kMainStackTop - 0x100;
    int code = r.Run(&ctx);
    CHECK_TRUE(!ctx.error);
    CHECK_EQ(code, 7);
    CHECK_EQ(vm.Read32(0x11000), 123);
    CHECK_EQ(vm.Read32(0x11004), 30);
    CHECK_EQ(vm.Read32(0x11008), 0);
    CHECK_EQ(vm.Read32(0x1100C), 15);
    CHECK_EQ(vm.Read32(0x11010), 256);
    CHECK_TRUE(ctx.icount > 0);  // interpreter fallback happened
    CHECK_TRUE(r.fallbacks.size() >= 1);
}

TEST(llvm_emit_aot_artifacts) {
    // Lift all liftable blocks of hello + branch, emit .ll + x86-64 and
    // aarch64 objects, verify the files and (if llvm-objdump is present)
    // that the objects really contain the block symbols.
    Vm vm;
    MapFixture(&vm, khello_text, khello_text_size, khello_text_addr, khello_data,
               sizeof(khello_data), khello_data_addr);
    MapFixture(&vm, kbranch_text, kbranch_text_size, kbranch_text_addr, kbranch_data,
               sizeof(kbranch_data), kbranch_data_addr);

    std::vector<std::pair<u32, std::vector<AIROp>>> owned;
    for (u32 pc : {khello_text_addr, kbranch_text_addr, 0x10008u, 0x10014u, 0x10028u}) {
        std::vector<AIROp> ir;
        if (PpuLiftBlock(vm, pc, 64, &ir)) owned.emplace_back(pc, std::move(ir));
    }
    CHECK_TRUE(owned.size() >= 2);

    std::vector<std::pair<u32, const std::vector<AIROp>*>> blocks;
    for (auto& [pc, ir] : owned) blocks.emplace_back(pc, &ir);

    const char* dir = "/tmp/anyps3_llvm_aot_test";
    std::remove("/tmp/anyps3_llvm_aot_test/anyps3_test.ll");
    std::remove("/tmp/anyps3_llvm_aot_test/anyps3_test.x64.o");
    std::remove("/tmp/anyps3_llvm_aot_test/anyps3_test.arm64.o");
    std::system("mkdir -p /tmp/anyps3_llvm_aot_test");

    mini_aot::LlvmAotArtifacts art;
    bool ok = mini_aot::LlvmEmitAot("anyps3_test", blocks, dir, &art);
    CHECK_TRUE(ok);
    CHECK_TRUE(art.blocks_built >= 2);

    // .ll text: contains per-block functions
    CHECK_TRUE(!art.ll_path.empty());
    {
        FILE* f = std::fopen(art.ll_path.c_str(), "rb");
        CHECK_TRUE(f != nullptr);
        if (f) {
            std::string text;
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
            CHECK_TRUE(text.find("define void @ap3_") != std::string::npos);
        }
    }

    // objects: non-empty for BOTH targets
    auto size_of = [](const std::string& p) -> long {
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) return -1;
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fclose(f);
        return n;
    };
    CHECK_TRUE(size_of(art.x64_obj_path) > 0);
    CHECK_TRUE(size_of(art.arm64_obj_path) > 0);

    // verify with llvm-objdump when available (the emitted code is real)
    const char* objdump_candidates[] = {
        "/tmp/tools/llvm/usr/bin/llvm-objdump-19", "llvm-objdump", nullptr};
    for (int i = 0; objdump_candidates[i]; i++) {
        std::string cmd = std::string(objdump_candidates[i]) + " -t " +
                          art.x64_obj_path + " > /tmp/anyps3_x64_syms.txt 2>&1";
        if (std::system(cmd.c_str()) == 0) {
            FILE* f = std::fopen("/tmp/anyps3_x64_syms.txt", "rb");
            if (f) {
                std::string text;
                char buf[4096];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
                std::fclose(f);
                CHECK_TRUE(text.find("ap3_") != std::string::npos);
            }
            break;
        }
    }
}

TEST(llvm_fault_on_bad_memory_access) {
    Vm vm;
    Lv2 lv2(vm);
    const u32 code[] = {0x3D802000, 0x38600001, 0x90640000, 0x39600016,
                        0x38600005, 0x44000002, 0x48000000};
    vm.PageAlloc(0x10000, 0x10000);
    u8 buf[32];
    for (size_t i = 0; i < 7; i++) WriteBE32(buf + i * 4, code[i]);
    vm.WriteBytes(0x10000, buf, 28);
    LlvmRunner r{&vm, &lv2};
    PpuContext ctx;
    ctx.pc = 0x10000;
    ctx.gpr[1] = kMainStackTop - 0x100;
    r.Run(&ctx);
    CHECK_TRUE(ctx.error);  // native store faulted cleanly
    CHECK_EQ(ctx.error_pc, 0x10008);
}

#endif  // ANYPS3_WITH_LLVM
