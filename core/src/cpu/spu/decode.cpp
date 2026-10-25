#include "anyps3/cpu/spu/decode.h"

#include <cstdio>

namespace anyps3 {

// Dispatch = word >> 21 (11-bit index, 2048 entries). The generated table
// (spu_dispatch_table.inc) replicates the SPU ISA format layout: each
// (opcode, magnitude) row covers the index range [op<<magn, (op<<magn)+2^magn),
// where the ignored low bits hold parts of immediates or the RRR rt4 operand.

#include "spu_dispatch_table.inc"

bool SpuDecode(u32 word, u32 pc, SpuInstr* out) {
    out->mn = Smn::UNK;
    out->word = word;
    out->pc = pc;
    out->rt = word & 0x7F;
    out->ra = (word >> 7) & 0x7F;
    out->rb = (word >> 14) & 0x7F;
    out->rt4 = (word >> 21) & 0x7F;
    out->rc = out->rt;  // RRR destination shares the rt position
    out->si10 = static_cast<s32>((word >> 14) & 0x3FF);
    if (out->si10 & 0x200) out->si10 -= 0x400;  // sign-extend 10-bit
    out->i16 = (word >> 7) & 0xFFFF;
    out->i18 = (word >> 7) & 0x3FFFF;
    out->e = (word >> 18) & 1;
    out->d = (word >> 19) & 1;

    const u32 idx = word >> 21;
    out->mn = kSpuDispatch[idx & 2047];

    if (out->mn == Smn::UNK) return false;
    if (out->mn == Smn::STOP) {
        out->stop_code = word & 0x3FFF;
        return out->rb == 0;  // rb must be zero by convention
    }
    return true;
}

std::string SpuDisasm(const SpuInstr& in) {
    char buf[128];
    switch (in.mn) {
        case Smn::STOP:
            std::snprintf(buf, sizeof(buf), "stop 0x%x", in.stop_code);
            return buf;
        case Smn::STOPD: return "stopd";
        case Smn::LNOP: return "lnop";
        case Smn::NOP: return "nop";
        case Smn::STQD: case Smn::LQD:
            std::snprintf(buf, sizeof(buf), "%s $%u, %d($%u)",
                          in.mn == Smn::STQD ? "stqd" : "lqd", in.rt, in.si10 * 16, in.ra);
            return buf;
        case Smn::WRCH:
            std::snprintf(buf, sizeof(buf), "wrch $%u, $%u", in.ra, in.rt);
            return buf;
        case Smn::RDCH:
            std::snprintf(buf, sizeof(buf), "rdch $%u, $%u", in.rt, in.ra);
            return buf;
        case Smn::RCHCNT:
            std::snprintf(buf, sizeof(buf), "rchcnt $%u, $%u", in.rt, in.ra);
            return buf;
        default:
            std::snprintf(buf, sizeof(buf), "spu-op(%d)", static_cast<int>(in.mn));
            return buf;
    }
}

}  // namespace anyps3
