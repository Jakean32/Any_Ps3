// Any_Ps3 — LV2 syscall HLE layer + PPU fallback interpreter.
//
// The AOT recompiler (not yet implemented — see pipeline.h) is the primary
// execution path. This interpreter is the honest fallback used for tiny
// dynamic-code stubs; it implements a deliberately small, tested subset of
// PPU instructions and reports everything else as illegal.
#pragma once

#include "types.h"
#include "elf.h"
#include "ppu.h"

namespace any_ps3 {

// ---- LV2 syscall table (intentionally minimal and correct) ----
// Only numbers that are publicly documented and verified are present.
const char* lv2_syscall_name(uint32_t num);      // nullptr when unknown
std::string lv2_syscall_display(uint32_t num);   // "sys_tty_write" or "unknown syscall 0x3dc"

// HLE backend interface implemented by host platforms.
class HleBackend {
public:
    virtual ~HleBackend() = default;
    // args[0..7] = r3..r10 (first 8 syscall args). ret in r3.
    // Return false to signal "unhandled" — the interpreter stops honestly.
    virtual bool syscall(uint32_t num, uint64_t args[8], uint64_t& ret) = 0;
};

// Default LV2 HLE: sys_tty_write (988) captures stdout; sys_process_exit (1)
// requests a clean stop. Everything else is honestly unhandled.
class Lv2Hle : public HleBackend {
public:
    PpuMemory* mem = nullptr;
    std::string captured_stdout;

    bool syscall(uint32_t num, uint64_t args[8], uint64_t& ret) override;
};

// ---- PPU fallback interpreter (subset) ----
class PpuInterpreter {
public:
    enum class Exit {
        Exited,            // sys_process_exit seen
        MaxSteps,
        IllegalInstruction, // includes unimplemented-in-fallback instructions
        MemFault,
        SyscallUnhandled,
    };

    struct RunStats {
        uint64_t steps = 0;
        uint64_t syscalls = 0;
        uint32_t last_pc = 0;
        uint32_t last_word = 0;
        std::string note;
    };

    PpuInterpreter(PpuMemory& mem, HleBackend& hle) : mem_(mem), hle_(hle) { reset(); }

    void reset();
    void set_register(uint8_t r, uint64_t v) { if (r < 32) gpr_[r] = v; }
    uint64_t register_value(uint8_t r) const { return r < 32 ? gpr_[r] : 0; }

    Exit run(uint64_t entry, uint64_t max_steps, RunStats* stats = nullptr);

    // Which instruction triggered an error (for honest reporting).
    PpuInsn last_insn() const { return last_insn_; }

private:
    bool fetch(uint32_t pc, uint32_t& word);
    Exit step(uint32_t& pc);

    PpuMemory& mem_;
    HleBackend& hle_;
    uint64_t gpr_[32] = {0};
    uint64_t lr_ = 0;
    uint64_t ctr_ = 0;
    PpuInsn last_insn_;
};

} // namespace any_ps3
