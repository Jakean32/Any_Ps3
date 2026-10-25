// Any_Ps3 — PPU (PowerPC64, big-endian) instruction decoder subset.
// Standard PowerPC instruction encodings (Book-E / PowerPC AS). This is a
// decode + name table, not an emulator; the fallback interpreter implements
// only a subset of these (see interp.h).
#pragma once

#include "types.h"

namespace any_ps3 {

enum class PpuMnemonic {
    Unknown,
    Addi, Addis,      // 14, 15  (li = addi rD,0,simm; lis = addis rD,0,simm)
    Ori, Oris,        // 24, 25  (nop = ori r0,r0,0)
    Xori, Xoris,      // 26, 27
    AndiDot, AndisDot,// 28, 29
    Or, Add, Subf, Nop, Mr, // opcode 31 subforms (444, 266, 40)
    Lwz, Lbz, Lhz, Lha, Lwzu, // 32,34,40,42,33
    Stw, Stb, Sth, Stwu,      // 36,38,44,37
    Ld, Ldu, Lwa,     // 58 DS-form
    Std, Stdu,        // 62 DS-form
    B, Ba, Bl, Bla,   // 18
    Bc, Bca, Bcl, Bcla, // 16
    Bclr, Bcctr,      // 19: 16, 528 (blr/bctr)
    Sc,               // 17
    Twi, Tw,          // 3, 31:4
    Mflr, Mtlr,       // 31:339 (mfLR special = 339? spr move) — see notes
};

struct PpuInsn {
    uint32_t pc = 0;
    uint32_t word = 0;
    PpuMnemonic m = PpuMnemonic::Unknown;
    const char* name = "";
    uint8_t rd = 0, ra = 0, rb = 0;
    int32_t imm = 0;      // sign- or zero-extended per form
    bool valid = false;   // false => decoder does not know this instruction
};

PpuInsn ppu_decode(uint32_t pc, uint32_t word);

// Rough disassembly text (for logs — best-effort, not a full disassembler).
std::string ppu_to_text(const PpuInsn& i);

} // namespace any_ps3
