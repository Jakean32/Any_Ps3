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
    r[0] = 0;  // hardwired by convention (RA=0 immediate base)
    const u32 next_pc = pc + 4;
    u32 new_pc = next_pc;

    auto ra_val = [&](const PpuInstr& i) -> u64 { return i.ra == 0 ? 0 : r[i.ra]; };
    auto ea_d = [&](const PpuInstr& i) -> u32 {
        return static_cast<u32>(ra_val(i) + static_cast<s64>(i.simm));
    };
    auto ea_x = [&](const PpuInstr& i) -> u32 {
        return static_cast<u32>(ra_val(i) + (i.rb == 0 ? 0 : r[i.rb]));
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
        case Mn::ORI:   r[in.rt] = ra_val(in) | in.imm; break;
        case Mn::ORIS:  r[in.rt] = ra_val(in) | (static_cast<u64>(in.imm) << 16); break;
        case Mn::XORI:  r[in.rt] = ra_val(in) ^ in.imm; break;
        case Mn::XORIS: r[in.rt] = ra_val(in) ^ (static_cast<u64>(in.imm) << 16); break;
        case Mn::ANDI_RC:
            r[in.rt] = ra_val(in) & in.imm;
            set_cr0(r[in.rt]);
            break;
        case Mn::ANDIS_RC:
            r[in.rt] = ra_val(in) & (static_cast<u64>(in.imm) << 16);
            set_cr0(r[in.rt]);
            break;
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
        case Mn::OR:   r[in.rt] = r[in.ra] | r[in.rb]; break;
        case Mn::XOR:  r[in.rt] = r[in.ra] ^ r[in.rb]; break;
        case Mn::AND:  r[in.rt] = r[in.ra] & r[in.rb]; break;
        case Mn::NAND: r[in.rt] = ~(r[in.ra] | r[in.rb]); break;
        case Mn::NOR:  r[in.rt] = ~(r[in.ra] | r[in.rb]); break;
        case Mn::ANDC: r[in.rt] = r[in.ra] & ~r[in.rb]; break;
        case Mn::ORC:  r[in.rt] = r[in.ra] | ~r[in.rb]; break;
        case Mn::EQV:  r[in.rt] = ~(r[in.ra] ^ r[in.rb]); break;
        case Mn::EXTSB: r[in.rt] = static_cast<s8>(r[in.ra] & 0xFF); break;
        case Mn::EXTSH: r[in.rt] = static_cast<s16>(r[in.ra] & 0xFFFF); break;
        case Mn::EXTSW: r[in.rt] = static_cast<s32>(r[in.ra] & 0xFFFFFFFF); break;
        case Mn::CNTLZW: {
            u32 v = static_cast<u32>(r[in.ra]);
            r[in.rt] = v == 0 ? 32 : __builtin_clz(v);
            break;
        }
        case Mn::CNTLZD: r[in.rt] = r[in.ra] == 0 ? 64 : __builtin_clzll(r[in.ra]); break;
        case Mn::SLW: {
            u32 sh = static_cast<u32>(r[in.rb] & 0x3F);
            r[in.rt] = sh < 32 ? (r[in.ra] << sh) : 0;
            break;
        }
        case Mn::SRW: {
            u32 sh = static_cast<u32>(r[in.rb] & 0x3F);
            r[in.rt] = sh < 32 ? ((r[in.ra] & 0xFFFFFFFF) >> sh) : 0;
            break;
        }
        case Mn::SRAW: {
            s64 v = static_cast<s64>(static_cast<s32>(r[in.ra] & 0xFFFFFFFF));
            u32 sh = static_cast<u32>(r[in.rb] & 0x3F);
            r[in.rt] = sh >= 32 ? (v < 0 ? ~0ULL : 0) : static_cast<u64>(v >> sh);
            break;
        }
        case Mn::SRAWI:
            r[in.rt] = static_cast<u64>(static_cast<s32>(r[in.ra] & 0xFFFFFFFF) >> in.sh);
            break;
        case Mn::SLD: {
            u32 sh = static_cast<u32>(r[in.rb] & 0x7F);
            r[in.rt] = sh < 64 ? (r[in.ra] << sh) : 0;
            break;
        }
        case Mn::SRD: {
            u32 sh = static_cast<u32>(r[in.rb] & 0x7F);
            r[in.rt] = sh < 64 ? (r[in.ra] >> sh) : 0;
            break;
        }
        case Mn::SRAD: {
            u32 sh = static_cast<u32>(r[in.rb] & 0x7F);
            s64 v = static_cast<s64>(r[in.ra]);
            r[in.rt] = sh >= 64 ? (v < 0 ? ~0ULL : 0) : static_cast<u64>(v >> sh);
            break;
        }
        case Mn::SRADI:
            r[in.rt] = static_cast<u64>(static_cast<s64>(r[in.ra]) >> in.sh);
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
            if (exp == 0x7FF) v32 |= 0x7F800000 | man;
            else if (exp > 896 + 127) v32 |= 0x7F800000;  // inf
            else if (exp >= 896) v32 |= ((exp - 896) << 23) | man;
            else if (exp >= 874) v32 |= (exp - 874) << 23;  // subnormal (truncated)
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
