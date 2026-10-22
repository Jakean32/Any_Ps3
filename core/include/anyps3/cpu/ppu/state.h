// Any_Ps3 — PPU thread context (register file + runtime hooks).
#pragma once

#include <string>

#include "anyps3/memory/vm.h"

namespace anyps3 {

struct PpuContext {
    u64 gpr[32] = {};   // gpr[0] is kept hardwired to zero (PPC convention)
    u64 lr = 0, ctr = 0;
    u32 cr = 0, xer = 0;
    u64 fpr[32] = {};   // raw FP bits (no FP arithmetic yet — see PHASE_REPORTS.md)
    u32 fpscr = 0;
    u32 pc = 0;

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

    // stats
    u64 icount = 0;
    u64 scount = 0;    // syscalls executed
};

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
