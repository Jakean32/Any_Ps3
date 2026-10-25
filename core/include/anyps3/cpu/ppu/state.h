// Any_Ps3 — PPU thread context (register file + runtime hooks).
#pragma once

#include <cstring>
#include <string>

#include "anyps3/memory/vm.h"

namespace anyps3 {

struct PpuContext {
    u64 gpr[32] = {};   // gpr[0] is kept hardwired to zero (PPC convention)
    u64 lr = 0, ctr = 0;
    u32 cr = 0, xer = 0;
    u64 fpr[32] = {};   // raw FP bits (double-precision view)
    u32 fpscr = 0;
    u32 pc = 0;

    // ---- VMX / AltiVec (128-bit registers, big-endian byte order) ----
    u8 vpr[32][16] = {};
    u32 vscr = 0;
    u32 vrsave = 0;

    // ---- reservations (lwarx/ldarx/stwcx/stdcx) — per-thread state ----
    u64 res_addr = 0;
    bool res_valid = false;

    // runtime hooks
    Vm* vm = nullptr;
    void* lv2 = nullptr;                       // struct Lv2* (avoids include cycle)
    void (*syscall_fn)(PpuContext*) = nullptr; // LV2 syscall dispatch (number in r11)
    u8* mem_base = nullptr;                    // guest memory backing base (AOT fast path)
    bool cond = false;                          // materialized CR0 condition (AOT path)

    // control
    bool exit = false;
    s64 exit_code = 0;
    bool error = false;
    u32 error_pc = 0;
    std::string error_msg;
    bool yield_req = 0;    // cooperative scheduler: end this time slice
    bool replay_sc = false;  // re-run the last sc (used for blocking joins)

    // stats
    u64 icount = 0;
    u64 scount = 0;    // syscalls executed
};

// ---- FP helpers: view an fpr as double / single ----
inline double FprD(const PpuContext& ctx, int i) {
    double d;
    std::memcpy(&d, &ctx.fpr[i], sizeof(d));
    return d;
}
inline void SetFprD(PpuContext* ctx, int i, double d) {
    std::memcpy(&ctx->fpr[i], &d, sizeof(d));
}
inline float FprS(const PpuContext& ctx, int i) {
    float f = static_cast<float>(FprD(ctx, i));  // operand is converted to single
    return f;
}

// ---- VMX lane access (registers are big-endian byte sequences) ----
inline u32 VLaneU32(const PpuContext& ctx, int v, int lane) {  // lane 0..3, MSW first
    const u8* p = ctx.vpr[v] + lane * 4;
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | p[3];
}
inline void VSetLaneU32(PpuContext* ctx, int v, int lane, u32 x) {
    u8* p = ctx->vpr[v] + lane * 4;
    p[0] = static_cast<u8>(x >> 24);
    p[1] = static_cast<u8>(x >> 16);
    p[2] = static_cast<u8>(x >> 8);
    p[3] = static_cast<u8>(x);
}
inline float VLaneF32(const PpuContext& ctx, int v, int lane) {
    u32 x = VLaneU32(ctx, v, lane);
    float f;
    std::memcpy(&f, &x, sizeof(f));
    return f;
}
inline void VSetLaneF32(PpuContext* ctx, int v, int lane, float f) {
    u32 x;
    std::memcpy(&x, &f, sizeof(x));
    VSetLaneU32(ctx, v, lane, x);
}

// Set a CR field (crf: 0..7) from a 4-bit value (LT GT EQ SO).
inline void CrSetField(u32* cr, u32 crf, u32 bits) {
    u32 shift = 28 - crf * 4;
    *cr = (*cr & ~(0xFu << shift)) | ((bits & 0xF) << shift);
}

// Branch-condition evaluation for BO/BI — empirically verified against GNU as
// encodings (beq BO=12, bne BO=4, bgt BO=12/BI=1, bdz BO=24, bdnz BO=16,
// blr/bctr BO=20):
//   BO[4] (0x10) == 0 -> CR[BI] is tested with polarity BO[3] (0x08)
//   BO[2] (0x04) == 0 -> CTR is decremented and tested with polarity BO[3]
//   BO[1..0] (0x03)   -> branch-hint bits, no architectural effect
inline bool PpuEvaluateCondition(PpuContext& ctx, u8 bo, u8 bi) {
    bool cond_ok = true;
    bool ctr_ok = true;
    if ((bo & 0x10) == 0) {
        u32 bit = (ctx.cr >> (31 - bi)) & 1;
        cond_ok = (bo & 0x8) ? bit != 0 : bit == 0;
    }
    if ((bo & 0x4) == 0) {
        ctx.ctr--;
        ctr_ok = (bo & 0x8) ? ctx.ctr == 0 : ctx.ctr != 0;
    }
    return ctr_ok && cond_ok;
}

// One-instruction step. Returns false on a guest error (details in ctx).
bool PpuStep(PpuContext* ctx);

// Run until ctx->exit or error. Returns guest exit code.
int PpuRun(PpuContext* ctx);

// Raise a guest-visible interpreter error.
void PpuRaise(PpuContext* ctx, const std::string& what);

}  // namespace anyps3
