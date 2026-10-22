// Any_Ps3 — SPU decoder. The opcode table is anchored to the RPCS3-derived
// SPU ISA table (research/spu_opcode_table.txt; itself validated against real
// games), with dispatch = word >> 21 and format-dependent opcode shifting:
//   RR exact (11-bit op) | RI10 << 3 (8-bit op) | RI16 << 2 (9-bit op)
//   RI18 << 5 (6-bit op) | RRR << 7 (4-bit op)
// Field LSB positions: Rt = w & 0x7F, Ra = (w>>7) & 0x7F, Rb = (w>>14) & 0x7F,
// Rt4 = (w>>21) & 0x7F, I10 = (w>>14) & 0x3FF, I16 = (w>>7) & 0xFFFF,
// I18 = (w>>7) & 0x3FFFF. SPU local store is LITTLE-endian.
#pragma once

#include <string>

#include "anyps3/types.h"

namespace anyps3 {

enum class Smn : u16 {
    UNK = 0,
    STOP, STOPD, LNOP, NOP, SYNC, DSYNC,
    IL, ILHU, ILH, ILA, IOHL, AI, SFI,
    ORI, ORHI, ORBI, ANDI, ANDHI, ANDBI,
    XORI, XORHI, XORBI, CEQI, CGTI, CLGTI,
    A, SF, OR, AND, XOR, NAND, NOR, ANDC, ORC, EQV, CGT, CEQ, CLGT,
    SELB, SHUFB, MPYA,
    STQD, LQD, STQR, LQR, STQA, LQA, STQX, LQX,
    BR, BRA, BRSL, BRZ, BRNZ, BRHZ, BRHNZ, BI, BISL, BIZ, BINZ,
    HBRA, HBRR,
    WRCH, RDCH, RCHCNT,
    FSMBI, FSMB, GB,
    CWD, CBD, CWX, CBX,
    ROT, ROTM, SHL, SHLH, ROTQBY, SHLQBY, CNTB, CLZ,
    XSBH, XSWD, AVGB,
    MAX = 0xFFF,
};

struct SpuInstr {
    Smn mn = Smn::UNK;
    u32 word = 0;
    u32 pc = 0;
    u8 rt = 0, ra = 0, rb = 0, rt4 = 0, rc = 0;
    s32 si10 = 0;
    u32 i16 = 0;
    s32 i18 = 0;
    u32 stop_code = 0;
    bool e = false, d = false;
};

// Decode one SPU instruction (32-bit LE word) at LS address pc.
bool SpuDecode(u32 word, u32 pc, SpuInstr* out);

// Human-readable one-liner (subset).
std::string SpuDisasm(const SpuInstr& in);

}  // namespace anyps3
