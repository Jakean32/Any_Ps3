// Any_Ps3 — AOT-IR: the intermediate representation the PPU lifter produces and
// the AOT backends (MiniAOT x86-64, LLVM, interpreter-of-IR) consume.
//
// Design: one IR op per PPC64 instruction (no SSA yet — vregs are the guest GPR
// slots in PpuContext). Unsupported instructions make LiftBlock fail, and the
// runtime falls back to the interpreter for that block. This is exactly the
// "AOT with interpreter fallback" architecture from ARCHITECTURE.md.
#pragma once

#include "anyps3/cpu/ppu/decode.h"
#include "anyps3/memory/vm.h"

#include <vector>

namespace anyps3 {

enum class IRKind : u8 {
    GprImm,        // dst = imm
    GprMov,        // dst = src
    GprAddImm,     // dst = src + simm
    AluAdd,        // dst = a + b
    AluSub,        // dst = b - a (PPC subf(rt, ra, rb): rt = rb - ra)
    AluOr,         // dst = a | b
    AluXor,        // dst = a ^ b
    AluAnd,        // dst = a & b
    Load32,        // dst = BE32[guest[rbase + off]]     (zero-extended)
    Load32s,       // dst = (s32)BE32[...]               (sign-extended)
    Load64,        // dst = BE64[...]
    Store32,       // BE32[rbase+off] = rsrc
    Store64,       // BE64[rbase+off] = rsrc
    MtCtr,         // ctr = rsrc
    MtLr,          // lr  = rsrc
    AluMulW,       // dst = (s64)(s32)a * (s32)b      (mullw)
    AluSar64Imm,   // dst = (s64)a >> shift            (sradi)
    Rlwinm,        // dst = rol32(a, sh) & mask       (rlwinm/slwi/srwi)
    AluOrImm,      // dst = a | imm                    (ori with source reg)
    CondCmpImm,    // cond = compare(rsrc, imm) per cmp_kind (signed)
    Syscall,       // ctx->syscall_fn(ctx); check exit/error
    BranchAlways,  // end block: pc = target
    BranchCond,    // end block: if (cond == polarity) pc = target else pc = fallthrough
    BranchCtr,     // end block: ctr--; if (ctr != 0 per polarity) pc = target else fallthrough
    Fail,          // unlift-able — caller falls back to interpreter
};

enum class CmpKind : u8 { Eq, Ne, Lt, Gt, Le, Ge };

struct AIROp {
    IRKind kind;
    u8 dst = 0, a = 0, b = 0;   // guest GPR indices
    u64 imm = 0;
    u32 pc = 0;                 // guest pc of the originating instruction
    CmpKind cmp = CmpKind::Eq;
    bool cmp64 = false;         // BranchCond: compare width (cmpdi vs cmpwi)
    bool polarity = true;       // BranchCond: required cond value
    u32 target = 0, fallthrough = 0;
};

// Lifts one basic block starting at `start_pc` (guest memory via vm). Fills `ops`.
// Returns false when the block contains unsupported constructs — the caller must
// execute the block with the interpreter fallback instead.
bool PpuLiftBlock(Vm& vm, u32 start_pc, u32 max_instrs, std::vector<AIROp>* ops);

}  // namespace anyps3
