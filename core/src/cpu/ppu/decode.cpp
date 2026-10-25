#include "anyps3/cpu/ppu/decode.h"

#include <cstdio>
#include <cstring>

namespace anyps3 {

// Ground-truth-validated table (GNU binutils 2.44 powerpc64-linux-gnu-as).
// See research/inventory.dump in the project repo notes and tests/test_ppu.cpp.

bool PpuDecode(u32 word, u32 pc, PpuInstr* out) {
    *out = PpuInstr{};  // value-initialize (memset on non-trivial type is UB-adjacent)
    out->word = word;
    out->pc = pc;
    if (word == 0x60000000) {
        out->mn = Mn::NOP;
        return true;
    }
    u32 op = word >> 26;
    out->rt = static_cast<u8>((word >> 21) & 0x1F);
    out->ra = static_cast<u8>((word >> 16) & 0x1F);
    out->rb = static_cast<u8>((word >> 11) & 0x1F);
    out->ext = (word >> 1) & 0x3FF;
    out->mb = static_cast<u8>((word >> 6) & 0x1F);
    out->me = static_cast<u8>((word >> 1) & 0x1F);
    out->imm = word & 0xFFFF;
    out->simm = static_cast<s16>(word);
    out->rcs = (word & 1) != 0;
    out->bo = out->rt;
    out->bi = out->ra;

    switch (op) {
        case 2:  out->mn = Mn::TDI; out->to = out->rt; return true;
        case 3:  out->mn = Mn::TWI; out->to = out->rt; return true;
        case 7:  out->mn = Mn::MULLI; return true;
        case 8:  out->mn = Mn::SUBFIC; return true;
        case 10:  // CMPLI (L bit in rt)
            out->mn = Mn::CMPLI;
            out->crfd = out->rt >> 2;
            return true;
        case 11:  // CMPI (L bit in rt)
            out->mn = Mn::CMPI;
            out->crfd = out->rt >> 2;
            return true;
        case 12: out->mn = Mn::ADDIC; return true;
        case 13: out->mn = Mn::ADDIC_RC; return true;
        case 14: out->mn = Mn::ADDI; return true;
        case 15: out->mn = Mn::ADDIS; return true;
        case 16: {  // BC forms (BO, BI, BD, AA, LK) — byte offset = BD*4
            out->aa = (word >> 1) & 1;
            out->lk = word & 1;
            s32 off = static_cast<s16>(((word >> 2) & 0x3FFF) << 2);
            out->target = out->aa ? static_cast<u32>(off) : pc + off;
            if (out->lk) out->mn = out->aa ? Mn::BCAL : Mn::BCL;
            else out->mn = out->aa ? Mn::BCA : Mn::BC;
            return true;
        }
        case 17: out->mn = Mn::SC; return true;
        case 18: {  // B / BL — byte offset = LI*4 (26-bit signed)
            out->aa = (word >> 1) & 1;
            out->lk = word & 1;
            u32 li = (word >> 2) & 0xFFFFFF;
            s32 off = static_cast<s32>(li << 8) >> 6;  // (24-bit LI)*4, sign-extended
            out->target = out->aa ? static_cast<u32>(off) : pc + off;
            out->mn = out->lk ? Mn::BL : Mn::B;
            return true;
        }
        case 19: {
            switch (out->ext) {
                case 0:  // mcrf: crfD = rt>>2, crfS = ra>>2
                    out->mn = Mn::MCRF;
                    out->crfd = out->rt >> 2;
                    out->crfs = out->ra >> 2;
                    return true;
                case 16:  // bclr/bclrl
                    out->lk = word & 1;
                    out->mn = out->lk ? Mn::BCLRL : Mn::BCLR;
                    return true;
                case 33:  out->mn = Mn::CRNOR; return true;
                case 129: out->mn = Mn::CRANDC; return true;
                case 150: out->mn = Mn::ISYNC; return true;
                case 193: out->mn = Mn::CRXOR; return true;
                case 225: out->mn = Mn::CRNAND; return true;
                case 257: out->mn = Mn::CRAND; return true;
                case 289: out->mn = Mn::CREQV; return true;
                case 417: out->mn = Mn::CRORC; return true;
                case 449: out->mn = Mn::CROR; return true;
                case 528:  // bcctr/bctrl
                    out->lk = word & 1;
                    out->mn = out->lk ? Mn::BCTRL : Mn::BCTR;
                    return true;
                default: return false;
            }
        }
        case 20: out->mn = Mn::RLWIMI; out->sh = out->rb; return true;
        case 21: out->mn = Mn::RLWINM; out->sh = out->rb; return true;
        case 23: out->mn = Mn::RLWNM; return true;
        case 24: out->mn = Mn::ORI; return true;
        case 25: out->mn = Mn::ORIS; return true;
        case 26: out->mn = Mn::XORI; return true;
        case 27: out->mn = Mn::XORIS; return true;
        case 28: out->mn = Mn::ANDI_RC; return true;
        case 29: out->mn = Mn::ANDIS_RC; return true;
        case 30: {  // MD/MDS rotates (binutils-verified):
            //   ext bits 5-9 = MB operand
            //   MD-form:  ext bits 1-4 = 4-bit XO (0 rldicl, 1 rldicr, 2 rldic,
            //             3 rldimi), ext bit 0 = SH high bit (bit 30)
            //   MDS-form: ext bits 0-4 = 5-bit XO (8 rldcl, 9 rldcr), no sh_hi
            u32 xo5 = out->ext & 0x1F;
            u8 sh_hi = static_cast<u8>(out->ext & 1);
            out->sh = static_cast<u8>(out->rb | (sh_hi << 5));
            switch (xo5) {
                case 0: case 1: out->mn = Mn::RLDICL; return true;   // xo4=0 | sh_hi
                case 2: case 3: out->mn = Mn::RLDICR; return true;     // xo4=1 | sh_hi
                case 4: case 5: out->mn = Mn::RLDIC; return true;      // xo4=2 | sh_hi
                case 6: case 7: out->mn = Mn::RLDIMI; return true;      // xo4=3 | sh_hi
                case 8: out->mn = Mn::RLDCL; return true;
                case 9: out->mn = Mn::RLDCR; return true;
                default: return false;
            }
        }
        case 31: {
            switch (out->ext) {
                case 0:   // CMP (L bit in rt)
                    out->mn = Mn::CMP;
                    out->crfd = out->rt >> 2;
                    return true;
                case 4:   out->mn = Mn::TW; return true;
                case 11:  out->mn = Mn::MULHWU; return true;
                case 19:  out->mn = Mn::MFCR; return true;
                case 20:  out->mn = Mn::LWARX; return true;   // reservation load (word)
                case 21:  out->mn = Mn::LDX; return true;
                case 23:  out->mn = Mn::LWZX; return true;
                case 24:  out->mn = Mn::SLW; return true;
                case 26:  out->mn = Mn::CNTLZW; return true;
                case 27:  out->mn = Mn::SLD; return true;
                case 28:  out->mn = Mn::AND; return true;
                case 32:  // CMPL (L bit in rt)
                    out->mn = Mn::CMPL;
                    out->crfd = out->rt >> 2;
                    return true;
                case 40:  out->mn = Mn::SUBF; return true;
                case 54:  out->mn = Mn::DCBS; return true;
                case 53:  out->mn = Mn::LDUX; return true;
                case 55:  out->mn = Mn::LWZUX; return true;
                case 58:  out->mn = Mn::CNTLZD; return true;
                case 60:  out->mn = Mn::ANDC; return true;
                case 68:  out->mn = Mn::TD; return true;
                case 75:  out->mn = Mn::MULHW; return true;
                case 84:  out->mn = Mn::LDARX; return true;   // reservation load (doubleword)
                case 86:  out->mn = Mn::DCBF; return true;
                case 87:  out->mn = Mn::LBZX; return true;
                case 104: out->mn = Mn::NEG; return true;
                case 119: out->mn = Mn::LBZUX; return true;
                case 124: out->mn = Mn::NOR; return true;
                case 144:  // MTCRF: RS = rt field, CRM = (word >> 12) & 0xFF
                    out->mn = Mn::MTCRF;
                    out->mb = static_cast<u8>((word >> 12) & 0xFF);
                    return true;
                case 149: out->mn = Mn::STDX; return true;
                case 150:  out->mn = Mn::STWCX; out->rcs = true; return true;  // always Rc
                case 151: out->mn = Mn::STWX; return true;
                case 181: out->mn = Mn::STDUX; return true;
                case 183: out->mn = Mn::STWUX; return true;
                case 214:  out->mn = Mn::STDCX; out->rcs = true; return true;  // always Rc
                case 215: out->mn = Mn::STBX; return true;
                case 233: out->mn = Mn::MULLD; return true;
                case 235: out->mn = Mn::MULLW; return true;
                case 246: out->mn = Mn::DCBTST; return true;
                case 247: out->mn = Mn::STBUX; return true;
                case 266: out->mn = Mn::ADD; return true;
                case 278: out->mn = Mn::DCBT; return true;
                case 279: out->mn = Mn::LHZX; return true;
                case 284: out->mn = Mn::EQV; return true;
                case 311: out->mn = Mn::LHZUX; return true;
                case 316: out->mn = Mn::XOR; return true;
                case 339:  // MFSPR: spr = ra | (rb << 5)
                    out->mn = Mn::MFSPR;
                    out->spr = static_cast<u16>(out->ra | (out->rb << 5));
                    return true;
                case 341: out->mn = Mn::LWAX; return true;
                case 343: out->mn = Mn::LHAX; return true;
                case 375: out->mn = Mn::LHAUX; return true;
                case 407: out->mn = Mn::STHX; return true;
                case 412: out->mn = Mn::ORC; return true;
                case 444: out->mn = Mn::OR; return true;
                case 459: out->mn = Mn::DIVWU; return true;
                case 467:  // MTSPR: spr = ra | (rb << 5)
                    out->mn = Mn::MTSPR;
                    out->spr = static_cast<u16>(out->ra | (out->rb << 5));
                    return true;
                case 476: out->mn = Mn::NAND; return true;
                case 489: out->mn = Mn::DIVD; return true;
                case 491: out->mn = Mn::DIVW; return true;
                case 457: out->mn = Mn::DIVDU; return true;
                case 534: out->mn = Mn::LWBRX; return true;
                case 536: out->mn = Mn::SRW; return true;
                case 539: out->mn = Mn::SRD; return true;
                case 598:  // sync family: L bit = LSB of the rt field (bit 6)
                    out->mn = (out->rt & 1) ? Mn::LWSYNC : Mn::SYNC;
                    return true;
                case 662: out->mn = Mn::STWBRX; return true;
                case 792: out->mn = Mn::SRAW; return true;
                case 794: out->mn = Mn::SRAD; return true;
                case 824: out->mn = Mn::SRAWI; out->sh = out->rb; return true;
                case 826: case 827:  // sradi: sh_hi = ext bit 0 (bit 30)
                    out->mn = Mn::SRADI;
                    out->sh = static_cast<u8>(out->rb | ((out->ext & 1) << 5));
                    return true;
                case 854: out->mn = Mn::EIEIO; return true;
                case 922: out->mn = Mn::EXTSH; return true;
                case 954: out->mn = Mn::EXTSB; return true;
                case 982: out->mn = Mn::ICB; return true;
                case 986: out->mn = Mn::EXTSW; return true;
                case 1014: out->mn = Mn::DCBZ; return true;
                // vector loads/stores (binutils: lvx=103 lvxl=359 stvx=231 stvxl=487)
                case 103: out->mn = Mn::LVX; return true;
                case 231: out->mn = Mn::STVX; return true;
                case 359: out->mn = Mn::LVXL; return true;
                case 487: out->mn = Mn::STVXL; return true;
                // fp X-form loads/stores
                case 535: out->mn = Mn::LFSX; return true;
                case 567: out->mn = Mn::LFSUX; return true;
                case 599: out->mn = Mn::LFDX; return true;
                case 631: out->mn = Mn::LFDUX; return true;
                case 663: out->mn = Mn::STFSX; return true;
                case 695: out->mn = Mn::STFSUX; return true;
                case 727: out->mn = Mn::STFDX; return true;
                case 759: out->mn = Mn::STFDUX; return true;
                default: return false;
            }
        }
        case 32: out->mn = Mn::LWZ; return true;
        case 33: out->mn = Mn::LWZU; return true;
        case 34: out->mn = Mn::LBZ; return true;
        case 35: out->mn = Mn::LBZU; return true;
        case 36: out->mn = Mn::STW; return true;
        case 37: out->mn = Mn::STWU; return true;
        case 38: out->mn = Mn::STB; return true;
        case 39: out->mn = Mn::STBU; return true;
        case 40: out->mn = Mn::LHZ; return true;
        case 41: out->mn = Mn::LHZU; return true;
        case 42: out->mn = Mn::LHA; return true;
        case 43: out->mn = Mn::LHAU; return true;
        case 44: out->mn = Mn::STH; return true;
        case 45: out->mn = Mn::STHU; return true;
        case 48: out->mn = Mn::LFS; return true;
        case 49: out->mn = Mn::LFSU; return true;
        case 50: out->mn = Mn::LFD; return true;
        case 51: out->mn = Mn::LFDU; return true;
        case 52: out->mn = Mn::STFS; return true;
        case 53: out->mn = Mn::STFSU; return true;
        case 54: out->mn = Mn::STFD; return true;
        case 55: out->mn = Mn::STFDU; return true;
        case 58: {  // DS-form loads (14-bit DS scaled by 4)
            u32 ds = (word >> 2) & 0x3FFF;
            out->simm = (static_cast<s32>(ds << 18) >> 18) << 2;
            switch (word & 3) {
                case 0: out->mn = Mn::LD; return true;
                case 1: out->mn = Mn::LDU; return true;
                case 2: out->mn = Mn::LWA; return true;
                default: return false;
            }
        }
        case 62: {  // DS-form stores
            u32 ds = (word >> 2) & 0x3FFF;
            out->simm = (static_cast<s32>(ds << 18) >> 18) << 2;
            switch (word & 3) {
                case 0: out->mn = Mn::STD; return true;
                case 1: out->mn = Mn::STDU; return true;
                default: return false;
            }
        }
        case 59: {  // single-precision A-form. The 5-bit XO lives in PPC bits
            // 26-30; PPC bits 21-25 hold frC (our mb field). binutils-verified.
            u32 xo5 = (word >> 1) & 0x1F;
            switch (xo5) {
                case 21:  out->mn = Mn::FADDS; return true;
                case 20:  out->mn = Mn::FSUBS; return true;
                case 25:  out->mn = Mn::FMULS; return true;
                case 18:  out->mn = Mn::FDIVS; return true;
                case 22:  out->mn = Mn::FSQRTS; return true;
                case 24:  out->mn = Mn::FRES; return true;
                case 29:  out->mn = Mn::FMADDS; return true;
                case 28:  out->mn = Mn::FMSUBS; return true;
                case 31:  out->mn = Mn::FNMADDS; return true;
                case 30:  out->mn = Mn::FNMSUBS; return true;
                default: return false;
            }
        }
        case 63: {  // double-precision A-form (5-bit XO, frC in mb) + X-form (10-bit)
            u32 xo5 = (word >> 1) & 0x1F;
            switch (xo5) {
                case 21:  out->mn = Mn::FADD; return true;
                case 20:  out->mn = Mn::FSUB; return true;
                case 25:  out->mn = Mn::FMUL; return true;
                case 18:  out->mn = Mn::FDIV; return true;
                case 22:  out->mn = Mn::FSQRT; return true;
                case 29:  out->mn = Mn::FMADD; return true;
                case 28:  out->mn = Mn::FMSUB; return true;
                case 31:  out->mn = Mn::FNMADD; return true;
                case 30:  out->mn = Mn::FNMSUB; return true;
                case 26:  out->mn = Mn::FRSQRTE; return true;
                case 23:  out->mn = Mn::FSEL; return true;
                default: break;  // fall through to the X-form encodings
            }
            switch (out->ext) {
                case 0:   out->mn = Mn::FCMPU; out->crfd = out->rt >> 2; return true;
                case 32:  out->mn = Mn::FCMPO; out->crfd = out->rt >> 2; return true;
                case 12:  out->mn = Mn::FRSP; return true;
                case 14:  out->mn = Mn::FCTIW; return true;
                case 15:  out->mn = Mn::FCTIWZ; return true;
                case 40:  out->mn = Mn::FNEG; return true;
                case 72:  out->mn = Mn::FMR; return true;
                case 136: out->mn = Mn::FNABS; return true;
                case 264: out->mn = Mn::FABS; return true;
                case 583: out->mn = Mn::MFFS; return true;
                case 70:  out->mn = Mn::MTFSB0; out->crfd = out->rt >> 2; return true;
                case 38:  out->mn = Mn::MTFSB1; out->crfd = out->rt >> 2; return true;
                case 711: out->mn = Mn::MTFSF;
                          out->mb = static_cast<u8>((word >> 17) & 0xFF);  // FM field
                          return true;
                case 64:  out->mn = Mn::MCRFS;
                          out->crfd = out->rt >> 2; out->crfs = out->ra >> 2;
                          return true;
                default: return false;
            }
        }
        case 4: {  // VMX / AltiVec.
            // VA-form (vC operand, PPC bits 21-25 -> mb): 6-bit key =
            // (XO5 << 1) | bit31. Binutils-verified: vsel=42 vperm=43
            // vmaddfp=46 vnmsubfp=47. Assembly operand order for these is
            // vD,vA,vC,vB — the interpreter reads mb as vC.
            u32 va_key = (((word >> 1) & 0x1F) << 1) | (word & 1);
            switch (va_key) {
                case 42: out->mn = Mn::VSEL; return true;
                case 43: out->mn = Mn::VPERM; return true;
                case 46: out->mn = Mn::VMADDFP; return true;
                case 47: out->mn = Mn::VNMSUBFP; return true;
                default: break;
            }
            // VX-form: 10-bit XO in PPC bits 21-30 (no vC operand).
            switch (out->ext) {
                case 0:    out->mn = Mn::VADDUBM; return true;
                case 32:   out->mn = Mn::VADDUHM; return true;
                case 64:   out->mn = Mn::VADDUWM; return true;
                case 512:  out->mn = Mn::VSUBUBM; return true;
                case 544:  out->mn = Mn::VSUBUHM; return true;
                case 576:  out->mn = Mn::VSUBUWM; return true;
                case 70:   out->mn = Mn::VMRGHW; return true;
                case 198:  out->mn = Mn::VMRGLW; return true;
                case 390:  out->mn = Mn::VSPLTISB; return true;
                case 422:  out->mn = Mn::VSPLTISH; return true;
                case 454:  out->mn = Mn::VSPLTISW; return true;
                case 514:  out->mn = Mn::VAND; return true;
                case 546:  out->mn = Mn::VANDC; return true;
                case 578:  out->mn = Mn::VOR; return true;
                case 610:  out->mn = Mn::VXOR; return true;
                case 642:  out->mn = Mn::VNOR; return true;
                case 770:  out->mn = Mn::MFVSCR; return true;
                case 802:  out->mn = Mn::MTVSCR; return true;
                default: return false;
            }
        }
        default:
            return false;
    }
}

const char* PpuGprName(u8 r) {
    static const char* names[32] = {"r0",  "sp",  "rtoc", "r3",  "r4",  "r5",  "r6",
                                    "r7",  "r8",  "r9",   "r10", "r11", "r12", "r13",
                                    "r14", "r15", "r16",  "r17", "r18", "r19", "r20",
                                    "r21", "r22", "r23",  "r24", "r25", "r26", "r27",
                                    "r28", "r29", "r30",  "r31"};
    return names[r & 31];
}

std::string PpuDisasm(const PpuInstr& in) {
    char buf[160];
    const char* d = PpuGprName(in.rt);
    const char* a = PpuGprName(in.ra);
    const char* b = PpuGprName(in.rb);
    switch (in.mn) {
        case Mn::NOP:  return "nop";
        case Mn::ADDI:
            if (in.ra == 0) { std::snprintf(buf, sizeof(buf), "li %s, %d", d, in.simm); }
            else { std::snprintf(buf, sizeof(buf), "addi %s, %s, %d", d, a, in.simm); }
            return buf;
        case Mn::ADDIS:
            if (in.ra == 0) { std::snprintf(buf, sizeof(buf), "lis %s, %d", d, in.simm); }
            else { std::snprintf(buf, sizeof(buf), "addis %s, %s, %d", d, a, in.simm); }
            return buf;
        case Mn::ORI:
            if (in.ra == 0) { std::snprintf(buf, sizeof(buf), "li %s, %u", d, in.imm); }
            else { std::snprintf(buf, sizeof(buf), "ori %s, %s, %u", d, a, in.imm); }
            return buf;
        case Mn::B:    std::snprintf(buf, sizeof(buf), "b 0x%08x", in.target); return buf;
        case Mn::BL:   std::snprintf(buf, sizeof(buf), "bl 0x%08x", in.target); return buf;
        case Mn::BC:   std::snprintf(buf, sizeof(buf), "bc bo=%d bi=%d -> 0x%08x", in.bo, in.bi, in.target); return buf;
        case Mn::BCLR: return in.lk ? "blrl" : "blr";
        case Mn::BCTR: return in.lk ? "bctrl" : "bctr";
        case Mn::SC:   return "sc";
        case Mn::MFSPR:
            if (in.spr == 8) { std::snprintf(buf, sizeof(buf), "mflr %s", d); return buf; }
            if (in.spr == 9) { std::snprintf(buf, sizeof(buf), "mfctr %s", d); return buf; }
            if (in.spr == 1) { std::snprintf(buf, sizeof(buf), "mfxer %s", d); return buf; }
            std::snprintf(buf, sizeof(buf), "mfspr %s, %d", d, in.spr);
            return buf;
        case Mn::MTSPR:
            if (in.spr == 8) { std::snprintf(buf, sizeof(buf), "mtlr %s", d); return buf; }
            if (in.spr == 9) { std::snprintf(buf, sizeof(buf), "mtctr %s", d); return buf; }
            if (in.spr == 1) { std::snprintf(buf, sizeof(buf), "mtxer %s", d); return buf; }
            std::snprintf(buf, sizeof(buf), "mtspr %s, %d", d, in.spr);
            return buf;
        case Mn::LWZ: case Mn::LBZ: case Mn::LHZ: case Mn::LHA: case Mn::LWZU:
        case Mn::LBZU: case Mn::LHZU: case Mn::LHAU: case Mn::STW: case Mn::STB:
        case Mn::STH: case Mn::STWU: case Mn::STBU: case Mn::STHU: case Mn::LFS:
        case Mn::LFD: case Mn::STFS: case Mn::STFD: case Mn::LD: case Mn::LDU:
        case Mn::LWA: case Mn::STD: case Mn::STDU: {
            const char* name = "?";
            switch (in.mn) {
                case Mn::LWZ: name = "lwz"; break; case Mn::LBZ: name = "lbz"; break;
                case Mn::LHZ: name = "lhz"; break; case Mn::LHA: name = "lha"; break;
                case Mn::STW: name = "stw"; break; case Mn::STB: name = "stb"; break;
                case Mn::STH: name = "sth"; break; case Mn::LD: name = "ld"; break;
                case Mn::STD: name = "std"; break; case Mn::LFS: name = "lfs"; break;
                case Mn::LFD: name = "lfd"; break; case Mn::STFS: name = "stfs"; break;
                case Mn::STFD: name = "stfd"; break;
                default: name = "mem"; break;
            }
            std::snprintf(buf, sizeof(buf), "%s %s, %d(%s)", name, d, in.simm, a);
            return buf;
        }
        default:
            std::snprintf(buf, sizeof(buf), "op%d ext=%d %s,%s,%s", in.word >> 26, in.ext, d, a, b);
            return buf;
    }
}

}  // namespace anyps3
