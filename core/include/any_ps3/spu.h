// Any_Ps3 — SPU (Synergistic Processor Unit) instruction decoder subset.
// Encodings taken from GNU binutils opcodes/spu-opc.c + include/opcode/spu.h
// (public GPL reference), NOT invented: opcode = top 11 bits (word >> 21),
// RT = low 7 bits, RA = bits 7..13, RB = bits 14..20; I10/I16/I18 per format.
//
// Implemented subset (opcode values from binutils):
//   stop 0x000, lnop 0x001, nop 0x201, ori 0x020, ai 0x0e0, a 0x0c0,
//   lqd 0x1a0, stqd 0x120, ila 0x210, br 0x190, brsl 0x198
// Anything else decodes honestly as Unknown.
#pragma once

#include "types.h"

namespace any_ps3 {

enum class SpuMnemonic {
    Unknown, Stop, Lnop, Nop, Ori, Ai, A, Lqd, Stqd, Ila, Br, Brsl
};

struct SpuInsn {
    uint32_t pc = 0;
    uint32_t word = 0;
    SpuMnemonic m = SpuMnemonic::Unknown;
    const char* name = "";
    uint8_t rt = 0, ra = 0, rb = 0;
    int32_t s10 = 0;   // RI10 signed immediate (raw; scaled per instruction)
    int32_t s16 = 0;   // RI16 signed immediate (br/brsl: offset in WORDS)
    uint32_t u18 = 0;  // RI18 immediate (ila)
    bool valid = false;
};

SpuInsn spu_decode(uint32_t pc, uint32_t word);

// Small hand-encoders used by tests (and future SPU analyzers).
uint32_t spu_enc_stop();
uint32_t spu_enc_lnop();
uint32_t spu_enc_nop();
uint32_t spu_enc_ila(uint8_t rt, uint32_t imm18);
uint32_t spu_enc_ai(uint8_t rt, uint8_t ra, int32_t s10);
uint32_t spu_enc_ori(uint8_t rt, uint8_t ra, int32_t s10);
uint32_t spu_enc_a(uint8_t rt, uint8_t ra, uint8_t rb);
uint32_t spu_enc_br(int32_t words);
uint32_t spu_enc_brsl(uint8_t rt, int32_t words);
uint32_t spu_enc_lqd(uint8_t rt, int32_t s10, uint8_t ra);
uint32_t spu_enc_stqd(uint8_t rt, int32_t s10, uint8_t ra);

} // namespace any_ps3
