#include "any_ps3/ppu.h"

namespace any_ps3 {

static inline uint32_t bits(uint32_t w, int from, int to) {
    // Inclusive bit range, MSB-numbered (bit 0 = sign bit of the 32-bit word).
    int width = to - from + 1;
    return (w >> (31 - to)) & (uint32_t)((1ull << width) - 1);
}
static inline int32_t sext16(uint32_t v) { return (int32_t)(int16_t)(uint16_t)v; }

PpuInsn ppu_decode(uint32_t pc, uint32_t word) {
    PpuInsn i;
    i.pc = pc;
    i.word = word;
    uint32_t op = (word >> 26) & 0x3F; // primary opcode, bits 0..5
    uint8_t rd = (uint8_t)bits(word, 6, 10);
    uint8_t ra = (uint8_t)bits(word, 11, 15);
    uint8_t rb = (uint8_t)bits(word, 16, 20);
    i.rd = rd; i.ra = ra; i.rb = rb;

    switch (op) {
        case 14: i.m = PpuMnemonic::Addi; i.name = (ra == 0) ? "li" : "addi";
                 i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 15: i.m = PpuMnemonic::Addis; i.name = (ra == 0) ? "lis" : "addis";
                 i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 24: i.m = PpuMnemonic::Ori; i.name = (word == 0x60000000) ? "nop" : "ori";
                 i.imm = (int32_t)(word & 0xFFFF); i.valid = true; break;
        case 25: i.m = PpuMnemonic::Oris; i.name = "oris";
                 i.imm = (int32_t)(word & 0xFFFF); i.valid = true; break;
        case 26: i.m = PpuMnemonic::Xori; i.name = "xori";
                 i.imm = (int32_t)(word & 0xFFFF); i.valid = true; break;
        case 27: i.m = PpuMnemonic::Xoris; i.name = "xoris";
                 i.imm = (int32_t)(word & 0xFFFF); i.valid = true; break;
        case 28: i.m = PpuMnemonic::AndiDot; i.name = "andi."; i.valid = true; break;
        case 29: i.m = PpuMnemonic::AndisDot; i.name = "andis."; i.valid = true; break;
        case 19: {
            // XL-form branches: bclr (XO=16) and bcctr (XO=528)
            uint32_t xo = bits(word, 21, 30);
            switch (xo) {
                case 16:  i.m = PpuMnemonic::Bclr;  i.name = (word & 1) ? "bclrl" : "bclr"; i.valid = true; break;
                case 528: i.m = PpuMnemonic::Bcctr; i.name = (word & 1) ? "bctrl" : "bctr"; i.valid = true; break;
                default:  i.m = PpuMnemonic::Unknown; i.name = "op19"; i.valid = false; break;
            }
            break;
        }
        case 31: {
            uint32_t xo = bits(word, 21, 30); // extended opcode, bits 21..30
            switch (xo) {
                case 444: i.m = PpuMnemonic::Or;
                          i.name = (ra == rb && rb != 0) ? "mr" : "or"; i.valid = true; break;
                case 266: i.m = PpuMnemonic::Add; i.name = "add"; i.valid = true; break;
                case 40:  i.m = PpuMnemonic::Subf; i.name = "subf"; i.valid = true; break;
                case 4:   i.m = PpuMnemonic::Tw; i.name = "tw"; i.valid = true; break;
                case 339: i.m = PpuMnemonic::Mflr;
                          i.name = (bits(word, 11, 20) == 256 /* spr field for LR */) ? "mflr" : "mfspr";
                          i.valid = true; break;
                case 467: i.m = PpuMnemonic::Mtlr; i.name = "mtlr"; i.valid = true; break;
                default:  i.m = PpuMnemonic::Unknown; i.name = "op31"; i.valid = false; break;
            }
            break;
        }
        case 32: i.m = PpuMnemonic::Lwz; i.name = "lwz"; i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 33: i.m = PpuMnemonic::Lwzu; i.name = "lwzu"; i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 34: i.m = PpuMnemonic::Lbz; i.name = "lbz"; i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 36: i.m = PpuMnemonic::Stw; i.name = "stw"; i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 37: i.m = PpuMnemonic::Stwu; i.name = "stwu"; i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 38: i.m = PpuMnemonic::Stb; i.name = "stb"; i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 40: i.m = PpuMnemonic::Lhz; i.name = "lhz"; i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 42: i.m = PpuMnemonic::Lha; i.name = "lha"; i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 44: i.m = PpuMnemonic::Sth; i.name = "sth"; i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        case 58: {
            uint32_t sub = word & 3;
            i.imm = sext16((word >> 2) & 0xFFFF);
            if (sub == 0) { i.m = PpuMnemonic::Ld; i.name = "ld"; i.valid = true; }
            else if (sub == 1) { i.m = PpuMnemonic::Ldu; i.name = "ldu"; i.valid = true; }
            else if (sub == 2) { i.m = PpuMnemonic::Lwa; i.name = "lwa"; i.valid = true; }
            else { i.m = PpuMnemonic::Unknown; i.name = "ds"; i.valid = false; }
            break;
        }
        case 62: {
            uint32_t sub = word & 3;
            i.imm = sext16((word >> 2) & 0xFFFF);
            if (sub == 0) { i.m = PpuMnemonic::Std; i.name = "std"; i.valid = true; }
            else if (sub == 1) { i.m = PpuMnemonic::Stdu; i.name = "stdu"; i.valid = true; }
            else { i.m = PpuMnemonic::Unknown; i.name = "ds"; i.valid = false; }
            break;
        }
        case 16: {
            bool aa = (word >> 1) & 1, lk = word & 1;
            uint32_t bd14 = bits(word, 16, 29);                 // 14-bit BD
            i.imm = ((int32_t)(bd14 << 18) >> 18) << 2;         // sign-extend, word align
            i.m = PpuMnemonic::Bc;
            i.name = lk ? (aa ? "bcla" : "bcl") : (aa ? "bca" : "bc");
            i.valid = true; break;
        }
        case 18: {
            bool aa = (word >> 1) & 1, lk = word & 1;
            uint32_t li24 = bits(word, 6, 29);                  // 24-bit LI
            i.imm = ((int32_t)(li24 << 8) >> 8) << 2;           // sign-extend, word align
            i.name = lk ? (aa ? "bla" : "bl") : (aa ? "ba" : "b");
            i.m = lk ? (aa ? PpuMnemonic::Bla : PpuMnemonic::Bl)
                     : (aa ? PpuMnemonic::Ba : PpuMnemonic::B);
            i.valid = true; break;
        }
        case 17: i.m = PpuMnemonic::Sc; i.name = "sc"; i.valid = true; break;
        case 3:  i.m = PpuMnemonic::Twi; i.name = "twi"; i.imm = sext16(word & 0xFFFF); i.valid = true; break;
        default: i.m = PpuMnemonic::Unknown; i.name = "?"; i.valid = false; break;
    }
    if (word == 0x60000000) { i.m = PpuMnemonic::Nop; i.name = "nop"; i.valid = true; }
    return i;
}

std::string ppu_to_text(const PpuInsn& i) {
    // Best-effort one-line text for logs.
    std::string s = i.name;
    s += " r" + std::to_string(i.rd) + ", r" + std::to_string(i.ra);
    s += ", " + std::to_string(i.imm);
    return s;
}

} // namespace any_ps3
