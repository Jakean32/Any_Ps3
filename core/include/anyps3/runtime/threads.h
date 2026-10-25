// Any_Ps3 — PPU thread manager: cooperative round-robin scheduling over
// per-thread PpuContexts, plus the LV2 thread syscalls.
//
// Honest scope (TASK-2 milestone 1): threads run through the INTERPRETER in
// time slices (the AOT block path stays the single-thread fast path); there is
// no true parallelism — slices interleave deterministically, which is enough
// to make lwarx/stwcx synchronization observable and correct.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "anyps3/cpu/ppu/state.h"
#include "anyps3/memory/vm.h"

namespace anyps3 {

constexpr u64 kThreadStackTop = 0x1F000000;   // thread stacks grow down from here
constexpr u64 kThreadStackSize = 0x00100000;  // 1 MiB each

class PpuThreadManager {
public:
    explicit PpuThreadManager(Vm& vm) : vm_(vm) {}

    // Creates a thread (entry, arg, priority). Returns the new thread id.
    u64 CreateThread(const std::string& name, u32 entry, u64 arg, int prio);

    // Runs a cooperative round-robin slice over all threads until the main
    // thread (id 0) exits (or errors). Returns the main thread exit code.
    int RunAll(PpuContext* main, std::function<void(PpuContext*)> syscall_fn);

    // ---- per-thread syscall actions (called from Lv2) ----
    // _sys_ppu_thread_create(52): id_ptr(r3) entry(r4) arg(r5) prio(r6) ...
    // Returns CELL-style error code; stores the id as BE64 at id_ptr.
    s64 SysCreate(PpuContext* caller, u32 id_ptr, u32 entry, u64 arg, int prio);
    // sys_ppu_thread_join(44): tid(r3) value_ptr(r4, may be 0)
    s64 SysJoin(PpuContext* caller, u64 tid, u32 value_ptr);
    // sys_ppu_thread_detach(45)
    s64 SysDetach(PpuContext* caller, u64 tid);
    // _sys_ppu_thread_exit(41): the CURRENT thread exits with code r3.
    void SysExit(PpuContext* caller, s64 code);
    // sys_ppu_thread_yield(43): request a slice switch.
    void SysYield(PpuContext* caller) { caller->yield_req = 1; }

    size_t thread_count() const { return threads_.size(); }

private:
    struct Thread {
        u64 id = 0;
        std::string name;
        int prio = 0;
        bool finished = false;
        bool joinable = true;
        s64 exit_code = 0;
        std::unique_ptr<PpuContext> ctx;  // nullptr for the main thread
    };

    Thread* Find(u64 id);
    Thread* FindByCtx(PpuContext* ctx);

    Vm& vm_;
    std::vector<Thread> threads_;
    u64 next_id_ = 1;
    PpuContext* main_ = nullptr;
    // Runs up to `budget` instructions of one thread; stops early on
    // exit/error/yield request.
    void RunSlice(PpuContext* ctx, int budget, std::function<void(PpuContext*)> syscall_fn);

};

}  // namespace anyps3
