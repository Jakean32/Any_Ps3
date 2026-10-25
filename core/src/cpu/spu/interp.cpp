// Any_Ps3 — SPU interpreter. Executes the decoded subset with real channel
// semantics: blocking mailboxes, MFC DMA (synchronous simplification — real
// hardware is asynchronous; see PHASE_REPORTS.md), signal notification regs.
#include "anyps3/cpu/spu/state.h"

#include <cstring>

#include "anyps3/cpu/spu/decode.h"
#include "anyps3/log.h"

namespace anyps3 {

// Channel IDs (Cell ISA):
//   3/4 SPU_RdSigNotify1/2, 7 SPU_WrDec, 8 SPU_RdDec, 16..21 MFC_LSA..MFC_Cmd,
//   22 MFC_WrTagMask, 23 MFC_WrTagUpdate, 24 MFC_RdTagStat,
//   28 SPU_WrOutMbox, 29 SPU_RdInMbox, 30 SPU_WrOutIntrMbox
SpuThread::~SpuThread() = default;

u32 SpuThread::ChCount(u32 ch, std::string* err) {
    switch (ch) {
        case 28: {  // out mbox writable (1 slot)
            std::unique_lock<std::mutex> lk(ch_lock);
            return out_mbox_valid ? 0 : 1;
        }
        case 29: {  // inbound message count
            std::unique_lock<std::mutex> lk(ch_lock);
            return static_cast<u32>(in_mbox.size());
        }
        case 24: return 1;  // MFC_RdTagStat: always complete (synchronous DMA)
        case 30: return 1;  // interrupt mailbox writable
        case 3: case 4: return 1;
        default:
            *err = "rchcnt: unsupported channel " + std::to_string(ch);
            return 0xFFFFFFFF;
    }
}

bool SpuThread::RdCh(u32 ch, u32* value, std::string* err) {
    switch (ch) {
        case 29: {  // SPU_RdInMbox (blocking)
            std::unique_lock<std::mutex> lk(ch_lock);
            ch_cv.wait(lk, [this] { return !in_mbox.empty(); });
            *value = in_mbox.front();
            in_mbox.pop_front();
            return true;
        }
        case 24: {  // MFC_RdTagStat
            std::unique_lock<std::mutex> lk(ch_lock);
            *value = 1u << (mfc.tag & 31);
            return true;
        }
        case 3: case 4: {  // signal notification
            u32 snr_idx = ch - 3;
            *value = static_cast<u32>(snr[snr_idx]);
            snr_valid[snr_idx] = false;
            return true;
        }
        default:
            *err = "rdch: unsupported channel " + std::to_string(ch);
            return false;
    }
}

bool SpuThread::WrCh(u32 ch, u32 value, std::string* err) {
    switch (ch) {
        case 28: {  // SPU_WrOutMbox (blocking, single slot)
            std::unique_lock<std::mutex> lk(ch_lock);
            ch_cv.wait(lk, [this] { return !out_mbox_valid; });
            out_mbox = value;
            out_mbox_valid = true;
            return true;
        }
        case 30: {  // SPU_WrOutIntrMbox (routes to PPU event queues in Phase 6)
            std::unique_lock<std::mutex> lk(ch_lock);
            intr_mbox.push_back(value);
            if (intr_mbox.size() > 64) intr_mbox.erase(intr_mbox.begin());
            return true;
        }
        case 16: mfc.lsa = value; return true;
        case 17: mfc.eah = value; return true;
        case 18: mfc.eal = value; return true;
        case 19: mfc.size = value; return true;
        case 20: mfc.tag = value; return true;
        case 21: {  // MFC_Cmd
            mfc.cmd = value;
            mfc.dma_count++;
            return DmaExecute(err);
        }
        case 22: {  // MFC_WrTagMask
            mfc.tag_mask = value;
            return true;
        }
        case 23:  // MFC_WrTagUpdate: any/1/2 — all complete instantly (sync DMA)
            return true;
        default:
            *err = "wrch: unsupported channel " + std::to_string(ch);
            return false;
    }
}

bool SpuThread::DmaExecute(std::string* err) {
    // MFC commands: 0x20 = put (LS -> EA), 0x40 = get (EA -> LS), 0x81 = barrier
    u32 cmd = mfc.cmd & ~(1u << 23);  // some tools set bit 23 (list)
    u64 ea = (static_cast<u64>(mfc.eah) << 32) | mfc.eal;
    u32 lsa = mfc.lsa & (kSpuLsSize - 1);
    u32 size = mfc.size;
    if (cmd != 0x20 && cmd != 0x40 && cmd != 0x81 && cmd != 0x80) {
        *err = "mfc: unsupported cmd " + std::to_string(mfc.cmd);
        return false;
    }
    if (cmd == 0x81 || cmd == 0x80) return true;  // barrier: no-op (synchronous DMA)
    if (size > 16 * 1024) size = 16 * 1024;
    if (lsa + size > kSpuLsSize) {
        *err = "mfc: LSA out of range";
        return false;
    }
    if (!Vm::IsValid(static_cast<u32>(ea), size)) {
        *err = "mfc: EA out of guest range";
        return false;
    }
    // NOTE: the PPU-side manager binds `guest_vm` before group start.
    if (!guest_vm) {
        *err = "mfc: no guest VM bound";
        return false;
    }
    u8* ea_ptr = guest_vm->Ptr(static_cast<u32>(ea), size);
    if (!ea_ptr) {
        *err = "mfc: guest page not allocated";
        return false;
    }
    if (cmd == 0x20) {  // put
        std::memcpy(ea_ptr, ls.data() + lsa, size);
    } else {  // get
        std::memcpy(ls.data() + lsa, ea_ptr, size);
    }
    return true;
}

bool SpuThread::Step() {
    const u32 pc4 = pc & (kSpuLsSize - 4);
    if (pc4 + 4 > ls.size()) {
        error = "pc out of local store";
        return false;
    }
    u32 word = ReadLE32(ls.data() + pc4);
    SpuInstr in;
    if (!SpuDecode(word, pc4, &in)) {
        char tmp[32];
        std::snprintf(tmp, sizeof(tmp), "%08x", word);
        error = std::string("unsupported SPU instruction 0x") + tmp;
        return false;
    }
    icount++;
    const u32 next_pc = (pc4 + 4) & 0x3FFFC;

    switch (in.mn) {
        case Smn::LNOP: case Smn::NOP: case Smn::SYNC: case Smn::DSYNC:
        case Smn::HBRA: case Smn::HBRR:
            break;

        case Smn::STOP: {
            exit_status = in.stop_code & 0x3FFF;
            state = SpuRunState::Stopped;
            return false;  // stop is not an error; ends execution
        }
        case Smn::STOPD: {
            exit_status = 0;
            state = SpuRunState::Stopped;
            return false;
        }

        // ---------------- immediates / integer ----------------
        case Smn::IL: {  // sign-extend the low 10 bits into all 4 lanes
            s32 v = static_cast<s32>(in.i16 & 0x3FF);
            if (v & 0x200) v -= 0x400;
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = static_cast<u32>(v);
            gpr[in.rt] = r;
            break;
        }
        case Smn::ILH: {  // replicate i16 into 4 halfword lanes
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = (in.i16 << 16) | in.i16;
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::ILHU: {  // i16 into the upper halves
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = in.i16 << 16;
            gpr[in.rt] = r;
            break;
        }
        case Smn::ILA: {  // 18-bit zero-extended into all 4 lanes
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = static_cast<u32>(in.i18);
            gpr[in.rt] = r;
            break;
        }
        case Smn::IOHL: {  // OR i16 into all 4 lanes
            for (int i = 0; i < 4; i++) gpr[in.rt].w[i] |= in.i16;
            break;
        }
        case Smn::AI: {  // per-lane add sign-extended i10
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = gpr[in.rt].w[i] + static_cast<u32>(in.si10);
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::SFI: {  // per-lane i10 - rt? ISA: SFI RT <- exts(I10) - RT
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = static_cast<u32>(in.si10) - gpr[in.rt].w[i];
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::ORI: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = gpr[in.rt].w[i] | static_cast<u32>(in.si10);
            gpr[in.rt] = r;
            break;
        }
        case Smn::ANDI: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = gpr[in.rt].w[i] & static_cast<u32>(in.si10);
            gpr[in.rt] = r;
            break;
        }
        case Smn::XORI: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = gpr[in.rt].w[i] ^ static_cast<u32>(in.si10);
            gpr[in.rt] = r;
            break;
        }
        case Smn::CEQI: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = gpr[in.rt].w[i] == static_cast<u32>(in.si10) ? 0xFFFFFFFF : 0;
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::CGTI: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = static_cast<s32>(gpr[in.rt].w[i]) > in.si10 ? 0xFFFFFFFF : 0;
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::CLGTI: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = gpr[in.rt].w[i] > static_cast<u32>(in.si10) ? 0xFFFFFFFF : 0;
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::FSMBI: {  // select-by-bit from i16 into 16 byte lanes
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                u32 lane = 0;
                for (int b = 0; b < 8; b++) {
                    if (in.i16 & (1u << (i * 8 + b))) lane |= 0xFFu << (b * 4);
                }
                r.w[i] = lane;
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::A: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = gpr[in.ra].w[i] + gpr[in.rb].w[i];
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::SF: {  // rt <- rb - ra
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = gpr[in.rb].w[i] - gpr[in.ra].w[i];
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::OR: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = gpr[in.ra].w[i] | gpr[in.rb].w[i];
            gpr[in.rt] = r;
            break;
        }
        case Smn::AND: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = gpr[in.ra].w[i] & gpr[in.rb].w[i];
            gpr[in.rt] = r;
            break;
        }
        case Smn::XOR: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = gpr[in.ra].w[i] ^ gpr[in.rb].w[i];
            gpr[in.rt] = r;
            break;
        }
        case Smn::NOR: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = ~(gpr[in.ra].w[i] | gpr[in.rb].w[i]);
            gpr[in.rt] = r;
            break;
        }
        case Smn::NAND: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = ~(gpr[in.ra].w[i] & gpr[in.rb].w[i]);
            gpr[in.rt] = r;
            break;
        }
        case Smn::ANDC: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = gpr[in.ra].w[i] & ~gpr[in.rb].w[i];
            gpr[in.rt] = r;
            break;
        }
        case Smn::ORC: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = gpr[in.ra].w[i] | ~gpr[in.rb].w[i];
            gpr[in.rt] = r;
            break;
        }
        case Smn::EQV: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = ~(gpr[in.ra].w[i] ^ gpr[in.rb].w[i]);
            gpr[in.rt] = r;
            break;
        }
        case Smn::CEQ: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = gpr[in.ra].w[i] == gpr[in.rb].w[i] ? 0xFFFFFFFF : 0;
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::CGT: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = static_cast<s32>(gpr[in.ra].w[i]) > static_cast<s32>(gpr[in.rb].w[i])
                             ? 0xFFFFFFFF : 0;
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::CLGT: {
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                r.w[i] = gpr[in.ra].w[i] > gpr[in.rb].w[i] ? 0xFFFFFFFF : 0;
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::SELB: {  // byte select: rt4 = mask, ra, rb -> rc
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                u32 m = gpr[in.rt4].w[i];
                r.w[i] = (gpr[in.ra].w[i] & m) | (gpr[in.rb].w[i] & ~m);
            }
            gpr[in.rc] = r;
            break;
        }
        case Smn::MPYA: {  // rt <- rt + ra * rb (16-bit products, per 16 lanes)
            const SpuVReg& a = gpr[in.rt4];
            const SpuVReg& b = gpr[in.ra];
            SpuVReg r = gpr[in.rt];
            for (int i = 0; i < 4; i++) {
                for (int h = 0; h < 2; h++) {
                    u16 sh = (i * 8) >> 1; (void)sh;
                    u16 av = static_cast<u16>(a.w[i] >> (h * 16));
                    u16 bv = static_cast<u16>(b.w[i] >> (h * 16));
                    s32 p = static_cast<s16>(av) * static_cast<s16>(bv);
                    u32 sum = r.w[i] + static_cast<u32>(p);
                    if (h == 0) r.w[i] = (r.w[i] & 0xFFFF0000) | (sum & 0xFFFF);
                    else r.w[i] = (r.w[i] & 0x0000FFFF) | (sum << 16);
                }
            }
            gpr[in.rt] = r;
            break;
        }
        case Smn::GB: {  // gather bits 0 of each 32-bit lane into rt lane 0
            SpuVReg r;
            u32 v = 0;
            for (int i = 0; i < 4; i++) {
                v |= ((gpr[in.rt].w[i] & 1) << i);
            }
            r.w[0] = v;
            gpr[in.rt] = r;
            break;
        }
        case Smn::FSMB: {  // select-by-bit from ra's byte lanes
            SpuVReg r;
            for (int i = 0; i < 4; i++) {
                u32 lane = 0;
                for (int b = 0; b < 8; b++) {
                    if (gpr[in.rt].w[i] & (1u << b)) lane |= 0xFFu << (b * 4);
                }
                r.w[i] = lane;
            }
            gpr[in.ra] = r;
            break;
        }

        // ---------------- quadword loads/stores ----------------
        case Smn::LQD: {  // rt <- LS16[ra + si10*16]
            u32 addr = (gpr[in.ra].w[0] + static_cast<u32>(in.si10) * 16) & (kSpuLsSize - 16);
            SpuVReg r;
            std::memcpy(r.w, ls.data() + addr, 16);
            gpr[in.rt] = r;
            break;
        }
        case Smn::STQD: {  // LS16[...] <- rt
            u32 addr = (gpr[in.ra].w[0] + static_cast<u32>(in.si10) * 16) & (kSpuLsSize - 16);
            std::memcpy(ls.data() + addr, gpr[in.rt].w, 16);
            break;
        }
        case Smn::LQR: {  // rt <- LS16[pc + si16*4] (relative, word units)
            s32 off = static_cast<s16>(in.i16);
            u32 addr = (pc4 + off * 4) & (kSpuLsSize - 16);
            SpuVReg r;
            std::memcpy(r.w, ls.data() + addr, 16);
            gpr[in.rt] = r;
            break;
        }
        case Smn::STQR: {
            s32 off = static_cast<s16>(in.i16);
            u32 addr = (pc4 + off * 4) & (kSpuLsSize - 16);
            std::memcpy(ls.data() + addr, gpr[in.rt].w, 16);
            break;
        }
        case Smn::LQA: {  // rt <- LS16[abs i16*4]
            u32 addr = (in.i16 * 4) & (kSpuLsSize - 16);
            SpuVReg r;
            std::memcpy(r.w, ls.data() + addr, 16);
            gpr[in.rt] = r;
            break;
        }
        case Smn::STQA: {
            u32 addr = (in.i16 * 4) & (kSpuLsSize - 16);
            std::memcpy(ls.data() + addr, gpr[in.rt].w, 16);
            break;
        }
        case Smn::LQX: {  // rt <- LS16[ra + rb]
            u32 addr = (gpr[in.ra].w[0] + gpr[in.rb].w[0]) & (kSpuLsSize - 16);
            SpuVReg r;
            std::memcpy(r.w, ls.data() + addr, 16);
            gpr[in.rt] = r;
            break;
        }
        case Smn::STQX: {
            u32 addr = (gpr[in.ra].w[0] + gpr[in.rb].w[0]) & (kSpuLsSize - 16);
            std::memcpy(ls.data() + addr, gpr[in.rt].w, 16);
            break;
        }

        // ---------------- channels (channel number = RA field, an immediate) ----------------
        case Smn::WRCH: {
            u32 ch = in.ra & 0x7F;
            u32 v = gpr[in.rt].w[0];
            if (!WrCh(ch, v, &error)) return false;
            break;
        }
        case Smn::RDCH: {
            u32 ch = in.ra & 0x7F;
            u32 v = 0;
            if (!RdCh(ch, &v, &error)) return false;
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = v;
            gpr[in.rt] = r;
            break;
        }
        case Smn::RCHCNT: {
            u32 ch = in.ra & 0x7F;
            u32 v = ChCount(ch, &error);
            if (v == 0xFFFFFFFF) return false;
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = v;
            gpr[in.rt] = r;
            break;
        }

        // ---------------- branches ----------------
        case Smn::BR: {  // relative, i16 words
            s32 off = static_cast<s16>(in.i16);
            pc = (pc4 + off * 4) & 0x3FFFC;
            return true;
        }
        case Smn::BRA: {  // absolute
            pc = (in.i16 * 4) & 0x3FFFC;
            return true;
        }
        case Smn::BRZ: {
            s32 off = static_cast<s16>(in.i16);
            if (gpr[in.rt].w[0] == 0) pc = (pc4 + off * 4) & 0x3FFFC;
            else pc = next_pc;
            return true;
        }
        case Smn::BRNZ: {
            s32 off = static_cast<s16>(in.i16);
            if (gpr[in.rt].w[0] != 0) pc = (pc4 + off * 4) & 0x3FFFC;
            else pc = next_pc;
            return true;
        }
        case Smn::BRHZ: {
            s32 off = static_cast<s16>(in.i16);
            if ((gpr[in.rt].w[0] & 0xFFFF) == 0) pc = (pc4 + off * 4) & 0x3FFFC;
            else pc = next_pc;
            return true;
        }
        case Smn::BRHNZ: {
            s32 off = static_cast<s16>(in.i16);
            if ((gpr[in.rt].w[0] & 0xFFFF) != 0) pc = (pc4 + off * 4) & 0x3FFFC;
            else pc = next_pc;
            return true;
        }
        case Smn::BI: {  // indirect via ra
            pc = gpr[in.ra].w[0] & 0x3FFFC;
            return true;
        }
        case Smn::BISL: {  // link in rt then branch
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = next_pc;
            gpr[in.rt] = r;
            pc = gpr[in.ra].w[0] & 0x3FFFC;
            return true;
        }
        case Smn::BIZ: {
            if (gpr[in.rt].w[0] == 0) pc = gpr[in.ra].w[0] & 0x3FFFC;
            else pc = next_pc;
            return true;
        }
        case Smn::BINZ: {
            if (gpr[in.rt].w[0] != 0) pc = gpr[in.ra].w[0] & 0x3FFFC;
            else pc = next_pc;
            return true;
        }
        case Smn::BRSL: {  // link + relative call
            SpuVReg r;
            for (int i = 0; i < 4; i++) r.w[i] = next_pc;
            gpr[in.rt] = r;
            s32 off = static_cast<s16>(in.i16);
            pc = (pc4 + off * 4) & 0x3FFFC;
            return true;
        }

        default:
            error = "SPU interpreter: mnemonic not implemented (mn=" +
                    std::to_string(static_cast<u32>(in.mn)) + ")";
            return false;
    }
    pc = next_pc;
    return true;
}

void SpuThread::Run() {
    state = SpuRunState::Running;
    u64 budget = 500000000ULL;
    while (budget-- > 0) {
        if (!Step()) break;  // stop, or error (error set)
    }
    if (budget == 0 && state == SpuRunState::Running) {
        error = "SPU instruction budget exceeded";
    }
    if (state != SpuRunState::Stopped) state = SpuRunState::Stopped;
}

// ---------------------------------------------------------------------------
// SPU ELF32 (little-endian, EM_SPU = 23) loader
// ---------------------------------------------------------------------------
bool LoadSpuElf32(const u8* data, size_t size, SpuThread* out, std::string* err) {
    if (size < 52) {
        *err = "too small for ELF32";
        return false;
    }
    if (std::memcmp(data, "\x7f""ELF", 4) != 0) {
        *err = "not an ELF";
        return false;
    }
    if (data[4] != 1 || data[5] != 1) {  // ELFCLASS32, ELFDATA2LSB
        *err = "SPU images are ELF32 little-endian";
        return false;
    }
    u16 machine = ReadLE16(data + 18);
    if (machine != 23) {
        *err = "machine is not EM_SPU (23)";
        return false;
    }
    u32 entry = ReadLE32(data + 24);
    u32 phoff = ReadLE32(data + 28);
    u16 phentsize = ReadLE16(data + 42);
    u16 phnum = ReadLE16(data + 44);
    if (phnum == 0 || phentsize < 32) {
        *err = "no program headers";
        return false;
    }
    for (u16 i = 0; i < phnum; i++) {
        const u8* ph = data + phoff + i * phentsize;
        u32 p_type = ReadLE32(ph);
        if (p_type != 1) continue;  // PT_LOAD
        u32 p_offset = ReadLE32(ph + 4);
        u32 p_vaddr = ReadLE32(ph + 8);
        u32 p_filesz = ReadLE32(ph + 16);
        if (p_vaddr + p_filesz > kSpuLsSize) {
            *err = "segment outside 256KiB local store";
            return false;
        }
        if (p_offset + p_filesz > size) {
            *err = "segment out of file bounds";
            return false;
        }
        std::memcpy(out->ls.data() + p_vaddr, data + p_offset, p_filesz);
    }
    out->entry = entry;
    out->pc = entry & 0x3FFFC;
    return true;
}

void SpuElfBuilder::AddSegment(u32 vaddr, std::vector<u8> bytes) {
    segs_.push_back({vaddr, std::move(bytes)});
}

std::vector<u8> SpuElfBuilder::Build() const {
    // ELF32 LE header (52 bytes) + phdrs (32 each) + segment data.
    std::vector<u8> phdr(static_cast<size_t>(segs_.size()) * 32, 0);
    u32 data_off = 52 + static_cast<u32>(phdr.size());
    std::vector<u8> data;
    for (size_t i = 0; i < segs_.size(); i++) {
        u8* ph = phdr.data() + i * 32;
        WriteLE32(ph + 0, 1);                                   // PT_LOAD
        WriteLE32(ph + 4, data_off + data.size());              // p_offset
        WriteLE32(ph + 8, segs_[i].vaddr);                      // p_vaddr
        WriteLE32(ph + 16, static_cast<u32>(segs_[i].bytes.size()));  // p_filesz
        WriteLE32(ph + 20, static_cast<u32>(segs_[i].bytes.size()));  // p_memsz
        WriteLE32(ph + 24, 7);                                  // RWX
        data.insert(data.end(), segs_[i].bytes.begin(), segs_[i].bytes.end());
    }
    std::vector<u8> out(52);
    u8* e = out.data();
    std::memcpy(e, "\x7f""ELF", 4);
    e[4] = 1;  // ELF32
    e[5] = 1;  // LSB
    e[6] = 1;  // EV_CURRENT
    WriteLE16(e + 16, 2);   // ET_EXEC
    WriteLE16(e + 18, 23);  // EM_SPU
    WriteLE32(e + 20, 1);   // version
    WriteLE32(e + 24, entry_);
    WriteLE32(e + 28, 52);  // phoff
    WriteLE16(e + 40, 52);  // ehsize
    WriteLE16(e + 42, 32);  // phentsize
    WriteLE16(e + 44, static_cast<u16>(segs_.size()));
    out.insert(out.end(), phdr.begin(), phdr.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

}  // namespace anyps3
