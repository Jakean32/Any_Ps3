// TASK-2 milestone 1 tests: PPU floating point, VMX subset and reservation
// semantics, executed on the interpreter against GNU-binutils-assembled
// fixtures (tests/fixtures/ppu_fixtures.h; regenerate scripts/regen_ppu_fixtures.sh).
#include "framework.h"

#include <cmath>
#include <cstring>

#include "anyps3/cpu/ppu/state.h"
#include "anyps3/hle/lv2.h"
#include "anyps3/runtime/threads.h"

#include "fixtures/ppu_fixtures.h"

using namespace anyps3;

static void MapFixture(Vm* vm, const unsigned char* text, size_t tsize, u32 taddr,
                       const unsigned char* data, size_t dsize, u32 daddr) {
    vm->PageAlloc(taddr, (tsize + 0xFFFF) & ~0xFFFFULL);
    vm->WriteBytes(taddr, text, tsize);
    vm->PageAlloc(daddr, 0x10000);
    if (dsize) vm->WriteBytes(daddr, data, dsize);
    vm->PageAlloc(kMainStackTop - kMainStackSize, kMainStackSize);
}

static int RunFixture(Vm* vm, Lv2* lv2, PpuContext* ctx, u32 entry) {
    ctx->vm = vm;
    ctx->lv2 = lv2;
    ctx->syscall_fn = [](PpuContext* c) { static_cast<Lv2*>(c->lv2)->Syscall(c); };
    ctx->pc = entry;
    ctx->gpr[1] = kMainStackTop - 0x100;
    ctx->mem_base = vm->mutable_data();
    return PpuRun(ctx);
}

static double D(u64 bits) {
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}
static u64 Bits(double d) {
    u64 b;
    std::memcpy(&b, &d, 8);
    return b;
}
static float F(u64 bits) {  // low 32 = single bits? No: stored via stfs (already single)
    u32 x = static_cast<u32>(bits);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

// ---------------------------------------------------------------------------
// FP fixture: exact results the assembled program must produce.
// ---------------------------------------------------------------------------
TEST(ppu_interp_fp) {
    Vm vm;
    Lv2 lv2(vm);
    MapFixture(&vm, kfp_text, kfp_text_size, kfp_text_addr, kfp_data, sizeof(kfp_data),
               kfp_data_addr);
    PpuContext ctx;
    int code = RunFixture(&vm, &lv2, &ctx, kfp_text_addr);
    CHECK_TRUE(!ctx.error);
    if (ctx.error) {
        anyps3test::ReportFail(__FILE__, __LINE__, "fp: " + ctx.error_msg);
        return;
    }
    CHECK_EQ(code, 7);

    const double a = 3.5, b = 2.25, c = 1.25;
    CHECK_EQ(vm.Read64(0x11000), Bits(a + b));               // fadd
    CHECK_EQ(vm.Read64(0x11008), Bits(a - b));               // fsub
    CHECK_EQ(vm.Read64(0x11010), Bits(a * b));               // fmul
    CHECK_EQ(vm.Read64(0x11018), Bits(a / b));               // fdiv
    CHECK_EQ(vm.Read64(0x11020), Bits(std::fma(a, b, c)));   // fmadd (fused)
    CHECK_EQ(vm.Read64(0x11028), Bits(std::fma(a, b, -c)));   // fmsub
    CHECK_EQ(vm.Read64(0x11030), Bits(-std::fma(a, b, c)));   // fnmadd
    CHECK_EQ(vm.Read64(0x11038), Bits(-std::fma(a, b, -c)));   // fnmsub
    CHECK_EQ(vm.Read64(0x11040), Bits(-a * b));              // fneg
    CHECK_EQ(vm.Read64(0x11048), Bits(a * b));               // fabs
    CHECK_EQ(vm.Read64(0x11050), Bits(-(a * b)));            // fnabs
    CHECK_EQ(vm.Read64(0x11058), Bits(a + b));               // fmr
    CHECK_EQ(vm.Read64(0x11060), Bits(std::sqrt(a * b)));    // fsqrt
    CHECK_EQ(D(vm.Read64(0x11068)), static_cast<float>(a / b));  // frsp
    CHECK_EQ(static_cast<u32>(vm.Read64(0x11070)), 4u);      // fctiw(3.5)
    CHECK_EQ(static_cast<u32>(vm.Read64(0x11078)), 3u);      // fctiwz(3.5)

    u32 cr = vm.Read32(0x11080);
    u32 cr0 = (cr >> 28) & 0xF, cr1 = (cr >> 24) & 0xF, cr2 = (cr >> 20) & 0xF,
        cr3 = (cr >> 16) & 0xF;
    CHECK_EQ(cr0, 4u);  // fcmpu 3.5 > 2.25 -> GT
    CHECK_EQ(cr1, 4u);  // fcmpo
    CHECK_EQ(cr2, 2u);  // fcmpu equal -> EQ
    CHECK_EQ(cr3, 4u);  // fcmpu 3.5 > -7.875

    // single-precision ops (stored via stfs -> single bits in the low word).
    // Expected values computed with the same host float semantics.
    auto fbits = [](float f) { u32 x; std::memcpy(&x, &f, 4); return x; };
    CHECK_EQ(vm.Read32(0x11084), fbits(3.5f + 2.25f));   // fadds
    CHECK_EQ(vm.Read32(0x11088), fbits(3.5f - 2.25f));   // fsubs
    CHECK_EQ(vm.Read32(0x1108C), fbits(3.5f * 2.25f));   // fmuls
    {  // fdivs: single bits in the low word
        u32 w = vm.Read32(0x11090);
        float f;
        std::memcpy(&f, &w, 4);
        CHECK_EQ(f, static_cast<float>(a / b));
    }
    CHECK_EQ(vm.Read32(0x11094), fbits(std::fma(3.5f, 2.25f, 1.25f)));   // fmadds
    CHECK_EQ(vm.Read32(0x11098), fbits(std::fma(3.5f, 2.25f, -1.25f)));  // fmsubs
    CHECK_EQ(vm.Read32(0x1109C), fbits(1.5f));                            // lfsx of cs
}

// ---------------------------------------------------------------------------
// VMX fixture: vector results.
// ---------------------------------------------------------------------------
TEST(ppu_interp_vmx) {
    Vm vm;
    Lv2 lv2(vm);
    MapFixture(&vm, kvmx_text, kvmx_text_size, kvmx_text_addr, kvmx_data,
               sizeof(kvmx_data), kvmx_data_addr);
    PpuContext ctx;
    int code = RunFixture(&vm, &lv2, &ctx, kvmx_text_addr);
    CHECK_TRUE(!ctx.error);
    if (ctx.error) {
        anyps3test::ReportFail(__FILE__, __LINE__, "vmx: " + ctx.error_msg);
        return;
    }
    CHECK_EQ(code, 9);

    const u8 vsrc[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                         0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    u8 want[256];

    // slot 0: vspltisb 7
    for (int i = 0; i < 16; i++) want[i] = 7;
    // slot 1: vspltish -1 -> all halves 0xFFFF
    for (int i = 0; i < 16; i++) want[16 + i] = 0xFF;
    // slot 2: vspltisw 2 -> BE words of 2: 00 00 00 02
    for (int w = 0; w < 4; w++) {
        want[32 + w * 4] = 0x00;
        want[33 + w * 4] = 0x00;
        want[34 + w * 4] = 0x00;
        want[35 + w * 4] = 0x02;
    }
    // slot 3: vsrc & 7s (vand)
    for (int i = 0; i < 16; i++) want[48 + i] = vsrc[i] & 7;
    // slot 4: vsrc & ~7s (vandc)
    for (int i = 0; i < 16; i++) want[64 + i] = vsrc[i] & ~7;
    // slot 5: (vsrc|7)|0xFFs (vor twice)
    for (int i = 0; i < 16; i++) want[80 + i] = (vsrc[i] | 7) | 0xFF;
    // slot 6: vsrc ^ 7s
    for (int i = 0; i < 16; i++) want[96 + i] = vsrc[i] ^ 7;
    // slot 7: ~(vsrc|7s) (vnor)
    for (int i = 0; i < 16; i++) want[112 + i] = ~(vsrc[i] | 7);
    // slot 8: vaddubm vsrc + 7s
    for (int i = 0; i < 16; i++) want[128 + i] = static_cast<u8>(vsrc[i] + 7);
    // slot 9: vsububm vsrc - 7s
    for (int i = 0; i < 16; i++) want[144 + i] = static_cast<u8>(vsrc[i] - 7);
    // slot 10: vadduhm vsrc + v4. v4 = WORDS of 2 -> half-lanes alternate
    // 0x0000 / 0x0002, so alternate halves get +0 / +2.
    for (int h = 0; h < 8; h++) {
        u16 x = static_cast<u16>((vsrc[h * 2] << 8) | vsrc[h * 2 + 1]);
        u16 add = (h % 2 == 1) ? 2 : 0;
        u16 res = static_cast<u16>(x + add);
        want[160 + h * 2] = static_cast<u8>(res >> 8);
        want[161 + h * 2] = static_cast<u8>(res);
    }
    // slot 11: vadduwm vsrc + words(2)
    for (int w = 0; w < 4; w++) {
        u32 x = (static_cast<u32>(vsrc[w * 4]) << 24) | (static_cast<u32>(vsrc[w * 4 + 1]) << 16) |
                (static_cast<u32>(vsrc[w * 4 + 2]) << 8) | vsrc[w * 4 + 3];
        u32 res = x + 2;
        want[176 + w * 4] = static_cast<u8>(res >> 24);
        want[177 + w * 4] = static_cast<u8>(res >> 16);
        want[178 + w * 4] = static_cast<u8>(res >> 8);
        want[179 + w * 4] = static_cast<u8>(res);
    }
    // slot 12: v14 = v13 + v13 (words)
    for (int w = 0; w < 4; w++) {
        u32 x = (static_cast<u32>(vsrc[w * 4]) << 24) | (static_cast<u32>(vsrc[w * 4 + 1]) << 16) |
                (static_cast<u32>(vsrc[w * 4 + 2]) << 8) | vsrc[w * 4 + 3];
        u32 res = (x + 2) * 2;
        want[192 + w * 4] = static_cast<u8>(res >> 24);
        want[193 + w * 4] = static_cast<u8>(res >> 16);
        want[194 + w * 4] = static_cast<u8>(res >> 8);
        want[195 + w * 4] = static_cast<u8>(res);
    }

    // compare the first 13 result vectors
    for (int i = 0; i < 13; i++) {
        for (int j = 0; j < 16; j++) {
            u32 got = vm.Read8(0x11000 + i * 16 + j);
            CHECK_EQ(got, static_cast<u32>(want[i * 16 + j]));
        }
    }

    // slot 13: vmaddfp va*vb+vc  slot 14: vnmsubfp
    const float va[4] = {1.f, 2.f, 3.f, 4.f};
    const float vb[4] = {5.f, 6.f, 7.f, 8.f};
    const float vc[4] = {0.5f, -1.5f, 2.5f, -3.5f};
    for (int lane = 0; lane < 4; lane++) {
        float madd = va[lane] * vb[lane] + vc[lane];
        float nmsub = -(va[lane] * vb[lane] - vc[lane]);
        u32 got_madd = vm.Read32(0x11000 + 13 * 16 + lane * 4);
        u32 got_nmsub = vm.Read32(0x11000 + 14 * 16 + lane * 4);
        u32 w_madd, w_nmsub;
        std::memcpy(&w_madd, &madd, 4);
        std::memcpy(&w_nmsub, &nmsub, 4);
        CHECK_EQ(got_madd, w_madd);
        CHECK_EQ(got_nmsub, w_nmsub);
    }
}

// ---------------------------------------------------------------------------
// Reservation semantics: lwarx/stwcx retry loop + unpaired stwcx must fail.
// ---------------------------------------------------------------------------
TEST(ppu_interp_sync) {
    Vm vm;
    Lv2 lv2(vm);
    MapFixture(&vm, ksync_text, ksync_text_size, ksync_text_addr, ksync_data,
               sizeof(ksync_data), ksync_data_addr);
    PpuContext ctx;
    int code = RunFixture(&vm, &lv2, &ctx, ksync_text_addr);
    CHECK_TRUE(!ctx.error);
    if (ctx.error) {
        anyps3test::ReportFail(__FILE__, __LINE__, "sync: " + ctx.error_msg);
        return;
    }
    // exit codes from the fixture: 15 = word loop ok, 16 = doubleword ok,
    // 17 = unpaired stwcx failed as expected, 18 = value intact (all passed)
    CHECK_EQ(code, 18);
    CHECK_EQ(vm.Read32(0x11000), 15u);      // counter after 5x(+3) increments
    CHECK_EQ(vm.Read64(0x11008), 0x1007u);  // doubleword reservation result
    CHECK_EQ(vm.Read32(0x11010), 0xAAAAu);  // unpaired stwcx did NOT store
    CHECK_TRUE(!ctx.res_valid);             // reservation cleared by the last stwcx
}

// ---------------------------------------------------------------------------
// PPU threads: create/join through the real LV2 syscalls, cooperative
// scheduler, cross-thread lwarx/stwcx.
// ---------------------------------------------------------------------------
TEST(ppu_threads_create_join) {
    Vm vm;
    Lv2 lv2(vm);
    PpuThreadManager mgr(vm);
    lv2.SetThreadManager(&mgr);
    auto syscall_fn = [](PpuContext* c) { static_cast<Lv2*>(c->lv2)->Syscall(c); };

    MapFixture(&vm, kthreads_text, kthreads_text_size, kthreads_text_addr,
               kthreads_data, sizeof(kthreads_data), kthreads_data_addr);
    PpuContext ctx;
    ctx.vm = &vm;
    ctx.lv2 = &lv2;
    ctx.syscall_fn = [](PpuContext* c) { static_cast<Lv2*>(c->lv2)->Syscall(c); };
    ctx.pc = kthreads_text_addr;
    ctx.gpr[1] = kMainStackTop - 0x100;
    ctx.mem_base = vm.mutable_data();

    int code = mgr.RunAll(&ctx, syscall_fn);
    CHECK_TRUE(!ctx.error);
    if (ctx.error) {
        anyps3test::ReportFail(__FILE__, __LINE__, "threads: " + ctx.error_msg);
        return;
    }
    // 42: main joined the worker, saw the counter at 100 (worker added its arg
    // via lwarx/stwcx under the scheduler). 250 = guest-side mismatch.
    CHECK_EQ(code, 42);
    CHECK_EQ(vm.Read32(0x11000), 100u);
    CHECK_TRUE(mgr.thread_count() >= 1);
}
