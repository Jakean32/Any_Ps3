// Any_Ps3 — PPU interpreter (the mandated fallback backend; also the reference
// semantics for the AOT backends). Executes decoded PPC64 instructions directly.
//
// Register directions (verified against GNU as ground truth):
//   D-form ALU:   r[rt] = f(r[ra], imm)         (ra==0 reads as 0)
//   X-form ALU:   r[rt] = f(r[ra], r[rb])        (destination is the rt field)
//   M-form rotates: r[ra] = mask(rot(r[rt]))     (source is the rt field)
//   X loads/stores: r[rt] <- mem[r[ra]+r[rb]] / mem[..] <- r[rt]
//   MFSPR: r[rt] = spr; MTSPR: spr = r[rt]
#include "anyps3/cpu/ppu/state.h"

#include <cmath>
#include <cstring>

#include "anyps3/cpu/ppu/decode.h"
#include "anyps3/log.h"

namespace anyps3 {

void PpuRaise(PpuContext* ctx, const std::string& what) {
    if (!ctx->error) {
        ctx->error = true;
        ctx->error_pc = ctx->pc;
        ctx->error_msg = what;
    }
}

static inline u32 Rotl32(u32 v, u32 n) { return n ? (v << n) | (v >> (32 - n)) : v; }
static inline u64 Rotl64(u64 v, u64 n) { return n ? (v << n) | (v >> (64 - n)) : v; }

// Mask for rlw* (MB..ME inclusive, MSB bit numbering; wrapping allowed).
// MSB bit b == host bit (31-b), so kept bits map to host bits (31-mb)..(31-me).
static u32 RotMask32(u32 mb, u32 me) {
    if (mb <= me) {
        u32 width = me - mb + 1;
        u32 m = (width == 32) ? 0xFFFFFFFFu : ((1u << width) - 1);
        return m << (31 - me);
    }
    // wrap: zero hole at MSB bits (me+1)..(mb-1)
    u32 hole_width = mb - me - 1;
    u32 hole = ((1u << hole_width) - 1) << (32 - mb);
    return ~hole;
}
// rldicl (clear left): zeroes the top `mb` bits — host mask = ~0 >> mb.
static inline u64 RldiClMask(u32 mb) { return mb == 0 ? ~0ULL : (~0ULL >> mb); }
static inline u64 RldiCrMask(u32 me) { return me == 63 ? ~0ULL : ~0ULL << (63 - me); }

bool PpuStep(PpuContext* ctx) {
    const u32 pc = ctx->pc;
    const u8* code = ctx->vm->Ptr(pc, 4);
    if (!code) {
        PpuRaise(ctx, "instruction fetch outside guest memory");
        return false;
    }
    u32 word = ReadBE32(code);
    PpuInstr in;
    if (!PpuDecode(word, pc, &in)) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "%08x", word);
        PpuRaise(ctx, std::string("unsupported instruction word 0x") + tmp);
        return false;
    }
    ctx->icount++;
    Vm& vm = *ctx->vm;
    u64* r = ctx->gpr;
    // NOTE: gpr[0] is a REAL register on PPC. Only the rA=0 *addressing* forms
    // mean "zero base" (handled by ra_val/ea_x below); the old blanket
    // `r[0] = 0` hardwire here silently dropped legitimate writes to r0 (the
    // PowerOpen import-stub pattern uses r0 as scratch: mflr r0 / lwz r0 ... /
    // mtctr r0). Found by the module fixture.
    const u32 next_pc = pc + 4;
    u32 new_pc = next_pc;

    auto ra_val = [&](const PpuInstr& i) -> u64 { return i.ra == 0 ? 0 : r[i.ra]; };
    auto ea_d = [&](const PpuInstr& i) -> u32 {
        return static_cast<u32>(ra_val(i) + static_cast<s64>(i.simm));
    };
    auto ea_x = [&](const PpuInstr& i) -> u32 {
        // X-form: rA=0 means zero base, but rB is ALWAYS a real register index
        // (rB=0 uses gpr[0] as the index — no zero-substitution there).
        return static_cast<u32>(ra_val(i) + r[i.rb]);
    };
    auto cmp_bits = [&](s64 a, s64 b) {
        u32 bits = (a < b) ? 8 : (a > b) ? 4 : 2;
        bits |= (ctx->xer >> 31) & 1;
        return bits;
    };
    auto set_cr0 = [&](u64 v) {
        CrSetField(&ctx->cr, 0, cmp_bits(static_cast<s64>(v), 0));
    };

    switch (in.mn) {
        case Mn::NOP:
        case Mn::SYNC: case Mn::LWSYNC: case Mn::EIEIO: case Mn::ISYNC:
        case Mn::DCBS: case Mn::DCBF: case Mn::DCBT: case Mn::DCBTST: case Mn::ICB:
            break;

        // ---------------- D-form ALU ----------------
        case Mn::ADDI: case Mn::ADDIC:
            r[in.rt] = ra_val(in) + static_cast<s64>(in.simm);
            break;
        case Mn::ADDIC_RC:
            r[in.rt] = ra_val(in) + static_cast<s64>(in.simm);
            set_cr0(r[in.rt]);
            break;
        case Mn::ADDIS:
            r[in.rt] = ra_val(in) + (static_cast<u64>(in.imm) << 16);
            break;
        case Mn::SUBFIC:
            r[in.rt] = static_cast<u64>(in.simm) - ra_val(in);
            break;
        case Mn::MULLI:
            r[in.rt] = static_cast<s64>(static_cast<s32>(in.simm)) *
                       static_cast<s64>(static_cast<s32>(ra_val(in) & 0xFFFFFFFF));
            break;
        // D-form logicals: PPC writes "ori rA, rS, imm" — the DEST is the ra
        // field (bits 11-15), the SOURCE is the rt field (bits 6-10).
        case Mn::ORI:   r[in.ra] = r[in.rt] | in.imm; break;
        case Mn::ORIS:  r[in.ra] = r[in.rt] | (static_cast<u64>(in.imm) << 16); break;
        case Mn::XORI:  r[in.ra] = r[in.rt] ^ in.imm; break;
        case Mn::XORIS: r[in.ra] = r[in.rt] ^ (static_cast<u64>(in.imm) << 16); break;
        case Mn::ANDI_RC: {
            r[in.ra] = r[in.rt] & in.imm;
            set_cr0(r[in.ra]);
            break;
        }
        case Mn::ANDIS_RC: {
            r[in.ra] = r[in.rt] & (static_cast<u64>(in.imm) << 16);
            set_cr0(r[in.ra]);
            break;
        }
        case Mn::CMPI: {
            u64 a = ra_val(in);
            s64 sv = (in.rt & 1) ? static_cast<s64>(a)
                                 : static_cast<s64>(static_cast<s32>(a & 0xFFFFFFFF));
            CrSetField(&ctx->cr, in.crfd,
                       cmp_bits(sv, static_cast<s64>(in.simm)));
            break;
        }
        case Mn::CMPLI: {
            u64 a = ra_val(in);
            u64 b = in.imm;
            if (!(in.rt & 1)) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
            u32 bits = (a < b) ? 8 : (a > b) ? 4 : 2;
            bits |= (ctx->xer >> 31) & 1;
            CrSetField(&ctx->cr, in.crfd, bits);
            break;
        }

        // ---------------- X-form ALU (dest = rt) ----------------
        case Mn::ADD: case Mn::ADDC: r[in.rt] = r[in.ra] + r[in.rb]; break;
        case Mn::SUBF: case Mn::SUBFC: r[in.rt] = r[in.rb] - r[in.ra]; break;
        case Mn::NEG:   r[in.rt] = 0 - r[in.ra]; break;
        case Mn::MULLW:
            r[in.rt] = static_cast<u64>(static_cast<s64>(static_cast<s32>(r[in.ra] & 0xFFFFFFFF)) *
                                        static_cast<s64>(static_cast<s32>(r[in.rb] & 0xFFFFFFFF)));
            break;
        case Mn::MULLD: r[in.rt] = static_cast<s64>(r[in.ra]) * static_cast<s64>(r[in.rb]); break;
        case Mn::DIVW: {
            s64 n = static_cast<s32>(r[in.ra] & 0xFFFFFFFF);
            s64 d = static_cast<s32>(r[in.rb] & 0xFFFFFFFF);
            r[in.rt] = d == 0 ? 0 : static_cast<u32>(n / d);
            break;
        }
        case Mn::DIVWU: {
            u64 n = r[in.ra] & 0xFFFFFFFF, d = r[in.rb] & 0xFFFFFFFF;
            r[in.rt] = d == 0 ? 0 : static_cast<u32>(n / d);
            break;
        }
        case Mn::DIVD: {
            s64 n = static_cast<s64>(r[in.ra]), d = static_cast<s64>(r[in.rb]);
            r[in.rt] = d == 0 ? 0 : static_cast<u64>(n / d);
            break;
        }
        case Mn::DIVDU:
            r[in.rt] = r[in.rb] == 0 ? 0 : r[in.ra] / r[in.rb];
            break;
        case Mn::MULHW: {
            s64 v = (static_cast<s64>(static_cast<s32>(r[in.ra] & 0xFFFFFFFF)) *
                     static_cast<s64>(static_cast<s32>(r[in.rb] & 0xFFFFFFFF))) >> 32;
            r[in.rt] = static_cast<u64>(v);
            break;
        }
        case Mn::MULHWU:
            r[in.rt] = ((r[in.ra] & 0xFFFFFFFF) * (r[in.rb] & 0xFFFFFFFF)) >> 32;
            break;
        // X-form logicals + sign-extends + counts + shifts: PPC is
        // "op rA, rS, rB" — DEST = ra field (bits 11-15), SOURCE = rt field
        // (bits 6-10). (Ground truth: mr r7,r3 = or rA=7,rS=3,rB=3.)
        case Mn::OR:   r[in.ra] = r[in.rt] | r[in.rb]; break;
        case Mn::XOR:  r[in.ra] = r[in.rt] ^ r[in.rb]; break;
        case Mn::AND:  r[in.ra] = r[in.rt] & r[in.rb]; break;
        case Mn::NAND: r[in.ra] = ~(r[in.rt] | r[in.rb]); break;
        case Mn::NOR:  r[in.ra] = ~(r[in.rt] | r[in.rb]); break;
        case Mn::ANDC: r[in.ra] = r[in.rt] & ~r[in.rb]; break;
        case Mn::ORC:  r[in.ra] = r[in.rt] | ~r[in.rb]; break;
        case Mn::EQV:  r[in.ra] = ~(r[in.rt] ^ r[in.rb]); break;
        case Mn::EXTSB: r[in.ra] = static_cast<s8>(r[in.rt] & 0xFF); break;
        case Mn::EXTSH: r[in.ra] = static_cast<s16>(r[in.rt] & 0xFFFF); break;
        case Mn::EXTSW: r[in.ra] = static_cast<s32>(r[in.rt] & 0xFFFFFFFF); break;
        case Mn::CNTLZW: {
            u32 v = static_cast<u32>(r[in.rt]);
            r[in.ra] = v == 0 ? 32 : __builtin_clz(v);
            break;
        }
        case Mn::CNTLZD: r[in.ra] = r[in.rt] == 0 ? 64 : __builtin_clzll(r[in.rt]); break;
        case Mn::SLW: {
            u32 sh = static_cast<u32>(r[in.rb] & 0x3F);
            r[in.ra] = sh < 32 ? (r[in.rt] << sh) : 0;
            break;
        }
        case Mn::SRW: {
            u32 sh = static_cast<u32>(r[in.rb] & 0x3F);
            r[in.ra] = sh < 32 ? ((r[in.rt] & 0xFFFFFFFF) >> sh) : 0;
            break;
        }
        case Mn::SRAW: {
            s64 v = static_cast<s64>(static_cast<s32>(r[in.rt] & 0xFFFFFFFF));
            u32 sh = static_cast<u32>(r[in.rb] & 0x3F);
            r[in.ra] = sh >= 32 ? (v < 0 ? ~0ULL : 0) : static_cast<u64>(v >> sh);
            break;
        }
        case Mn::SRAWI:
            r[in.ra] = static_cast<u64>(static_cast<s32>(r[in.rt] & 0xFFFFFFFF) >> in.sh);
            break;
        case Mn::SLD: {
            u32 sh = static_cast<u32>(r[in.rb] & 0x7F);
            r[in.ra] = sh < 64 ? (r[in.rt] << sh) : 0;
            break;
        }
        case Mn::SRD: {
            u32 sh = static_cast<u32>(r[in.rb] & 0x7F);
            r[in.ra] = sh < 64 ? (r[in.rt] >> sh) : 0;
            break;
        }
        case Mn::SRAD: {
            u32 sh = static_cast<u32>(r[in.rb] & 0x7F);
            s64 v = static_cast<s64>(r[in.rt]);
            r[in.ra] = sh >= 64 ? (v < 0 ? ~0ULL : 0) : static_cast<u64>(v >> sh);
            break;
        }
        case Mn::SRADI:
            r[in.ra] = static_cast<u64>(static_cast<s64>(r[in.rt]) >> in.sh);
            break;
        case Mn::CMP: {
            u64 a = r[in.ra], b = r[in.rb];
            s64 sa, sb;
            if (in.rt & 1) {  // L = 1: 64-bit compare
                sa = static_cast<s64>(a);
                sb = static_cast<s64>(b);
            } else {
                sa = static_cast<s64>(static_cast<s32>(a & 0xFFFFFFFF));
                sb = static_cast<s64>(static_cast<s32>(b & 0xFFFFFFFF));
            }
            CrSetField(&ctx->cr, in.crfd, cmp_bits(sa, sb));
            break;
        }
        case Mn::CMPL: {
            u64 a = r[in.ra], b = r[in.rb];
            if (!(in.rt & 1)) { a &= 0xFFFFFFFF; b &= 0xFFFFFFFF; }
            u32 bits = (a < b) ? 8 : (a > b) ? 4 : 2;
            bits |= (ctx->xer >> 31) & 1;
            CrSetField(&ctx->cr, in.crfd, bits);
            break;
        }

        // ---------------- rotates (M/MD/MDS) ----------------
        case Mn::RLWINM: {
            u32 v = Rotl32(static_cast<u32>(r[in.rt]), in.sh);
            u32 m = RotMask32(in.mb, in.me);
            u32 res = v & m;
            r[in.ra] = res;
            if (in.rcs) set_cr0(res);
            break;
        }
        case Mn::RLWIMI: {
            u32 v = Rotl32(static_cast<u32>(r[in.rt]), in.sh);
            u32 m = RotMask32(in.mb, in.me);
            u32 res = (static_cast<u32>(r[in.ra]) & ~m) | (v & m);
            r[in.ra] = res;
            if (in.rcs) set_cr0(res);
            break;
        }
        case Mn::RLWNM: {
            u32 v = Rotl32(static_cast<u32>(r[in.rt]), static_cast<u32>(r[in.rb] & 0x1F));
            r[in.ra] = v & RotMask32(in.mb, in.me);
            break;
        }
        case Mn::RLDICL: {
            u64 rot = Rotl64(r[in.rt], in.sh);
            r[in.ra] = rot & RldiClMask(in.mb);
            break;
        }
        case Mn::RLDICR: {
            u64 rot = Rotl64(r[in.rt], in.sh);
            // for rldicr the mb field position holds ME
            r[in.ra] = rot & RldiCrMask(in.mb);
            break;
        }
        case Mn::RLDIC: {
            u64 rot = Rotl64(r[in.rt], in.sh);
            u64 m = RldiClMask(in.mb) & RldiCrMask(63 - in.mb);
            r[in.ra] = rot & m;
            break;
        }
        case Mn::RLDIMI: {
            u64 rot = Rotl64(r[in.rt], in.sh);
            u64 m = RldiClMask(in.mb) & RldiCrMask(63 - in.mb);
            r[in.ra] = (r[in.ra] & ~m) | (rot & m);
            break;
        }
        case Mn::RLDCL: {
            u64 rot = Rotl64(r[in.rt], r[in.rb] & 63);
            r[in.ra] = rot & RldiClMask(in.mb);
            break;
        }
        case Mn::RLDCR: {
            u64 rot = Rotl64(r[in.rt], r[in.rb] & 63);
            r[in.ra] = rot & RldiCrMask(in.mb);
            break;
        }

        // ---------------- CR ops ----------------
        case Mn::CRAND: case Mn::CRANDC: case Mn::CREQV: case Mn::CRNAND:
        case Mn::CRNOR: case Mn::CROR: case Mn::CRORC: case Mn::CRXOR: {
            u32 a = (ctx->cr >> (31 - in.ra)) & 1;
            u32 b = (ctx->cr >> (31 - in.rb)) & 1;
            u32 v = 0;
            switch (in.mn) {
                case Mn::CRAND:  v = a & b; break;
                case Mn::CRANDC: v = a & (1 - b); break;
                case Mn::CREQV:  v = 1 - (a ^ b); break;
                case Mn::CRNAND: v = 1 - (a & b); break;
                case Mn::CRNOR:  v = 1 - (a | b); break;
                case Mn::CROR:   v = a | b; break;
                case Mn::CRORC:  v = a | (1 - b); break;
                case Mn::CRXOR:  v = a ^ b; break;
                default: break;
            }
            u32 mask = 1u << (31 - in.rt);
            ctx->cr = (ctx->cr & ~mask) | (v << (31 - in.rt));
            break;
        }
        case Mn::MFCR: r[in.rt] = ctx->cr; break;
        case Mn::MTCRF: {
            u32 crm = in.mb;  // 8-bit CRM (decode.cpp stores it in mb)
            u32 v = static_cast<u32>(r[in.rt]);
            for (u32 f = 0; f < 8; f++) {
                if (crm & (0x80u >> f)) CrSetField(&ctx->cr, f, (v >> (28 - f * 4)) & 0xF);
            }
            break;
        }
        case Mn::MCRF: {
            u32 v = (ctx->cr >> (28 - in.crfs * 4)) & 0xF;
            CrSetField(&ctx->cr, in.crfd, v);
            break;
        }

        // ---------------- SPR ----------------
        case Mn::MFSPR:
            switch (in.spr) {
                case 8:  r[in.rt] = ctx->lr; break;
                case 9:  r[in.rt] = ctx->ctr; break;
                case 1:  r[in.rt] = ctx->xer; break;
                default:
                    PpuRaise(ctx, "mfspr: unsupported SPR " + std::to_string(in.spr));
                    return false;
            }
            break;
        case Mn::MTSPR:
            switch (in.spr) {
                case 8:  ctx->lr = r[in.rt]; break;
                case 9:  ctx->ctr = r[in.rt]; break;
                case 1:  ctx->xer = static_cast<u32>(r[in.rt]); break;
                default:
                    PpuRaise(ctx, "mtspr: unsupported SPR " + std::to_string(in.spr));
                    return false;
            }
            break;

        // ---------------- loads ----------------
        case Mn::LWZ: case Mn::LWZU: {
            u32 a = ea_d(in);
            if (in.mn == Mn::LWZU) r[in.ra] = a;
            r[in.rt] = vm.Read32(a);
            break;
        }
        case Mn::LBZ: case Mn::LBZU: r[in.rt] = vm.Read8(ea_d(in)); break;
        case Mn::LHZ: case Mn::LHZU: r[in.rt] = vm.Read16(ea_d(in)); break;
        case Mn::LHA: case Mn::LHAU: r[in.rt] = static_cast<s16>(vm.Read16(ea_d(in))); break;
        case Mn::STW: case Mn::STWU: {
            u32 a = ea_d(in);
            if (in.mn == Mn::STWU) r[in.ra] = a;
            vm.Write32(a, static_cast<u32>(r[in.rt]));
            break;
        }
        case Mn::STB: case Mn::STBU: vm.Write8(ea_d(in), static_cast<u8>(r[in.rt])); break;
        case Mn::STH: case Mn::STHU: vm.Write16(ea_d(in), static_cast<u16>(r[in.rt])); break;
        case Mn::LWZX: r[in.rt] = vm.Read32(ea_x(in)); break;
        case Mn::LBZX: r[in.rt] = vm.Read8(ea_x(in)); break;
        case Mn::LHZX: r[in.rt] = vm.Read16(ea_x(in)); break;
        case Mn::LHAX: r[in.rt] = static_cast<s16>(vm.Read16(ea_x(in))); break;
        case Mn::LWAX: r[in.rt] = static_cast<s64>(static_cast<s32>(vm.Read32(ea_x(in)))); break;
        case Mn::STWX: vm.Write32(ea_x(in), static_cast<u32>(r[in.rt])); break;
        case Mn::STBX: vm.Write8(ea_x(in), static_cast<u8>(r[in.rt])); break;
        case Mn::STHX: vm.Write16(ea_x(in), static_cast<u16>(r[in.rt])); break;
        case Mn::LD: case Mn::LDU: {
            u32 a = ea_d(in);
            if (in.mn == Mn::LDU) r[in.ra] = a;
            r[in.rt] = vm.Read64(a);
            break;
        }
        case Mn::LWA: r[in.rt] = static_cast<s64>(static_cast<s32>(vm.Read32(ea_d(in)))); break;
        case Mn::STD: case Mn::STDU: {
            u32 a = ea_d(in);
            if (in.mn == Mn::STDU) r[in.ra] = a;
            vm.Write64(a, r[in.rt]);
            break;
        }
        case Mn::LDX: case Mn::LDUX: r[in.rt] = vm.Read64(ea_x(in)); break;
        case Mn::STDX: case Mn::STDUX: vm.Write64(ea_x(in), r[in.rt]); break;
        case Mn::LWBRX: r[in.rt] = __builtin_bswap32(vm.Read32(ea_x(in))); break;
        case Mn::STWBRX: vm.Write32(ea_x(in), __builtin_bswap32(static_cast<u32>(r[in.rt]))); break;
        case Mn::LFS: {
            u32 v32 = vm.Read32(ea_d(in));
            u32 sign = (v32 >> 31) & 1, exp = (v32 >> 23) & 0xFF, man = v32 & 0x7FFFFF;
            u64 d = static_cast<u64>(sign) << 63;
            if (exp == 0xFF) d |= 0x7FF0000000000000ULL | (static_cast<u64>(man) << 29);
            else if (exp == 0 && man == 0) { /* zero */ }
            else if (exp == 0) d |= (1ULL << 52) | (static_cast<u64>(man) << 29);  // subnormal
            else d |= (static_cast<u64>(exp + 896) << 52) | (static_cast<u64>(man) << 29);
            ctx->fpr[in.rt] = d;
            break;
        }
        case Mn::LFD: ctx->fpr[in.rt] = vm.Read64(ea_d(in)); break;
        case Mn::STFD: vm.Write64(ea_d(in), ctx->fpr[in.rt]); break;
        case Mn::STFS: {
            u64 d = ctx->fpr[in.rt];
            u32 sign = static_cast<u32>(d >> 63) & 1;
            u32 exp = static_cast<u32>(d >> 52) & 0x7FF;
            u32 man = static_cast<u32>(d >> 29) & 0x7FFFFF;
            u32 v32 = sign << 31;
            if (exp == 0x7FF) v32 |= 0x7F800000 | man;          // NaN/Inf
            else if (exp - 896 > 254) v32 |= 0x7F800000;        // true overflow
            else if (exp >= 896) v32 |= ((exp - 896) << 23) | man;
            else if (exp >= 874) v32 |= (exp - 874) << 23;      // subnormal (truncated)
            vm.Write32(ea_d(in), v32);
            break;
        }
        case Mn::DCBZ: {
            u32 a = ea_x(in) & ~0x7Fu;  // PPU cache line = 128 bytes
            u8* p = vm.Ptr(a, 128);
            if (p) std::memset(p, 0, 128);
            else PpuRaise(ctx, "dcbz outside guest memory");
            break;
        }

        // ---------------- branches ----------------
        case Mn::B: case Mn::BL:
            if (in.lk) ctx->lr = next_pc;
            new_pc = in.target;
            break;
        case Mn::BC: case Mn::BCL: case Mn::BCA: case Mn::BCAL: {
            bool taken = PpuEvaluateCondition(*ctx, in.bo, in.bi);
            if (in.lk) ctx->lr = next_pc;
            if (taken) new_pc = in.target;
            break;
        }
        case Mn::BCLR: case Mn::BCLRL: {
            bool taken = PpuEvaluateCondition(*ctx, in.bo, in.bi);
            if (in.lk) ctx->lr = next_pc;
            if (taken) new_pc = static_cast<u32>(ctx->lr);
            break;
        }
        case Mn::BCTR: case Mn::BCTRL:
            if (in.lk) ctx->lr = next_pc;
            new_pc = static_cast<u32>(ctx->ctr);
            break;

        // ---------------- system ----------------
        case Mn::SC:
            if (!ctx->syscall_fn) {
                PpuRaise(ctx, "sc executed without an LV2 syscall handler");
                return false;
            }
            ctx->scount++;
            ctx->syscall_fn(ctx);
            if (ctx->exit || ctx->error) return !ctx->error;
            if (ctx->replay_sc) {
                // Blocking syscall (join) not satisfied yet: re-execute the
                // same sc on the next run instead of advancing.
                ctx->replay_sc = false;
                new_pc = pc;
            }
            break;
        case Mn::TWI: case Mn::TW: {
            s64 a = static_cast<s32>(ra_val(in) & 0xFFFFFFFF);
            s64 b = (in.mn == Mn::TWI) ? in.simm
                                       : static_cast<s32>(r[in.rb] & 0xFFFFFFFF);
            u32 to = in.to;
            bool trap = ((to & 0x10) && a < b) || ((to & 0x08) && a > b) ||
                        ((to & 0x04) && a == b) ||
                        ((to & 0x02) && (u64)(a & 0xFFFFFFFF) < (u64)(b & 0xFFFFFFFF)) ||
                        ((to & 0x01) && (u64)(a & 0xFFFFFFFF) > (u64)(b & 0xFFFFFFFF));
            if (trap) {
                ctx->exit = true;
                ctx->exit_code = 0x80;
            }
            break;
        }
        case Mn::TDI: case Mn::TD: {
            u64 a = ra_val(in);
            u64 b = (in.mn == Mn::TDI) ? static_cast<u64>(static_cast<s32>(in.simm)) : r[in.rb];
            u32 to = in.to;
            s64 sa = static_cast<s64>(a), sb = static_cast<s64>(b);
            bool trap = ((to & 0x10) && sa < sb) || ((to & 0x08) && sa > sb) ||
                        ((to & 0x04) && a == b) || ((to & 0x02) && a < b) ||
                        ((to & 0x01) && a > b);
            if (trap) {
                ctx->exit = true;
                ctx->exit_code = 0x80;
            }
            break;
        }

        // ---------------- reservations (synchronisation) ----------------
        case Mn::LWARX: {
            u32 a = ea_x(in);
            r[in.rt] = vm.Read32(a);
            ctx->res_addr = a;
            ctx->res_valid = true;
            break;
        }
        case Mn::LDARX: {
            u32 a = ea_x(in);
            r[in.rt] = vm.Read64(a);
            ctx->res_addr = a;
            ctx->res_valid = true;
            break;
        }
        case Mn::STWCX: {
            u32 a = ea_x(in);
            bool ok = ctx->res_valid && (ctx->res_addr & ~3ull) == (a & ~3ull);
            if (ok) vm.Write32(a, static_cast<u32>(r[in.rt]));
            ctx->res_valid = false;  // stwcx ALWAYS clears the reservation
            CrSetField(&ctx->cr, 0, (ok ? 2u : 0u) | ((ctx->xer >> 31) & 1));
            break;
        }
        case Mn::STDCX: {
            u32 a = ea_x(in);
            bool ok = ctx->res_valid && (ctx->res_addr & ~7ull) == (a & ~7ull);
            if (ok) vm.Write64(a, r[in.rt]);
            ctx->res_valid = false;
            CrSetField(&ctx->cr, 0, (ok ? 2u : 0u) | ((ctx->xer >> 31) & 1));
            break;
        }

        // ---------------- floating point ----------------
        case Mn::LFSU: case Mn::LFSX: case Mn::LFSUX: {
            u32 a = (in.mn == Mn::LFSU) ? ea_d(in) : ea_x(in);
            u32 v32 = vm.Read32(a);
            u32 sign = (v32 >> 31) & 1, exp = (v32 >> 23) & 0xFF, man = v32 & 0x7FFFFF;
            u64 d = static_cast<u64>(sign) << 63;
            if (exp == 0xFF) d |= 0x7FF0000000000000ULL | (static_cast<u64>(man) << 29);
            else if (exp == 0 && man == 0) { /* zero */ }
            else if (exp == 0) d |= (1ULL << 52) | (static_cast<u64>(man) << 29);
            else d |= (static_cast<u64>(exp + 896) << 52) | (static_cast<u64>(man) << 29);
            ctx->fpr[in.rt] = d;
            if (in.mn == Mn::LFSU || in.mn == Mn::LFSUX) r[in.ra] = a;
            break;
        }
        case Mn::LFDU: case Mn::LFDX: case Mn::LFDUX: {
            u32 a = (in.mn == Mn::LFDU) ? ea_d(in) : ea_x(in);
            ctx->fpr[in.rt] = vm.Read64(a);
            if (in.mn == Mn::LFDU || in.mn == Mn::LFDUX) r[in.ra] = a;
            break;
        }
        case Mn::STFSU: case Mn::STFSX: case Mn::STFSUX: {
            u32 a = (in.mn == Mn::STFSU) ? ea_d(in) : ea_x(in);
            u64 d = ctx->fpr[in.rt];
            u32 sign = static_cast<u32>(d >> 63) & 1;
            u32 exp = static_cast<u32>(d >> 52) & 0x7FF;
            u32 man = static_cast<u32>(d >> 29) & 0x7FFFFF;
            u32 v32 = sign << 31;
            // double exp is 1023-biased, single exp is 127-biased:
            // single_exp = exp - 896. Overflow to inf only past 254.
            if (exp == 0x7FF) v32 |= 0x7F800000 | man;          // NaN/Inf
            else if (exp == 0) { /* zero (subnormal doubles truncate to 0) */ }
            else if (exp - 896 > 254) v32 |= 0x7F800000;        // true overflow
            else if (exp >= 896) v32 |= ((exp - 896) << 23) | man;
            else if (exp >= 874) v32 |= (exp - 874) << 23;      // subnormal (truncated)
            vm.Write32(a, v32);
            if (in.mn == Mn::STFSU || in.mn == Mn::STFSUX) r[in.ra] = a;
            break;
        }
        case Mn::STFDU: case Mn::STFDX: case Mn::STFDUX: {
            u32 a = (in.mn == Mn::STFDU) ? ea_d(in) : ea_x(in);
            vm.Write64(a, ctx->fpr[in.rt]);
            if (in.mn == Mn::STFDU || in.mn == Mn::STFDUX) r[in.ra] = a;
            break;
        }

        // FP compare sets the CR field: LT GT EQ or UN (unordered bit).
        case Mn::FCMPU: case Mn::FCMPO: {
            double a = FprD(*ctx, in.ra), b = FprD(*ctx, in.rb);
            u32 bits;
            if (std::isnan(a) || std::isnan(b)) bits = 1;  // FU (unordered)
            else bits = (a < b) ? 8 : (a > b) ? 4 : 2;
            CrSetField(&ctx->cr, in.crfd, bits);
            break;
        }

        // double-precision arithmetic. PPC A-form operand ORDER matters:
        //   fadd/fsub/fdiv: fD,fA,fB   -> ra, rb
        //   fmul:           fD,fA,fC   -> ra, mb   (frB is ZERO in the encoding!)
        //   fmadd family:   fD,fA,fC,fB -> ra, mb, rb  (a*c + b)
        //   fsqrt/fres/frsqrte: fD,fB  -> rb
        //   fsel:           fD,fA,fC,fB -> ra(cond), mb(if>=0), rb(if<0)
        case Mn::FADD: case Mn::FSUB: case Mn::FMUL: case Mn::FDIV: case Mn::FSQRT: {
            double res = 0;
            switch (in.mn) {
                case Mn::FADD:  res = FprD(*ctx, in.ra) + FprD(*ctx, in.rb); break;
                case Mn::FSUB:  res = FprD(*ctx, in.ra) - FprD(*ctx, in.rb); break;
                case Mn::FMUL:  res = FprD(*ctx, in.ra) * FprD(*ctx, in.mb); break;
                case Mn::FDIV: {
                    double n = FprD(*ctx, in.ra), d = FprD(*ctx, in.rb);
                    res = d != 0 ? n / d : std::numeric_limits<double>::infinity();
                    break;
                }
                default:        res = std::sqrt(FprD(*ctx, in.rb)); break;  // FSQRT
            }
            SetFprD(ctx, in.rt, res);
            if (in.rcs) CrSetField(&ctx->cr, 1, (ctx->fpscr >> 16) & 0xF);
            break;
        }
        case Mn::FMADD: case Mn::FMSUB: case Mn::FNMADD: case Mn::FNMSUB: {
            // PPC multiply-add is FUSED (single rounding) — use fma.
            // fD = fA * fC + fB  (fC in mb, fB in rb).
            double a = FprD(*ctx, in.ra), c = FprD(*ctx, in.mb), b = FprD(*ctx, in.rb);
            double res;
            switch (in.mn) {
                case Mn::FMADD:  res = std::fma(a, c, b); break;
                case Mn::FMSUB:  res = std::fma(a, c, -b); break;
                case Mn::FNMADD: res = -std::fma(a, c, b); break;
                default:         res = -std::fma(a, c, -b); break;  // FNMSUB
            }
            SetFprD(ctx, in.rt, res);
            if (in.rcs) CrSetField(&ctx->cr, 1, (ctx->fpscr >> 16) & 0xF);
            break;
        }
        case Mn::FRSQRTE: {
            double b = FprD(*ctx, in.rb);
            // honest estimate: PPC defines frsqrte as a HW estimate; we compute
            // the exact reciprocal square root (documented difference).
            SetFprD(ctx, in.rt, b > 0 ? 1.0 / std::sqrt(b) : std::numeric_limits<double>::infinity());
            break;
        }
        case Mn::FSEL: {
            double a = FprD(*ctx, in.ra);
            SetFprD(ctx, in.rt, a >= 0 ? FprD(*ctx, in.mb) : FprD(*ctx, in.rb));
            break;
        }

        // single-precision arithmetic: operands converted to single, result
        // rounded to single (per PPC single-precision semantics)
        case Mn::FADDS: case Mn::FSUBS: case Mn::FMULS: case Mn::FDIVS: case Mn::FSQRTS:
        case Mn::FRES: {
            float res = 0;
            switch (in.mn) {
                case Mn::FADDS:  res = FprS(*ctx, in.ra) + FprS(*ctx, in.rb); break;
                case Mn::FSUBS:  res = FprS(*ctx, in.ra) - FprS(*ctx, in.rb); break;
                case Mn::FMULS:  res = FprS(*ctx, in.ra) * FprS(*ctx, in.mb); break;
                case Mn::FDIVS: {
                    float n = FprS(*ctx, in.ra), d = FprS(*ctx, in.rb);
                    res = d != 0 ? n / d : std::numeric_limits<float>::infinity();
                    break;
                }
                case Mn::FSQRTS: res = std::sqrt(FprS(*ctx, in.rb)); break;
                default: {  // FRES (fD, fB)
                    float b = FprS(*ctx, in.rb);
                    res = b != 0 ? 1.0f / b : std::numeric_limits<float>::infinity();
                    break;
                }
            }
            SetFprD(ctx, in.rt, static_cast<double>(res));
            if (in.rcs) CrSetField(&ctx->cr, 1, (ctx->fpscr >> 16) & 0xF);
            break;
        }
        case Mn::FMADDS: case Mn::FMSUBS: case Mn::FNMADDS: case Mn::FNMSUBS: {
            // fD = fA * fC + fB, single-precision fused (fC in mb).
            float a = FprS(*ctx, in.ra), c = FprS(*ctx, in.mb), b = FprS(*ctx, in.rb);
            float res;
            switch (in.mn) {
                case Mn::FMADDS:  res = std::fma(a, c, b); break;
                case Mn::FMSUBS:  res = std::fma(a, c, -b); break;
                case Mn::FNMADDS: res = -std::fma(a, c, b); break;
                default:          res = -std::fma(a, c, -b); break;  // FNMSUBS
            }
            SetFprD(ctx, in.rt, static_cast<double>(res));
            if (in.rcs) CrSetField(&ctx->cr, 1, (ctx->fpscr >> 16) & 0xF);
            break;
        }

        // FP moves / converts
        case Mn::FRSP: {
            SetFprD(ctx, in.rt, static_cast<double>(FprS(*ctx, in.rb)));  // fD, fB
            if (in.rcs) CrSetField(&ctx->cr, 1, (ctx->fpscr >> 16) & 0xF);
            break;
        }
        case Mn::FCTIW: {
            // round per FPSCR RN (we keep the default round-to-nearest)
            double a = FprD(*ctx, in.rb);
            long long v = std::isnan(a) ? 0x7FFFFFFFll
                        : std::nearbyint(a);  // inf handled by cast overflow below
            u32 word = (v > 2147483647ll || v < -2147483648ll)
                           ? (a < 0 ? 0x80000000u : 0x7FFFFFFFu)
                           : static_cast<u32>(v);
            ctx->fpr[in.rt] = word;  // low word = result, high word zero
            break;
        }
        case Mn::FCTIWZ: {
            double a = FprD(*ctx, in.rb);
            long long v = std::isnan(a) ? 0x7FFFFFFFll : static_cast<long long>(std::trunc(a));
            u32 word = (v > 2147483647ll || v < -2147483648ll)
                           ? (a < 0 ? 0x80000000u : 0x7FFFFFFFu)
                           : static_cast<u32>(v);
            ctx->fpr[in.rt] = word;
            break;
        }
        // X-form FP moves: the source operand is frB (bits 16-20), NOT frA
        // (binutils-verified: fneg f11,f5 encodes 5 in frB).
        case Mn::FNEG: {
            ctx->fpr[in.rt] = ctx->fpr[in.rb] ^ (1ull << 63);
            if (in.rcs) CrSetField(&ctx->cr, 1, (ctx->fpscr >> 16) & 0xF);
            break;
        }
        case Mn::FABS: {
            ctx->fpr[in.rt] = ctx->fpr[in.rb] & ~(1ull << 63);
            if (in.rcs) CrSetField(&ctx->cr, 1, (ctx->fpscr >> 16) & 0xF);
            break;
        }
        case Mn::FNABS: {
            ctx->fpr[in.rt] = ctx->fpr[in.rb] | (1ull << 63);
            break;
        }
        case Mn::FMR: {
            ctx->fpr[in.rt] = ctx->fpr[in.rb];  // fD, fB
            if (in.rcs) CrSetField(&ctx->cr, 1, (ctx->fpscr >> 16) & 0xF);
            break;
        }
        case Mn::MFFS: {
            ctx->fpr[in.rt] = ctx->fpscr;
            break;
        }
        case Mn::MTFSB0: {
            u32 bit = in.crfd;  // raw crfD field index (0..7): bit number
            ctx->fpscr &= ~(1u << bit);
            break;
        }
        case Mn::MTFSB1: {
            u32 bit = in.crfd;
            ctx->fpscr |= (1u << bit);
            break;
        }
        case Mn::MTFSF: {
            u32 fm = in.mb;  // 8-bit field mask (binutils-verified position)
            u32 v = static_cast<u32>(ctx->fpr[in.rb]);
            u32 mask = 0;
            for (u32 i = 0; i < 8; i++) {
                if (fm & (0x80u >> i)) mask |= 0xFu << (28 - i * 4);
            }
            ctx->fpscr = (ctx->fpscr & ~mask) | (v & mask);
            break;
        }
        case Mn::MCRFS: {
            u32 v = (ctx->fpscr >> (28 - in.crfs * 4)) & 0xF;
            CrSetField(&ctx->cr, in.crfd, v);
            break;
        }

        // ---------------- VMX / AltiVec subset ----------------
        case Mn::LVX: case Mn::LVXL: {
            u32 a = ea_x(in) & ~7u;  // lvx is 16-byte aligned in practice; mask
            // PPC lvx ignores the low 4 bits of the EA (16-byte alignment)
            a &= ~15u;
            const u8* p = vm.Ptr(a, 16);
            if (!p) { PpuRaise(ctx, "lvx outside guest memory"); return false; }
            std::memcpy(ctx->vpr[in.rt], p, 16);
            break;
        }
        case Mn::STVX: case Mn::STVXL: {
            u32 a = ea_x(in) & ~15u;
            u8* p = vm.Ptr(a, 16);
            if (!p) { PpuRaise(ctx, "stvx outside guest memory"); return false; }
            std::memcpy(p, ctx->vpr[in.rt], 16);
            break;
        }
        // vspltis* encode the signed 5-bit immediate in the vA field
        // (bits 11-15, binutils-verified: vspltisb v2,7 -> ra=7).
        case Mn::VSPLTISB: {
            s8 v = static_cast<s8>(in.ra << 3) >> 3;  // sign-extend 5-bit imm
            std::memset(ctx->vpr[in.rt], static_cast<u8>(v), 16);
            break;
        }
        case Mn::VSPLTISH: {
            s16 v = static_cast<s16>(in.ra << 11) >> 11;
            for (int i = 0; i < 8; i++) {
                ctx->vpr[in.rt][i * 2] = static_cast<u8>(v >> 8);
                ctx->vpr[in.rt][i * 2 + 1] = static_cast<u8>(v);
            }
            break;
        }
        case Mn::VSPLTISW: {
            s32 v = static_cast<s32>(in.ra << 27) >> 27;
            for (int lane = 0; lane < 4; lane++) VSetLaneU32(ctx, in.rt, lane, static_cast<u32>(v));
            break;
        }
        case Mn::VAND: case Mn::VANDC: case Mn::VOR: case Mn::VXOR: case Mn::VNOR: {
            const u8* a = ctx->vpr[in.ra];
            const u8* b = ctx->vpr[in.rb];
            u8* d = ctx->vpr[in.rt];
            for (int i = 0; i < 16; i++) {
                switch (in.mn) {
                    case Mn::VAND:  d[i] = a[i] & b[i]; break;
                    case Mn::VANDC: d[i] = a[i] & ~b[i]; break;
                    case Mn::VOR:   d[i] = a[i] | b[i]; break;
                    case Mn::VXOR:  d[i] = a[i] ^ b[i]; break;
                    default:        d[i] = ~(a[i] | b[i]); break;  // VNOR
                }
            }
            break;
        }
        case Mn::VADDUBM: case Mn::VSUBUBM: {
            const u8* a = ctx->vpr[in.ra];
            const u8* b = ctx->vpr[in.rb];
            u8* d = ctx->vpr[in.rt];
            for (int i = 0; i < 16; i++)
                d[i] = (in.mn == Mn::VADDUBM) ? static_cast<u8>(a[i] + b[i])
                                               : static_cast<u8>(a[i] - b[i]);
            break;
        }
        case Mn::VADDUHM: case Mn::VSUBUHM: {
            for (int lane = 0; lane < 8; lane++) {
                const u8* ap = ctx->vpr[in.ra] + lane * 2;
                const u8* bp = ctx->vpr[in.rb] + lane * 2;
                u16 x = (static_cast<u16>(ap[0]) << 8) | ap[1];
                u16 y = (static_cast<u16>(bp[0]) << 8) | bp[1];
                u16 res = (in.mn == Mn::VADDUHM) ? static_cast<u16>(x + y)
                                                 : static_cast<u16>(x - y);
                u8* dp = ctx->vpr[in.rt] + lane * 2;
                dp[0] = static_cast<u8>(res >> 8);
                dp[1] = static_cast<u8>(res);
            }
            break;
        }
        case Mn::VADDUWM: case Mn::VSUBUWM: {
            for (int lane = 0; lane < 4; lane++) {
                u32 x = VLaneU32(*ctx, in.ra, lane);
                u32 y = VLaneU32(*ctx, in.rb, lane);
                u32 res = (in.mn == Mn::VADDUWM) ? x + y : x - y;
                VSetLaneU32(ctx, in.rt, lane, res);
            }
            break;
        }
        case Mn::VMADDFP: case Mn::VNMSUBFP: {
            // PPC vmaddfp d = (vA * vC) + vB — vC is the MULTIPLIER (mb field),
            // vB is the ADDEND (rb field). Assembly order is vD,vA,vC,vB
            // (binutils-verified). Multiply, then add — each op rounded (not fused).
            for (int lane = 0; lane < 4; lane++) {
                float a = VLaneF32(*ctx, in.ra, lane);   // vA
                float c = VLaneF32(*ctx, in.mb, lane);   // vC (multiplier)
                float b = VLaneF32(*ctx, in.rb, lane);   // vB (addend)
                float res = (in.mn == Mn::VMADDFP) ? (a * c + b)
                                                   : -(a * c - b);
                VSetLaneF32(ctx, in.rt, lane, res);
            }
            break;
        }
        case Mn::VSEL: {
            const u8* a = ctx->vpr[in.ra];
            const u8* b = ctx->vpr[in.rb];
            const u8* c = ctx->vpr[in.mb];  // control
            u8* d = ctx->vpr[in.rt];
            for (int i = 0; i < 16; i++) d[i] = (a[i] & b[i]) | (c[i] & ~b[i]);
            break;
        }
        case Mn::VPERM: {
            // 32-byte source = {vA || vB}; index i = vC[i] & 0x1F
            const u8* a = ctx->vpr[in.ra];
            const u8* b = ctx->vpr[in.rb];
            const u8* c = ctx->vpr[in.mb];
            u8* d = ctx->vpr[in.rt];
            for (int i = 0; i < 16; i++) {
                u32 idx = c[i] & 0x1F;
                d[i] = idx < 16 ? a[idx] : b[idx - 16];
            }
            break;
        }
        case Mn::VMRGHW: case Mn::VMRGLW: {
            int base = (in.mn == Mn::VMRGHW) ? 0 : 2;
            for (int k = 0; k < 2; k++) {
                VSetLaneU32(ctx, in.rt, k * 2, VLaneU32(*ctx, in.ra, base + k));
                VSetLaneU32(ctx, in.rt, k * 2 + 1, VLaneU32(*ctx, in.rb, base + k));
            }
            break;
        }
        case Mn::MFVSCR: {
            VSetLaneU32(ctx, in.rt, 0, ctx->vscr);
            break;
        }
        case Mn::MTVSCR: {
            ctx->vscr = VLaneU32(*ctx, in.rb, 0);
            break;
        }

        default:
            PpuRaise(ctx, "interpreter: mnemonic not implemented (mn=" +
                              std::to_string(static_cast<u32>(in.mn)) + ")");
            return false;
    }

    ctx->pc = new_pc;
    return !ctx->error;
}

int PpuRun(PpuContext* ctx) {
    while (!ctx->exit && !ctx->error) {
        if (!PpuStep(ctx)) break;
        if (ctx->icount > 500000000ULL) {
            PpuRaise(ctx, "instruction budget exceeded");
            break;
        }
    }
    return static_cast<int>(ctx->exit_code);
}

}  // namespace anyps3
