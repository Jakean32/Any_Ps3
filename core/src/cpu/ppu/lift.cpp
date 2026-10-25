// Any_Ps3 — PPU block lifter: PPC64 -> AOT-IR (see ir.h).
//
// IR notes:
//   * The only lifted CR usage is the (cmpwi/cmpdi -> bc-on-CR0) peephole, which
//     becomes a single BranchCond op carrying the compare (reg, imm, kind) plus
//     branch polarity. Everything else CR-related falls back to the interpreter.
//   * BL/BLR/BCTR (register-indirect linkage) fall back to the interpreter.
//   * Any unlisted mnemonic falls back — the runtime executes such blocks with
//     PpuStep. This is the honest "AOT + fallback" split of the architecture.
#include "anyps3/cpu/ppu/ir.h"

#include "anyps3/log.h"

namespace anyps3 {

namespace {

struct LiftState {
    std::vector<AIROp>* out;
    bool has_cr0_cmp = false;
    u8 cmp_reg = 0;
    u64 cmp_imm = 0;
    bool cmp64 = false;
};

bool Emit(LiftState* st, const AIROp& op) {
    if (st->out->size() > 4096) return false;  // block-size safety valve
    st->out->push_back(op);
    return true;
}

// BC on CR0 after cmpwi: BI 0=LT, 1=GT, 2=EQ; branch polarity from BO[3] (0x08).
bool TryLiftBranchOnCr0(LiftState* st, const PpuInstr& in, u32 pc, u32 next_pc) {
    if (!st->has_cr0_cmp || in.bi > 2) return false;
    CmpKind kind;
    switch (in.bi) {
        case 0: kind = CmpKind::Lt; break;
        case 1: kind = CmpKind::Gt; break;
        default: kind = CmpKind::Eq; break;
    }
    AIROp op;
    op.pc = pc;
    op.kind = IRKind::BranchCond;
    op.a = st->cmp_reg;
    op.imm = st->cmp_imm;
    op.cmp = kind;
    op.cmp64 = st->cmp64;
    op.polarity = (in.bo & 0x8) != 0;  // true -> branch when the CR bit is set
    op.target = in.target;
    op.fallthrough = next_pc;
    return Emit(st, op);
}

// Mask for rlw* (MB..ME, MSB bit numbering) — host mask over a 32-bit value.
static u32 RotMask32(u32 mb, u32 me) {
    if (mb <= me) {
        u32 width = me - mb + 1;
        u32 m = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1);
        return m << (31 - me);
    }
    u32 hole_width = mb - me - 1;
    u32 hole = ((1u << hole_width) - 1) << (32 - mb);
    return ~hole;
}

}  // namespace

bool PpuLiftBlock(Vm& vm, u32 start_pc, u32 max_instrs, std::vector<AIROp>* ops) {
    ops->clear();
    LiftState st{ops};
    u32 pc = start_pc;
    u32 count = 0;

    while (count++ < max_instrs) {
        const u8* code = vm.Ptr(pc, 4);
        if (!code) return false;
        u32 word = ReadBE32(code);
        PpuInstr in;
        if (!PpuDecode(word, pc, &in)) return false;
        const u32 next_pc = pc + 4;

        switch (in.mn) {
            case Mn::NOP:
            case Mn::SYNC: case Mn::LWSYNC: case Mn::EIEIO: case Mn::ISYNC:
            case Mn::DCBS: case Mn::DCBF: case Mn::DCBT: case Mn::DCBTST: case Mn::ICB:
                break;  // no architectural effect in our model

            case Mn::ADDI: case Mn::ADDIC: {
                AIROp op;
                op.pc = pc;
                if (in.ra == 0) {
                    op.kind = IRKind::GprImm;
                    op.dst = in.rt;
                    op.imm = static_cast<u64>(static_cast<s64>(in.simm));
                } else {
                    op.kind = IRKind::GprAddImm;
                    op.dst = in.rt;
                    op.a = in.ra;
                    op.imm = static_cast<u64>(static_cast<s64>(in.simm));
                }
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::ADDIS: {
                AIROp op;
                op.pc = pc;
                if (in.ra == 0) {
                    op.kind = IRKind::GprImm;
                    op.dst = in.rt;
                    op.imm = static_cast<u64>(in.imm) << 16;
                } else {
                    op.kind = IRKind::GprAddImm;
                    op.dst = in.rt;
                    op.a = in.ra;
                    op.imm = static_cast<u64>(in.imm) << 16;
                }
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::ORI:
                if (in.ra == 0) {
                    AIROp op;
                    op.pc = pc;
                    op.kind = IRKind::GprImm;
                    op.dst = in.rt;
                    op.imm = in.imm;
                    if (!Emit(&st, op)) return false;
                } else {
                    AIROp op;
                    op.pc = pc;
                    op.kind = IRKind::AluOrImm;
                    op.dst = in.rt;
                    op.a = in.ra;
                    op.imm = in.imm;
                    if (!Emit(&st, op)) return false;
                }
                break;
            case Mn::ADD: case Mn::OR: case Mn::XOR: case Mn::AND: {
                AIROp op;
                op.pc = pc;
                op.kind = in.mn == Mn::ADD ? IRKind::AluAdd
                        : in.mn == Mn::OR ? IRKind::AluOr
                        : in.mn == Mn::XOR ? IRKind::AluXor
                                          : IRKind::AluAnd;
                op.dst = in.rt;
                op.a = in.ra;
                op.b = in.rb;
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::SUBF: {
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::AluSub;
                op.dst = in.rt;
                op.a = in.ra;
                op.b = in.rb;  // dst = b - a
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::MULLW: {
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::AluMulW;
                op.dst = in.rt;
                op.a = in.ra;
                op.b = in.rb;
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::SRADI: {
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::AluSar64Imm;
                op.dst = in.rt;
                op.a = in.ra;
                op.b = in.sh;  // shift rides in b (0..63)
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::RLWINM: {
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::Rlwinm;
                op.dst = in.ra;
                op.a = in.rt;  // source is the rt field
                op.b = in.sh;
                op.imm = RotMask32(in.mb, in.me);
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::LWZ: {
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::Load32;
                op.dst = in.rt;
                op.a = in.ra;
                op.imm = static_cast<u64>(static_cast<s64>(in.simm));
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::LWA: {  // DS-form sign-extended load (LWAX X-form falls back)
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::Load32s;
                op.dst = in.rt;
                op.a = in.ra;
                op.imm = static_cast<u64>(static_cast<s64>(in.simm));
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::LD: {
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::Load64;
                op.dst = in.rt;
                op.a = in.ra;
                op.imm = static_cast<u64>(static_cast<s64>(in.simm));
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::STW: {
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::Store32;
                op.dst = in.rt;  // store source register
                op.a = in.ra;
                op.imm = static_cast<u64>(static_cast<s64>(in.simm));
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::STD: {
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::Store64;
                op.dst = in.rt;
                op.a = in.ra;
                op.imm = static_cast<u64>(static_cast<s64>(in.simm));
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::CMPI: {  // cmpwi / cmpdi (CR0 target only)
                if (in.crfd != 0) return false;
                st.has_cr0_cmp = true;
                st.cmp_reg = in.ra;
                st.cmp_imm = static_cast<u64>(static_cast<s64>(in.simm));
                st.cmp64 = (in.rt & 1) != 0;  // L bit: cmpdi
                break;  // compare is folded into the following BranchCond
            }
            case Mn::MTSPR: {
                IRKind k = in.spr == 9 ? IRKind::MtCtr : in.spr == 8 ? IRKind::MtLr
                                                                        : IRKind::Fail;
                if (k == IRKind::Fail) return false;
                AIROp op;
                op.pc = pc;
                op.kind = k;
                op.dst = in.rt;
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::SC: {
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::Syscall;
                if (!Emit(&st, op)) return false;
                break;
            }
            case Mn::B: {
                AIROp op;
                op.pc = pc;
                op.kind = IRKind::BranchAlways;
                op.target = in.target;
                if (!Emit(&st, op)) return false;
                return true;  // block ends
            }
            case Mn::BL:
                return false;  // BL linkage falls back to the interpreter
            case Mn::BC: {
                if (in.lk) return false;
                if ((in.bo & 0x4) == 0) {  // CTR-driven (bdnz/bdz family)
                    if ((in.bo & 0x10) == 0) return false;  // CR+CTR forms fall back
                    AIROp op;
                    op.pc = pc;
                    op.kind = IRKind::BranchCtr;
                    op.polarity = (in.bo & 0x8) != 0;  // true: bdz (==0); false: bdnz (!=0)
                    op.target = in.target;
                    op.fallthrough = next_pc;
                    if (!Emit(&st, op)) return false;
                    return true;
                }
                if ((in.bo & 0x10) != 0) {  // CR ignored -> always
                    AIROp op;
                    op.pc = pc;
                    op.kind = IRKind::BranchAlways;
                    op.target = in.target;
                    if (!Emit(&st, op)) return false;
                    return true;
                }
                if (!TryLiftBranchOnCr0(&st, in, pc, next_pc)) return false;
                return true;
            }
            case Mn::BCL: case Mn::BCA: case Mn::BCAL: case Mn::BCLR: case Mn::BCLRL:
            case Mn::BCTR: case Mn::BCTRL:
                return false;  // indirect/linkage branches fall back
            default:
                return false;  // everything else falls back
        }
        pc = next_pc;
    }
    // Budget exhausted without a terminator: fall through to the next block.
    AIROp op;
    op.pc = pc;
    op.kind = IRKind::BranchAlways;
    op.target = pc;
    if (!Emit(&st, op)) return false;
    return true;
}

}  // namespace anyps3
