// Any_Ps3 — PPU thread manager implementation (see threads.h for scope).
#include "anyps3/runtime/threads.h"

#include "anyps3/log.h"

namespace anyps3 {

PpuThreadManager::Thread* PpuThreadManager::Find(u64 id) {
    for (auto& t : threads_)
        if (t.id == id) return &t;
    return nullptr;
}

PpuThreadManager::Thread* PpuThreadManager::FindByCtx(PpuContext* ctx) {
    if (ctx == main_) return &threads_[0];
    for (auto& t : threads_)
        if (t.ctx.get() == ctx) return &t;
    return nullptr;
}

u64 PpuThreadManager::CreateThread(const std::string& name, u32 entry, u64 arg, int prio) {
    if (threads_.size() >= 32) return 0;  // thread limit (honest, small)
    Thread t;
    t.id = next_id_++;
    t.name = name;
    t.prio = prio;
    t.ctx = std::make_unique<PpuContext>();
    t.ctx->vm = &vm_;
    t.ctx->pc = entry;

    // Thread stack in the MMAP region, top-down.
    u64 stack_top = kThreadStackTop - threads_.size() * kThreadStackSize;
    if (!vm_.PageAlloc(static_cast<u32>(stack_top - kThreadStackSize), kThreadStackSize)) {
        AP_ERROR("threads: no room for a thread stack");
        return 0;
    }
    t.ctx->gpr[1] = stack_top - 0x100;
    t.ctx->gpr[3] = arg;  // thread entry argument (PPC calling convention)
    threads_.push_back(std::move(t));
    AP_INFO("threads: created id ", (long long)threads_.back().id, " entry 0x",
            entry);
    return threads_.back().id;
}

s64 PpuThreadManager::SysCreate(PpuContext* caller, u32 id_ptr, u32 entry, u64 arg, int prio) {
    if (threads_.empty()) {
        Thread main_t;
        main_t.id = 0;
        main_t.name = "main";
        threads_.push_back(std::move(main_t));
        main_ = caller;
    }
    u64 id = CreateThread("ppu", entry, arg, prio);
    if (id == 0) return 0x8001000C;  // CELL_ENOMEM (approx.)
    vm_.Write64(id_ptr, id);         // BE u64 out-param
    return 0;                        // CELL_OK
}

s64 PpuThreadManager::SysJoin(PpuContext* caller, u64 tid, u32 value_ptr) {
    Thread* t = Find(tid);
    if (!t) return 0x8001000D;  // no such thread (CELL_EFAULT approx.)
    if (!t->joinable) return 0x80010009;
    if (!t->finished) {
        // Cooperative wait: replay this join sc until the target finishes.
        caller->replay_sc = true;
        caller->yield_req = 1;
        return 0;
    }
    if (value_ptr) vm_.Write64(value_ptr, static_cast<u64>(t->exit_code));
    AP_INFO("threads: join(tid ", (long long)tid, ") -> ", (long long)t->exit_code);
    return 0;
}

s64 PpuThreadManager::SysDetach(PpuContext* caller, u64 tid) {
    Thread* t = Find(tid);
    if (!t) return 0x8001000D;
    t->joinable = false;
    return 0;
}

void PpuThreadManager::SysExit(PpuContext* caller, s64 code) {
    Thread* t = FindByCtx(caller);
    if (!t || t->id == 0) {
        caller->exit = true;  // main thread exit ends the process
        caller->exit_code = code;
        return;
    }
    t->finished = true;
    t->exit_code = code;
    AP_INFO("threads: id ", (long long)t->id, " exited with ", (long long)code);
}

void PpuThreadManager::RunSlice(PpuContext* ctx, int budget,
                               std::function<void(PpuContext*)> syscall_fn) {
    for (int i = 0; i < budget && !ctx->exit && !ctx->error && !ctx->yield_req; i++) {
        if (!PpuStep(ctx)) return;
    }
}

int PpuThreadManager::RunAll(PpuContext* main, std::function<void(PpuContext*)> syscall_fn) {
    if (threads_.empty()) {
        Thread main_t;
        main_t.id = 0;
        threads_.push_back(std::move(main_t));
    }
    main_ = main;

    const u64 kMaxSlices = 10000000;  // safety valve (deadlock guard)
    u64 slices = 0;
    while (!main->exit && !main->error && slices++ < kMaxSlices) {
        main->yield_req = 0;
        RunSlice(main, 64, syscall_fn);

        bool any_running = false;
        for (auto& t : threads_) {
            if (t.id == 0 || t.finished || !t.ctx) continue;
            // Wire the shared runtime pointers into the worker context.
            t.ctx->syscall_fn = main->syscall_fn;
            t.ctx->lv2 = main->lv2;
            t.ctx->mem_base = main->mem_base;
            t.ctx->yield_req = 0;
            RunSlice(t.ctx.get(), 64, syscall_fn);
            if (t.ctx->error) {
                AP_ERROR("threads: id ", (long long)t.id,
                         " faulted: ", t.ctx->error_msg.c_str());
                t.finished = true;
            }
            if (t.ctx->exit) {
                t.finished = true;
                t.exit_code = t.ctx->exit_code;
            }
            any_running |= !t.finished;
        }
        (void)any_running;
    }
    if (slices >= kMaxSlices && !main->exit && !main->error) {
        AP_ERROR("threads: scheduler budget exceeded (guest deadlock?)");
    }
    return static_cast<int>(main->exit_code);
}

}  // namespace anyps3
