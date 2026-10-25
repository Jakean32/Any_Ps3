#include "any_ps3/spu.h"

namespace any_ps3 {

// Field extraction per binutils include/opcode/spu.h:
//   RT  = insn & 0x7f;            RA = (insn >> 7) & 0x7f;   RB = (insn >> 14) & 0x7f
//   I10 = SIGNED_EXTRACT(insn, 10, 14)   -> bits 14..23
//   I16 = SIGNED_EXTRACT(insn, 16, 7)    -> bits 7..22
//   I18 = SIGNED_EXTRACT(insn, 18, 7)    -> bits 7..24
static int32_t signed_bits(uint32_t w, int size, int pos) {
    uint32_t mask = (uint32_t)((1ull << size) - 1);
    uint32_t v = (w >> pos) & mask;
    if (v & (1u << (size - 1))) v |= ~mask; // sign-extend
    return (int32_t)v;
}

SpuInsn spu_decode(uint32_t pc, uint32_t word) {
    SpuInsn i;
    i.pc = pc;
    i.word = word;
    i.rt = (uint8_t)(word & 0x7F);
    i.ra = (uint8_t)((word >> 7) & 0x7F);
    i.rb = (uint8_t)((word >> 14) & 0x7F);
    uint32_t op = word >> 21; // 11-bit opcode

    // Format-aware matching, mirroring binutils get_index_for_opcode():
    // the opcode field OVERLAPS the top bits of the immediates, so each
    // format is matched with the mask that ignores exactly the overlap:
    //   RR   (stop/lnop/nop/a)  -> exact opcode
    //   RI10 (ori/ai/lqd/stqd)  -> mask 0x7f8 (ignore low 3 bits)
    //   RI16 (br/brsl)          -> mask 0x7fc (ignore low 2 bits)
    //   RI18 (ila)              -> mask 0x7f0 (ignore low 4 bits)
    switch (op) {
        case 0x000: i.m = SpuMnemonic::Stop; i.name = "stop"; i.valid = true; return i;
        case 0x001: i.m = SpuMnemonic::Lnop; i.name = "lnop"; i.valid = true; return i;
        case 0x201: i.m = SpuMnemonic::Nop;  i.name = "nop";  i.valid = true; return i;
        case 0x0c0: i.m = SpuMnemonic::A;    i.name = "a";    i.valid = true; return i;
        default: break;
    }
    uint32_t op_ri10 = op & 0x7F8;
    uint32_t op_ri16 = op & 0x7FC;
    uint32_t op_ri18 = op & 0x7F0;
    if (op_ri10 == 0x020) {
        i.m = SpuMnemonic::Ori; i.name = "ori";
        i.s10 = signed_bits(word, 10, 14); i.valid = true;
    } else if (op_ri10 == 0x0e0) {
        i.m = SpuMnemonic::Ai; i.name = "ai";
        i.s10 = signed_bits(word, 10, 14); i.valid = true;
    } else if (op_ri10 == 0x1a0) {
        i.m = SpuMnemonic::Lqd; i.name = "lqd";
        i.s10 = signed_bits(word, 10, 14); i.valid = true;
    } else if (op_ri10 == 0x120) {
        i.m = SpuMnemonic::Stqd; i.name = "stqd";
        i.s10 = signed_bits(word, 10, 14); i.valid = true;
    } else if (op_ri16 == 0x190) {
        i.m = SpuMnemonic::Br; i.name = "br";
        i.s16 = signed_bits(word, 16, 7); i.valid = true;
    } else if (op_ri16 == 0x198) {
        i.m = SpuMnemonic::Brsl; i.name = "brsl";
        i.s16 = signed_bits(word, 16, 7); i.valid = true;
    } else if (op_ri18 == 0x210) {
        i.m = SpuMnemonic::Ila; i.name = "ila";
        i.u18 = (uint32_t)signed_bits(word, 18, 7) & 0x3FFFF; i.valid = true;
    } else {
        i.m = SpuMnemonic::Unknown; i.name = "?"; i.valid = false;
    }
    return i;
}

static uint32_t enc_op(uint32_t opcode) { return opcode << 21; }

uint32_t spu_enc_stop() { return enc_op(0x000); }
uint32_t spu_enc_lnop() { return enc_op(0x001); }
uint32_t spu_enc_nop()  { return enc_op(0x201) | 0; }
uint32_t spu_enc_ila(uint8_t rt, uint32_t imm18) {
    return enc_op(0x210) | ((uint32_t)rt & 0x7F) | ((imm18 & 0x3FFFF) << 7);
}
uint32_t spu_enc_ai(uint8_t rt, uint8_t ra, int32_t s10) {
    return enc_op(0x0e0) | ((uint32_t)rt & 0x7F) | (((uint32_t)ra & 0x7F) << 7) |
           (((uint32_t)s10 & 0x3FF) << 14);
}
uint32_t spu_enc_ori(uint8_t rt, uint8_t ra, int32_t s10) {
    return enc_op(0x020) | ((uint32_t)rt & 0x7F) | (((uint32_t)ra & 0x7F) << 7) |
           (((uint32_t)s10 & 0x3FF) << 14);
}
uint32_t spu_enc_a(uint8_t rt, uint8_t ra, uint8_t rb) {
    return enc_op(0x0c0) | ((uint32_t)rt & 0x7F) | (((uint32_t)ra & 0x7F) << 7) |
           (((uint32_t)rb & 0x7F) << 14);
}
uint32_t spu_enc_br(int32_t words) {
    return enc_op(0x190) | (((uint32_t)words & 0xFFFF) << 7);
}
uint32_t spu_enc_brsl(uint8_t rt, int32_t words) {
    return enc_op(0x198) | ((uint32_t)rt & 0x7F) | (((uint32_t)words & 0xFFFF) << 7);
}
uint32_t spu_enc_lqd(uint8_t rt, int32_t s10, uint8_t ra) {
    return enc_op(0x1a0) | ((uint32_t)rt & 0x7F) | (((uint32_t)s10 & 0x3FF) << 14) |
           (((uint32_t)ra & 0x7F) << 7);
}
uint32_t spu_enc_stqd(uint8_t rt, int32_t s10, uint8_t ra) {
    return enc_op(0x120) | ((uint32_t)rt & 0x7F) | (((uint32_t)s10 & 0x3FF) << 14) |
           (((uint32_t)ra & 0x7F) << 7);
}

} // namespace any_ps3
