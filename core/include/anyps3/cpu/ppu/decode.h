// Any_Ps3 — PPC64 (Cell PPE) decoder + disassembler.
//
// The extended-opcode table below is VALIDATED against GNU binutils 2.44
// (powerpc64-linux-gnu-as ground truth; see tests/test_ppu.cpp, which decodes
// the full assembled instruction inventory byte-for-byte).
//
// Not covered yet (see PHASE_REPORTS.md): VMX/AltiVec, FP arithmetic, FPSCR modes,
// segmented addressing, reservations (lwarx/stwcx are approximated), sc LEV.
#pragma once

#include <string>

#include "anyps3/types.h"

namespace anyps3 {

enum class Mn : u32 {
    UNK = 0,
    NOP,
    // D-form ALU
    ADDI, ADDIS, ADDIC, ADDIC_RC, SUBFIC, MULLI,
    CMPI, CMPLI,
    ORI, ORIS, XORI, XORIS, ANDI_RC, ANDIS_RC,
    // X-form ALU
    ADD, SUBF, NEG, ADDC, SUBFC, MULLW, MULLD, DIVW, DIVWU, DIVD, DIVDU, MULHW, MULHWU,
    OR, XOR, AND, NAND, NOR, ANDC, ORC, EQV,
    EXTSB, EXTSH, EXTSW, CNTLZW, CNTLZD,
    SLW, SRW, SRAW, SRAWI, SLD, SRD, SRAD, SRADI,
    CMP, CMPL,
    // rotates / shifts extended
    RLWINM, RLWIMI, RLWNM, RLDICL, RLDICR, RLDIC, RLDIMI, RLDCL, RLDCR,
    // CR ops
    CRAND, CRANDC, CREQV, CRNAND, CRNOR, CROR, CRORC, CRXOR, MFCR, MTCRF, MCRF,
    // loads/stores
    LWZ, LWZU, LBZ, LBZU, LHZ, LHZU, LHA, LHAU, STW, STWU, STB, STBU, STH, STHU,
    LWZX, LWZUX, LBZX, LBZUX, STWX, STWUX, STBX, STBUX,
    LHZX, LHZUX, LHAX, LHAUX, STHX, STHUX,
    LFS, LFD, STFS, STFD,
    LD, LDU, LWA, STD, STDU, LWAX, LDX, LDUX, STDX, STDUX,
    LWBRX, STWBRX,
    DCBZ, DCBS, DCBF, DCBT, DCBTST, ICB, SYNC, LWSYNC, EIEIO, ISYNC,
    // branches
    B, BL, BC, BCA, BCL, BCAL, BCLR, BCLRL, BCTR, BCTRL,
    // system
    SC, TWI, TW, TDI, TD, MFSPR, MTSPR,
    MAX = 0xFFF,
};

struct PpuInstr {
    Mn mn = Mn::UNK;
    u32 word = 0;
    u32 pc = 0;
    // unified fields (LSB extraction from the big-endian instruction word)
    u8 rt = 0;    // D/S/T/BO field (bits 6-10)
    u8 ra = 0;    // A/BI field (bits 11-15)
    u8 rb = 0;    // B/SH field (bits 16-20)
    u8 to = 0;    // TO field (same position as rt) for traps
    u8 mb = 0;    // bits 21-25 (rotate masks / CRM / crfS)
    u8 me = 0;    // bits 26-30 (rotate masks)
    u32 ext = 0;  // extended opcode (bits 21-30)
    u32 imm = 0;  // zero-extended D-form immediate
    s32 simm = 0; // sign-extended
    u8 sh = 0;    // shift amount (combined with high bit where applicable)
    u8 bo = 0, bi = 0;
    bool aa = false, lk = false, rcs = false;
    u16 spr = 0;
    u8 crfd = 0, crfs = 0;
    u32 target = 0;  // resolved absolute branch target
};

bool PpuDecode(u32 word, u32 pc, PpuInstr* out);
std::string PpuDisasm(const PpuInstr& in);
const char* PpuGprName(u8 r);

}  // namespace anyps3
