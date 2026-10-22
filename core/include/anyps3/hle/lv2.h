// Any_Ps3 — LV2 (PS3 kernel) HLE: syscall dispatch + the implemented subset.
//
// Convention (verified): r11 = syscall number, r3..r10 = args, return in r3.
// Numbers verified against the LV2 syscall table (see research/syscall_table.txt).
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "anyps3/memory/vm.h"

namespace anyps3 {

struct PpuContext;
class SpuManager;

// LV2 error-style codes returned to the guest. Numeric values are approximations
// pending verification against the documented LV2 errno table (functional code
// paths only depend on zero/nonzero today). See PHASE_REPORTS.md.
constexpr s32 CELL_OK = 0;
constexpr u32 CELL_EPERM = 0x80010001;
constexpr u32 CELL_ENOSYS = 0x80010002;  // not supported (approx. value)
constexpr u32 CELL_EBADF = 0x80010009;   // bad fd (approx. value)
constexpr u32 CELL_EINVAL = 0x8001000B;  // invalid argument (approx. value)
constexpr u32 CELL_ENOMEM = 0x8001000C;   // out of memory (approx. value)
constexpr u32 CELL_EFAULT = 0x8001000D;  // bad memory access (approx. value)

class Lv2 {
public:
    explicit Lv2(Vm& vm);
    ~Lv2();

    // Dispatch one syscall. Reads r11, writes r3, may set ctx->exit.
    void Syscall(PpuContext* ctx);

    // Debug console output (sys_tty_write target)
    const std::string& tty() const { return tty_; }

    void SetContentRoot(const std::string& dir) { app_home_ = dir; }
    void SetConfigDir(const std::string& dir) { config_dir_ = dir; }

    // Syscall counters for the UI/stats.
    u64 syscall_count = 0;

    SpuManager& spu() { return *spu_; }

private:
    void HandleTtyWrite(PpuContext* ctx);
    void HandleExit(PpuContext* ctx);
    void HandleMemoryAllocate(PpuContext* ctx);
    void HandleMemoryFree(PpuContext* ctx);
    void HandleMemorySize(PpuContext* ctx);
    void HandleFsOpen(PpuContext* ctx);
    void HandleFsRead(PpuContext* ctx);
    void HandleFsWrite(PpuContext* ctx);
    void HandleFsClose(PpuContext* ctx);
    void HandleFsLseek(PpuContext* ctx);
    void HandleFsStat(PpuContext* ctx);
    void HandleFsOpendir(PpuContext* ctx);
    void HandleFsReaddir(PpuContext* ctx);
    void HandleFsClosedir(PpuContext* ctx);
    void HandleTime(PpuContext* ctx);
    void HandleTimebase(PpuContext* ctx);
    void HandleUsleep(PpuContext* ctx);
    void HandleGetPid(PpuContext* ctx);
    void HandleYield(PpuContext* ctx);

    Vm& vm_;
    std::string tty_;
    std::string app_home_;     // host directory backing /app_home
    std::string config_dir_;   // host directory for hdd0 emulation
    u32 pid_ = 1;

    struct FsFile {
        void* handle = nullptr;  // FILE*
        u64 position = 0;
        u64 size = 0;
        bool is_dir = false;
    };
    std::map<u32, FsFile> fd_table_;
    u32 next_fd_ = 0x10;

    std::unique_ptr<SpuManager> spu_;  // sys_spu HLE

    u32 mmap_top_ = 0x1FFFF000;  // sys_memory_allocate goes top-down in MMAP region
    struct MemAlloc {
        u32 addr;
        u64 size;
    };
    std::vector<MemAlloc> mem_allocs_;
};

}  // namespace anyps3
