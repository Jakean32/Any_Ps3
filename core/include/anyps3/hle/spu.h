// Any_Ps3 — sys_spu HLE: thread groups, SPU threads, mailboxes, LS access.
//
// Syscall numbers are the verified LV2 ones (170 create, 172 initialize,
// 173 start, 178 join, 165 exit status, 181/182 LS r/w, 184 SNR,
// 190 inbound mailbox).
//
// ⚠ Simplification (documented): real SDK images are passed via a
// sys_spu_image_t describing the ELF and its segments. Any_Ps3 currently
// accepts a simplified image contract (type 1: raw SPU ELF at image->src;
// segment lists are future work) — enough for homebrew built against our
// documented headers. See PHASE_REPORTS.md.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "anyps3/cpu/spu/state.h"
#include "anyps3/memory/vm.h"

namespace anyps3 {

struct PpuContext;

class SpuManager {
public:
    explicit SpuManager(Vm& vm) : vm_(vm) {}

    void HandleSyscall(u32 nr, PpuContext* ctx);

    u64 syscall_count = 0;

private:
    void GroupCreate(PpuContext* ctx);
    void GroupDestroy(PpuContext* ctx);
    void GroupStart(PpuContext* ctx);
    void GroupJoin(PpuContext* ctx);
    void ThreadInitialize(PpuContext* ctx);
    void ThreadGetExitStatus(PpuContext* ctx);
    void ThreadWriteSpuMb(PpuContext* ctx);
    void ThreadWriteSnr(PpuContext* ctx);
    void ThreadWriteLs(PpuContext* ctx);
    void ThreadReadLs(PpuContext* ctx);
    void InitializeCount(PpuContext* ctx);  // sys_spu_initialize(169)

    Vm& vm_;
    std::map<u32, std::unique_ptr<SpuGroup>> groups_;
    std::map<u32, SpuThread*> thread_lookup_;   // thread id -> thread
    std::vector<std::unique_ptr<std::thread>> runners_;
    u32 next_group_id_ = 0x1001;
    u32 next_thread_id_ = 0x2001;
};

}  // namespace anyps3
