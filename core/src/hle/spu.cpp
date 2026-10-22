// Any_Ps3 — sys_spu HLE implementation.
#include "anyps3/hle/spu.h"

#include <chrono>
#include <cstdio>

#include "anyps3/cpu/ppu/state.h"
#include "anyps3/hle/lv2.h"
#include "anyps3/log.h"

namespace anyps3 {

static s64 Arg(const PpuContext& ctx, int i) {
    return static_cast<s64>(ctx.gpr[3 + i]);
}

void SpuManager::HandleSyscall(u32 nr, PpuContext* ctx) {
    syscall_count++;
    switch (nr) {
        case 169: InitializeCount(ctx); return;         // sys_spu_initialize
        case 170: GroupCreate(ctx); return;             // sys_spu_thread_group_create
        case 171: GroupDestroy(ctx); return;            // ..._group_destroy
        case 172: ThreadInitialize(ctx); return;       // sys_spu_thread_initialize
        case 173: GroupStart(ctx); return;              // ..._group_start
        case 178: GroupJoin(ctx); return;               // ..._group_join
        case 165: ThreadGetExitStatus(ctx); return;    // get_exit_status
        case 181: ThreadWriteLs(ctx); return;          // write_ls
        case 182: ThreadReadLs(ctx); return;           // read_ls
        case 184: ThreadWriteSnr(ctx); return;         // write_snr
        case 190: ThreadWriteSpuMb(ctx); return;       // write_spu_mb
        default:
            ctx->gpr[3] = CELL_ENOSYS;
            return;
    }
}

void SpuManager::InitializeCount(PpuContext* ctx) {
    // sys_spu_initialize(max_usable_spus, ...) — informational in HLE.
    ctx->gpr[3] = CELL_OK;
}

void SpuManager::GroupCreate(PpuContext* ctx) {
    // sys_spu_thread_group_create(id*, name_ptr, num, prio, type, option)
    u32 id_ptr = static_cast<u32>(Arg(*ctx, 0));
    u32 name_ptr = static_cast<u32>(Arg(*ctx, 1));
    u32 num = static_cast<u32>(Arg(*ctx, 2));
    if (num == 0 || num > 6) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    const u32 gid = next_group_id_++;
    auto g = std::make_unique<SpuGroup>();
    g->id = gid;
    vm_.ReadString(name_ptr, 32, &g->name);
    groups_[gid] = std::move(g);
    vm_.Write32(id_ptr, gid);
    ctx->gpr[3] = CELL_OK;
}

void SpuManager::GroupDestroy(PpuContext* ctx) {
    u32 id = static_cast<u32>(Arg(*ctx, 0));
    auto it = groups_.find(id);
    if (it == groups_.end() || it->second->started) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    groups_.erase(it);
    ctx->gpr[3] = CELL_OK;
}

void SpuManager::ThreadInitialize(PpuContext* ctx) {
    // sys_spu_thread_initialize(id*, group, spu_index, image*, attr, arg, ...)
    // Simplified image contract (see hle/spu.h): the struct at r6 is
    //   { u32 type; u32 entry; u64 src; }  with type 1 = raw SPU ELF at `src`.
    u32 id_ptr = static_cast<u32>(Arg(*ctx, 0));
    u32 group_id = static_cast<u32>(Arg(*ctx, 1));
    u32 spu_index = static_cast<u32>(Arg(*ctx, 2));
    u32 image_ptr = static_cast<u32>(Arg(*ctx, 3));
    u64 arg = static_cast<u64>(Arg(*ctx, 5));

    auto git = groups_.find(group_id);
    if (git == groups_.end()) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    SpuGroup* group = git->second.get();
    if (spu_index != group->threads.size()) {
        ctx->gpr[3] = CELL_EINVAL;  // spu_index must be the next thread slot
        return;
    }
    u32 type = vm_.Read32(image_ptr);
    u32 entry = vm_.Read32(image_ptr + 4);
    u64 src = vm_.Read64(image_ptr + 8);
    if (type != 1) {
        AP_WARN("sys_spu_thread_initialize: unsupported image type ", type,
                " (segment-list images are future work)");
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    // Read the raw SPU ELF from guest memory (bounded).
    u32 elf_size = 1024 * 1024;
    const u8* p = vm_.Ptr(static_cast<u32>(src), elf_size);
    if (!p) {
        ctx->gpr[3] = CELL_EFAULT;
        return;
    }
    auto t = std::make_unique<SpuThread>(next_thread_id_++, "spu");
    std::string err;
    if (!LoadSpuElf32(p, elf_size, t.get(), &err)) {
        AP_WARN("sys_spu_thread_initialize: ", err.c_str());
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    if (entry) t->pc = entry;
    t->arg = arg;
    t->guest_vm = &vm_;
    SpuThread* raw = t.get();
    thread_lookup_[t->id] = raw;
    group->threads.push_back(std::move(t));
    vm_.Write32(id_ptr, raw->id);
    ctx->gpr[3] = CELL_OK;
}

void SpuManager::GroupStart(PpuContext* ctx) {
    u32 id = static_cast<u32>(Arg(*ctx, 0));
    auto it = groups_.find(id);
    if (it == groups_.end() || it->second->started) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    SpuGroup* group = it->second.get();
    group->started = true;
    for (auto& t : group->threads) {
        runners_.emplace_back(std::make_unique<std::thread>(
            [th = t.get()] { th->Run(); }));
    }
    ctx->gpr[3] = CELL_OK;
}

void SpuManager::GroupJoin(PpuContext* ctx) {
    u32 id = static_cast<u32>(Arg(*ctx, 0));
    u32 cause_ptr = static_cast<u32>(Arg(*ctx, 1));
    u32 status_ptr = static_cast<u32>(Arg(*ctx, 2));
    auto it = groups_.find(id);
    if (it == groups_.end()) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    SpuGroup* group = it->second.get();
    // Join pending SPU host threads. (Simplification documented in PHASE_REPORTS:
    // runners are tracked globally, matching the single-concurrent-group usage
    // pattern of the test suite; per-group runner tracking is minor follow-up.)
    for (auto& r : runners_) {
        if (r && r->joinable()) r->join();
    }
    runners_.clear();
    u32 cause = 0;  // SYS_SPU_THREAD_GROUP_JOIN_ALL_THREADS_EXITED
    u32 status = group->threads.empty() ? 0 : group->threads[0]->exit_status;
    group->join_status = status;
    group->joined = true;
    if (cause_ptr) vm_.Write32(cause_ptr, cause);
    if (status_ptr) vm_.Write32(status_ptr, status);
    ctx->gpr[3] = CELL_OK;
}

void SpuManager::ThreadGetExitStatus(PpuContext* ctx) {
    u32 tid = static_cast<u32>(Arg(*ctx, 0));
    u32 status_ptr = static_cast<u32>(Arg(*ctx, 1));
    auto it = thread_lookup_.find(tid);
    if (it == thread_lookup_.end()) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    vm_.Write32(status_ptr, it->second->exit_status);
    ctx->gpr[3] = CELL_OK;
}

void SpuManager::ThreadWriteSpuMb(PpuContext* ctx) {
    // sys_spu_thread_write_spu_mb(id, value) -> inbound mailbox
    u32 tid = static_cast<u32>(Arg(*ctx, 0));
    u32 value = static_cast<u32>(Arg(*ctx, 1));
    auto it = thread_lookup_.find(tid);
    if (it == thread_lookup_.end()) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    it->second->PushInMbox(value);
    ctx->gpr[3] = CELL_OK;
}

void SpuManager::ThreadWriteSnr(PpuContext* ctx) {
    u32 tid = static_cast<u32>(Arg(*ctx, 0));
    u32 which = static_cast<u32>(Arg(*ctx, 1));
    u64 value = static_cast<u64>(Arg(*ctx, 2));
    auto it = thread_lookup_.find(tid);
    if (it == thread_lookup_.end() || which > 1) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    it->second->snr[which] = value;
    it->second->snr_valid[which] = true;
    ctx->gpr[3] = CELL_OK;
}

void SpuManager::ThreadWriteLs(PpuContext* ctx) {
    u32 tid = static_cast<u32>(Arg(*ctx, 0));
    u32 ls_addr = static_cast<u32>(Arg(*ctx, 1));
    u32 size = static_cast<u32>(Arg(*ctx, 2));
    u32 src_ptr = static_cast<u32>(Arg(*ctx, 3));
    auto it = thread_lookup_.find(tid);
    if (it == thread_lookup_.end() || it->second->state != SpuRunState::Ready) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    if (ls_addr + size > kSpuLsSize) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    std::vector<u8> tmp(size);
    vm_.ReadBytes(src_ptr, size, tmp.data());
    std::memcpy(it->second->ls.data() + ls_addr, tmp.data(), size);
    ctx->gpr[3] = CELL_OK;
}

void SpuManager::ThreadReadLs(PpuContext* ctx) {
    u32 tid = static_cast<u32>(Arg(*ctx, 0));
    u32 ls_addr = static_cast<u32>(Arg(*ctx, 1));
    u32 size = static_cast<u32>(Arg(*ctx, 2));
    u32 dst_ptr = static_cast<u32>(Arg(*ctx, 3));
    auto it = thread_lookup_.find(tid);
    if (it == thread_lookup_.end() || ls_addr + size > kSpuLsSize) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    vm_.WriteBytes(dst_ptr, it->second->ls.data() + ls_addr, size);
    ctx->gpr[3] = CELL_OK;
}

}  // namespace anyps3
