#include "any_ps3/interp.h"

#include <cstring>

namespace any_ps3 {

// ---- LV2 table ----
const char* lv2_syscall_name(uint32_t num) {
    switch (num) {
        case 1:   return "sys_process_exit";   // public LV2 documentation
        case 988: return "sys_tty_write";
        default:  return nullptr;
    }
}

std::string lv2_syscall_display(uint32_t num) {
    const char* n = lv2_syscall_name(num);
    if (n) return n;
    char buf[48];
    std::snprintf(buf, sizeof buf, "unknown syscall 0x%x", num);
    return buf;
}

// ---- Lv2Hle ----
bool Lv2Hle::syscall(uint32_t num, uint64_t args[8], uint64_t& ret) {
    ret = 0;
    switch (num) {
        case 988: { // sys_tty_write(fd, buf, len, *written)
            uint64_t fd = args[0], buf = args[1], len = args[2], written_ptr = args[3];
            if (fd > 2 && fd != 1000001 && fd != 1000002) { ret = 0x80010006 /*EBADF*/; return true; }
            std::vector<char> tmp;
            size_t take = (size_t)(len > 65536 ? 65536 : len);
            tmp.resize(take);
            if (take && mem && !mem->read(buf, tmp.data(), take)) { ret = 0x80010002 /*EFAULT*/; return true; }
            captured_stdout.append(tmp.data(), take);
            // emulate *written = len
            uint64_t out_len = len;
            if (mem && written_ptr) mem->write(written_ptr, &out_len, 8);
            return true;
        }
        case 1: // sys_process_exit(status)
            return true; // caller checks name and stops
        default:
            return false; // honestly unhandled
    }
}

// ---- interpreter ----
void PpuInterpreter::reset() {
    std::memset(gpr_, 0, sizeof gpr_);
    lr_ = 0;
    ctr_ = 0;
    last_insn_ = {};
}

bool PpuInterpreter::fetch(uint32_t pc, uint32_t& word) {
    uint8_t b[4];
    if (!mem_.read(pc, b, 4)) return false;
    word = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    return true;
}

PpuInterpreter::Exit PpuInterpreter::run(uint64_t entry, uint64_t max_steps, RunStats* stats) {
    uint32_t pc = (uint32_t)entry;
    uint64_t steps = 0;
    while (steps < max_steps) {
        uint32_t word;
        if (!fetch(pc, word)) {
            if (stats) { stats->steps = steps; stats->last_pc = pc; stats->note = "fetch fault"; }
            return Exit::MemFault;
        }
        last_insn_ = ppu_decode(pc, word);

        // Dispatch on mnemonic (subset, everything else => IllegalInstruction).
        uint8_t rd = last_insn_.rd, ra = last_insn_.ra, rb = last_insn_.rb;
        switch (last_insn_.m) {
            case PpuMnemonic::Nop:
                pc += 4;
                break;
            case PpuMnemonic::Addi: // rD = (rA==0 ? 0 : rA) + simm
                gpr_[rd] = (uint64_t)((int64_t)(ra == 0 ? 0 : gpr_[ra]) + last_insn_.imm);
                pc += 4;
                break;
            case PpuMnemonic::Addis: // rD = (rA==0 ? 0 : rA) + (simm << 16)
                gpr_[rd] = (uint64_t)((int64_t)(ra == 0 ? 0 : gpr_[ra]) + ((int64_t)last_insn_.imm << 16));
                pc += 4;
                break;
            case PpuMnemonic::Ori:
                gpr_[rd] = (ra == 0 ? 0 : gpr_[ra]) | (uint32_t)last_insn_.imm;
                pc += 4;
                break;
            case PpuMnemonic::Oris:
                gpr_[rd] = (ra == 0 ? 0 : gpr_[ra]) | ((uint64_t)(uint32_t)last_insn_.imm << 16);
                pc += 4;
                break;
            case PpuMnemonic::Or:
                gpr_[rd] = (ra ? gpr_[ra] : 0) | (rb ? gpr_[rb] : 0);
                pc += 4;
                break;
            case PpuMnemonic::Mr: // or rD,rA,rA form (decoder marks mr when ra==rb)
                gpr_[rd] = gpr_[ra];
                pc += 4;
                break;
            case PpuMnemonic::Lwz: {
                uint64_t addr = (ra == 0 ? 0 : gpr_[ra]) + (uint64_t)(int64_t)last_insn_.imm;
                uint32_t v;
                if (!mem_.read(addr, &v, 4)) { if (stats) stats->note = "lwz fault"; return Exit::MemFault; }
                uint8_t b[4];
                std::memcpy(b, &v, 4);
                gpr_[rd] = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
                pc += 4;
                break;
            }
            case PpuMnemonic::Lbz: {
                uint64_t addr = (ra == 0 ? 0 : gpr_[ra]) + (uint64_t)(int64_t)last_insn_.imm;
                uint8_t v;
                if (!mem_.read(addr, &v, 1)) { if (stats) stats->note = "lbz fault"; return Exit::MemFault; }
                gpr_[rd] = v;
                pc += 4;
                break;
            }
            case PpuMnemonic::Stw: {
                uint64_t addr = (ra == 0 ? 0 : gpr_[ra]) + (uint64_t)(int64_t)last_insn_.imm;
                uint32_t v = (uint32_t)gpr_[rd];
                uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
                if (!mem_.write(addr, b, 4)) { if (stats) stats->note = "stw fault"; return Exit::MemFault; }
                pc += 4;
                break;
            }
            case PpuMnemonic::Ld: {
                uint64_t addr = (ra == 0 ? 0 : gpr_[ra]) + (uint64_t)(int64_t)last_insn_.imm;
                uint8_t b[8];
                if (!mem_.read(addr, b, 8)) { if (stats) stats->note = "ld fault"; return Exit::MemFault; }
                uint64_t v = 0;
                for (int k = 0; k < 8; k++) v = (v << 8) | b[k];
                gpr_[rd] = v;
                pc += 4;
                break;
            }
            case PpuMnemonic::Std: {
                uint64_t addr = (ra == 0 ? 0 : gpr_[ra]) + (uint64_t)(int64_t)last_insn_.imm;
                uint64_t v = gpr_[rd];
                uint8_t b[8];
                for (int k = 0; k < 8; k++) b[k] = (uint8_t)(v >> (56 - k * 8));
                if (!mem_.write(addr, b, 8)) { if (stats) stats->note = "std fault"; return Exit::MemFault; }
                pc += 4;
                break;
            }
            case PpuMnemonic::B:
            case PpuMnemonic::Ba:
                pc = (last_insn_.m == PpuMnemonic::Ba) ? (uint32_t)last_insn_.imm
                                                       : (uint32_t)(pc + (int64_t)last_insn_.imm);
                break;
            case PpuMnemonic::Bl:
                lr_ = pc + 4;
                pc = (uint32_t)(pc + (int64_t)last_insn_.imm);
                break;
            case PpuMnemonic::Bla:
                lr_ = pc + 4;
                pc = (uint32_t)last_insn_.imm;
                break;
            case PpuMnemonic::Bc: {
                // Simplified condition evaluation: only "branch always" patterns
                // (BO with the two known-bits clear) are taken, others fall
                // through. This is deliberate and documented.
                uint32_t bo = (last_insn_.word >> 21) & 0x1F; // BO = bits 6..10
                bool taken = ((bo & 0x14) == 0x14);
                bool aa = (last_insn_.word >> 1) & 1, lk = last_insn_.word & 1;
                if (lk) lr_ = pc + 4;
                if (taken) {
                    uint32_t bd14 = (last_insn_.word >> 2) & 0x3FFF;
                    int32_t off = ((int32_t)(bd14 << 18) >> 18) << 2;
                    pc = aa ? (uint32_t)off : (uint32_t)(pc + off);
                } else {
                    pc += 4;
                }
                break;
            }
            case PpuMnemonic::Bclr: { // blr / bclr (simplified: always return)
                pc = (uint32_t)lr_;
                break;
            }
            case PpuMnemonic::Mtlr:
                lr_ = gpr_[rd]; // mtlr rD => LR = rD (mtspr form: rS is field RD)
                pc += 4;
                break;
            case PpuMnemonic::Mflr:
                gpr_[rd] = lr_;
                pc += 4;
                break;
            case PpuMnemonic::Sc: {
                uint32_t num = (uint32_t)(gpr_[11] & 0xFFFFFFFF);
                uint64_t args[8];
                for (int k = 0; k < 8; k++) args[k] = gpr_[3 + k];
                if (num == 1) {
                    if (stats) { stats->steps = steps; stats->syscalls++; stats->last_pc = pc; }
                    return Exit::Exited;
                }
                uint64_t ret = 0;
                if (!hle_.syscall(num, args, ret)) {
                    if (stats) {
                        stats->steps = steps;
                        stats->syscalls++;
                        stats->last_pc = pc;
                        stats->note = lv2_syscall_display(num) + " not implemented in HLE";
                    }
                    return Exit::SyscallUnhandled;
                }
                gpr_[3] = ret;
                pc += 4;
                break;
            }
            default: {
                if (stats) {
                    stats->steps = steps;
                    stats->last_pc = pc;
                    stats->last_word = word;
                    char buf[96];
                    std::snprintf(buf, sizeof buf,
                                  "instruction not implemented in fallback interpreter: 0x%08x (%s)",
                                  word, last_insn_.name);
                    stats->note = buf;
                }
                return Exit::IllegalInstruction;
            }
        }
        steps++;
    }
    if (stats) { stats->steps = steps; stats->note = "step limit reached"; }
    return Exit::MaxSteps;
}

} // namespace any_ps3
