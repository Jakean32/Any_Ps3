#include "anyps3/hle/lv2.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include "anyps3/cpu/ppu/state.h"
#include "anyps3/hle/spu.h"
#include "anyps3/hle/modules.h"
#include "anyps3/hle/syscall_names.h"
#include "anyps3/log.h"
#include "anyps3/runtime/threads.h"

namespace anyps3 {

// Verified syscall numbers (research/syscall_table.txt):
//   1 getpid | 22 _sys_process_exit | 26 _sys_process_exit2 | 41 _sys_ppu_thread_exit
//   43 yield | 141 usleep | 142 sleep | 145 time | 147 timebase
//   348 sys_memory_allocate | 349 free | 352 get_user_memory_size
//   402 tty_read | 403 tty_write | 801-818 sys_fs | SPU: 165-190 (see spu.cpp)
Lv2::Lv2(Vm& vm) : vm_(vm), spu_(std::make_unique<SpuManager>(vm)) {
    modules_ = std::make_unique<ModuleHost>();
    modules_->Init(&vm_, this);
}

Lv2::~Lv2() {
    for (auto& [fd, file] : fd_table_) {
        if (file.handle) std::fclose(static_cast<FILE*>(file.handle));
    }
}

static s64 Arg(const PpuContext& ctx, int i) {
    return static_cast<s64>(ctx.gpr[3 + i]);
}

void Lv2::Syscall(PpuContext* ctx) {
    syscall_count++;
    const u32 nr = static_cast<u32>(ctx->gpr[11]);

    // HLE trampoline dispatch (module-import calls; see hle/modules.h).
    if (nr >= kHleDispatchBase && nr < kHleDispatchBase + 0x10000) {
        syscall_count--;
        modules_->Dispatch(nr - kHleDispatchBase, ctx);
        return;
    }

    switch (nr) {
        // SPU management goes to the dedicated manager (double counting avoided).
        case 165: case 169: case 170: case 171: case 172: case 173: case 178:
        case 181: case 182: case 184: case 190:
            syscall_count--;
            spu_->HandleSyscall(nr, ctx);
            return;
        case 1: HandleGetPid(ctx); return;
        case 22: case 26: HandleExit(ctx); return;
        case 41: {
            // _sys_ppu_thread_exit: ends the thread; for the primary thread
            // this ends the process (same as process exit).
            if (threads_) {
                threads_->SysExit(ctx, static_cast<s64>(ctx->gpr[3]));
            } else {
                HandleExit(ctx);
            }
            return;
        }
        case 43:  // sys_ppu_thread_yield
            if (threads_) {
                threads_->SysYield(ctx);
                ctx->gpr[3] = CELL_OK;
            } else {
                HandleYield(ctx);
            }
            return;
        case 44:  // sys_ppu_thread_join(tid r3, value* r4)
            if (threads_) {
                ctx->gpr[3] = static_cast<u64>(
                    threads_->SysJoin(ctx, static_cast<u64>(Arg(*ctx, 0)),
                                      static_cast<u32>(Arg(*ctx, 1))));
            } else {
                ctx->gpr[3] = CELL_ENOSYS;
            }
            return;
        case 45:  // sys_ppu_thread_detach(tid r3)
            if (threads_) {
                ctx->gpr[3] = static_cast<u64>(
                    threads_->SysDetach(ctx, static_cast<u64>(Arg(*ctx, 0))));
            } else {
                ctx->gpr[3] = CELL_ENOSYS;
            }
            return;
        case 52: {  // _sys_ppu_thread_create(id* r3, entry r4, arg r5, prio r6, ...)
            if (threads_) {
                ctx->gpr[3] = static_cast<u64>(threads_->SysCreate(
                    ctx, static_cast<u32>(Arg(*ctx, 0)), static_cast<u32>(Arg(*ctx, 1)),
                    static_cast<u64>(Arg(*ctx, 2)), static_cast<int>(Arg(*ctx, 3))));
                // Wake the scheduler: worker threads exist now.
                ctx->yield_req = 1;
            } else {
                ctx->gpr[3] = CELL_ENOSYS;
            }
            return;
        }
        case 53:  // sys_ppu_thread_start: threads start runnable on create
            ctx->gpr[3] = CELL_OK;
            return;
        case 141: HandleUsleep(ctx); return;
        case 142: HandleUsleep(ctx); return;  // sys_sleep: clamped like usleep
        case 145: HandleTime(ctx); return;
        case 147: HandleTimebase(ctx); return;
        case 348: HandleMemoryAllocate(ctx); return;
        case 349: HandleMemoryFree(ctx); return;
        case 352: HandleMemorySize(ctx); return;
        case 402:  // sys_tty_read: no input available
            ctx->gpr[3] = 0;
            return;
        case 403: HandleTtyWrite(ctx); return;
        case 801: HandleFsOpen(ctx); return;
        case 802: HandleFsRead(ctx); return;
        case 803: HandleFsWrite(ctx); return;
        case 804: HandleFsClose(ctx); return;
        case 805: HandleFsOpendir(ctx); return;
        case 806: HandleFsReaddir(ctx); return;
        case 807: HandleFsClosedir(ctx); return;
        case 808: HandleFsStat(ctx); return;
        case 818: HandleFsLseek(ctx); return;

        // ---- sys_prx (module manager; HLE registry in ModuleHost) ----------
        case 480: {  // _sys_prx_load_module(const char* path*, ...)
            std::string path = HleReadStr(vm_, static_cast<u32>(Arg(*ctx, 0)));
            size_t slash = path.find_last_of('/');
            std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
            size_t dot = base.find_last_of('.');
            std::string mod = (dot == std::string::npos) ? base : base.substr(0, dot);
            if (mod.empty() || !modules_->HasModule(mod)) {
                AP_INFO("LV2: prx load '", path, "' (module '", mod, "') — no HLE implementation; honest refusal");
                ctx->gpr[3] = CELL_ENOSYS;
            } else {
                s64 id = modules_->PrxLoadByName(mod, nullptr);
                AP_INFO("LV2: module '", mod, "' loaded via sys_prx (id ", static_cast<long long>(id), ")");
                ctx->gpr[3] = static_cast<u64>(id);
            }
            return;
        }
        case 481:  // _sys_prx_start_module(id, flags, *res)
            ctx->gpr[3] = static_cast<u64>(modules_->PrxStart(Arg(*ctx, 0), nullptr));
            return;
        case 482:  // _sys_prx_stop_module
            ctx->gpr[3] = static_cast<u64>(modules_->PrxStop(Arg(*ctx, 0), nullptr));
            return;
        case 483:  // _sys_prx_unload_module
            ctx->gpr[3] = static_cast<u64>(modules_->PrxUnload(Arg(*ctx, 0), nullptr));
            return;
        case 494:  // _sys_prx_get_module_list: HLE modules are not LV2 objects
            if (Arg(*ctx, 3)) vm_.Write32(static_cast<u32>(Arg(*ctx, 3)), 0);
            ctx->gpr[3] = CELL_OK;
            return;
        case 495:  // _sys_prx_get_module_info — not available for HLE modules
            ctx->gpr[3] = CELL_ENOSYS;
            return;
        case 496: {  // _sys_prx_get_module_id_by_name(const char* name*)
            std::string name = HleReadStr(vm_, static_cast<u32>(Arg(*ctx, 0)));
            s64 id = modules_->PrxIdByName(name);
            ctx->gpr[3] = id ? static_cast<u64>(id) : CELL_ENOENT;
            return;
        }

        default: {
            const char* name = Lv2SyscallName(nr);
            if (name) {
                AP_WARN("LV2: syscall ", static_cast<unsigned>(nr), " (", name,
                        ") not implemented — returning 0x80010002");
            } else {
                AP_WARN("LV2: syscall ", static_cast<unsigned>(nr),
                        " not implemented — returning 0x80010002");
            }
            ctx->gpr[3] = CELL_ENOSYS;
            return;
        }
    }
}

void Lv2::HandleGetPid(PpuContext* ctx) {
    ctx->gpr[3] = pid_;
}

void Lv2::HandleExit(PpuContext* ctx) {
    ctx->exit = true;
    ctx->exit_code = static_cast<s64>(ctx->gpr[3]) & 0xFFFFFFFF;
    AP_INFO("LV2: process exit with code ", static_cast<long>(ctx->exit_code));
}

void Lv2::HandleYield(PpuContext* ctx) {
    std::this_thread::yield();
    ctx->gpr[3] = CELL_OK;
}

void Lv2::HandleUsleep(PpuContext* ctx) {
    // sys_timer_usleep / sleep(142): clamp to avoid stalling tests
    u64 usec = static_cast<u64>(Arg(*ctx, 0));
    if (usec > 100000) usec = 100000;
    if (usec) std::this_thread::sleep_for(std::chrono::microseconds(usec));
    ctx->gpr[3] = CELL_OK;
}

void Lv2::HandleTime(PpuContext* ctx) {
    // sys_time_get_current_time(seconds*, nseconds*)
    u32 sec_ptr = static_cast<u32>(Arg(*ctx, 0));
    u32 nsec_ptr = static_cast<u32>(Arg(*ctx, 1));
    auto now = std::chrono::system_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count() % 1000000000;
    vm_.Write64(sec_ptr, static_cast<u64>(secs));
    vm_.Write64(nsec_ptr, static_cast<u64>(nsecs));
    ctx->gpr[3] = CELL_OK;
}

void Lv2::HandleTimebase(PpuContext* ctx) {
    ctx->gpr[3] = 79800000ULL;  // Cell timebase: 79.8 MHz
}

void Lv2::HandleTtyWrite(PpuContext* ctx) {
    // sys_tty_write(ch r3, buf r4, len r5, nwritten r6)
    u32 buf = static_cast<u32>(Arg(*ctx, 1));
    u32 len = static_cast<u32>(Arg(*ctx, 2));
    u32 out_ptr = static_cast<u32>(Arg(*ctx, 3));
    std::string line;
    if (len > 4096) len = 4096;
    for (u32 i = 0; i < len; i++) {
        u8 c = vm_.Read8(buf + i);
        line.push_back(static_cast<char>(c));
    }
    tty_.append(line);
    if (tty_.size() > 65536) tty_.erase(0, tty_.size() - 65536);
    AP_INFO("TTY: ", line.c_str());
    if (out_ptr) vm_.Write32(out_ptr, len);
    ctx->gpr[3] = CELL_OK;
}

void Lv2::HandleMemoryAllocate(PpuContext* ctx) {
    // sys_memory_allocate(size r3, flags r4, addr* r5)
    u64 size = static_cast<u64>(Arg(*ctx, 0));
    u32 flags = static_cast<u32>(Arg(*ctx, 1));
    u32 out_addr = static_cast<u32>(Arg(*ctx, 2));
    (void)flags;
    u64 page = kPageSize64K;
    u64 asize = (size + page - 1) & ~(page - 1);
    if (asize == 0 || mmap_top_ - asize < kMmapBase) {
        ctx->gpr[3] = CELL_ENOMEM;
        return;
    }
    u32 addr = static_cast<u32>(mmap_top_ - asize);
    if (!vm_.PageAlloc(addr, asize)) {
        ctx->gpr[3] = CELL_ENOMEM;
        return;
    }
    mmap_top_ = addr;
    mem_allocs_.push_back({addr, asize});
    vm_.Write32(out_addr, addr);
    ctx->gpr[3] = CELL_OK;
}

void Lv2::HandleMemoryFree(PpuContext* ctx) {
    u32 addr = static_cast<u32>(Arg(*ctx, 0));
    for (auto it = mem_allocs_.begin(); it != mem_allocs_.end(); ++it) {
        if (it->addr == addr) {
            u64 asize = it->size;
            for (u64 off = 0; off < asize; off += kPageSize64K) {
                vm_.PageFree(addr + static_cast<u32>(off));
            }
            mem_allocs_.erase(it);
            if (addr + asize >= mmap_top_) mmap_top_ = addr + static_cast<u32>(asize);
            ctx->gpr[3] = CELL_OK;
            return;
        }
    }
    ctx->gpr[3] = CELL_EINVAL;
}

void Lv2::HandleMemorySize(PpuContext* ctx) {
    // sys_memory_get_user_memory_size(total*, avail*)
    u32 tot_ptr = static_cast<u32>(Arg(*ctx, 0));
    u32 av_ptr = static_cast<u32>(Arg(*ctx, 1));
    u64 total = kGuestMemSize - kGuestMemBase;
    vm_.Write64(tot_ptr, total);
    vm_.Write64(av_ptr, total - vm_.used_bytes());
    ctx->gpr[3] = CELL_OK;
}

// ---------------- sys_fs (host-backed, /app_home only for now) ----------------

static bool FsAllowed(const std::string& guest) {
    return guest.rfind("/app_home/", 0) == 0 || guest.rfind("/dev_hdd0/", 0) == 0;
}

// Map guest paths to host paths (/app_home -> content root, /dev_hdd0 -> data dir).
std::string FsMapPath(const std::string& guest, const std::string& app_home,
                      const std::string& hdd_root) {
    auto strip = [&guest](const char* prefix) -> std::string {
        size_t n = std::strlen(prefix);
        if (guest.rfind(prefix, 0) != 0) return std::string();
        std::string rest = guest.substr(n);
        while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());
        return rest;
    };
    if (std::string rest = strip("/app_home/"); !rest.empty() || guest == "/app_home") {
        return rest.empty() ? app_home : app_home + "/" + rest;
    }
    if (std::string rest = strip("/dev_hdd0/"); !rest.empty()) {
        return hdd_root + "/" + rest;
    }
    return std::string();
}

void Lv2::HandleFsOpen(PpuContext* ctx) {
    // sys_fs_open(path r3, flags r4, fd* r5, mode r6, ...)
    u32 path_ptr = static_cast<u32>(Arg(*ctx, 0));
    s32 flags = static_cast<s32>(Arg(*ctx, 1));
    u32 fd_ptr = static_cast<u32>(Arg(*ctx, 2));
    (void)flags;
    std::string guest;
    if (!vm_.ReadString(path_ptr, 4096, &guest)) {
        ctx->gpr[3] = CELL_EFAULT;
        return;
    }
    if (!FsAllowed(guest)) {
        AP_WARN("sys_fs_open: path outside supported roots: ", guest.c_str());
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    std::string host = FsMapPath(guest, app_home_, config_dir_ + "/hdd0");
    const char* mode = "rb";
    if (flags & 0x200) mode = "r+b";  // CELL_FS_O_CREAT
    FILE* f = std::fopen(host.c_str(), mode);
    if (!f) {
        ctx->gpr[3] = CELL_EINVAL;  // (ENOENT-ish)
        return;
    }
    u32 fd = next_fd_++;
    FsFile file;
    file.handle = f;
    std::fseek(f, 0, SEEK_END);
    file.size = static_cast<u64>(std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    fd_table_[fd] = file;
    vm_.Write32(fd_ptr, fd);
    ctx->gpr[3] = CELL_OK;
}

void Lv2::HandleFsRead(PpuContext* ctx) {
    u32 fd = static_cast<u32>(Arg(*ctx, 0));
    u32 buf = static_cast<u32>(Arg(*ctx, 1));
    u64 size = static_cast<u64>(Arg(*ctx, 2));
    u32 nread_ptr = static_cast<u32>(Arg(*ctx, 3));
    auto it = fd_table_.find(fd);
    if (it == fd_table_.end()) {
        ctx->gpr[3] = CELL_EBADF;
        return;
    }
    FILE* f = static_cast<FILE*>(it->second.handle);
    if (size > 0x1000000) size = 0x1000000;
    std::vector<u8> tmp(static_cast<size_t>(size));
    size_t got = std::fread(tmp.data(), 1, tmp.size(), f);
    vm_.WriteBytes(buf, tmp.data(), static_cast<u32>(got));
    it->second.position += got;
    if (nread_ptr) vm_.Write64(nread_ptr, got);
    ctx->gpr[3] = CELL_OK;
}

void Lv2::HandleFsWrite(PpuContext* ctx) {
    u32 fd = static_cast<u32>(Arg(*ctx, 0));
    u32 buf = static_cast<u32>(Arg(*ctx, 1));
    u64 size = static_cast<u64>(Arg(*ctx, 2));
    u32 nwrite_ptr = static_cast<u32>(Arg(*ctx, 3));
    auto it = fd_table_.find(fd);
    if (it == fd_table_.end()) {
        ctx->gpr[3] = CELL_EBADF;
        return;
    }
    FILE* f = static_cast<FILE*>(it->second.handle);
    if (size > 0x1000000) size = 0x1000000;
    std::vector<u8> tmp(static_cast<size_t>(size));
    vm_.ReadBytes(buf, static_cast<u32>(size), tmp.data());
    size_t put = std::fwrite(tmp.data(), 1, tmp.size(), f);
    if (nwrite_ptr) vm_.Write64(nwrite_ptr, put);
    ctx->gpr[3] = CELL_OK;
}

void Lv2::HandleFsClose(PpuContext* ctx) {
    u32 fd = static_cast<u32>(Arg(*ctx, 0));
    auto it = fd_table_.find(fd);
    if (it == fd_table_.end()) {
        ctx->gpr[3] = CELL_EBADF;
        return;
    }
    if (it->second.handle) std::fclose(static_cast<FILE*>(it->second.handle));
    fd_table_.erase(it);
    ctx->gpr[3] = CELL_OK;
}

void Lv2::HandleFsLseek(PpuContext* ctx) {
    u32 fd = static_cast<u32>(Arg(*ctx, 0));
    s64 off = static_cast<s64>(Arg(*ctx, 1));
    u32 whence = static_cast<u32>(Arg(*ctx, 2));
    auto it = fd_table_.find(fd);
    if (it == fd_table_.end()) {
        ctx->gpr[3] = CELL_EBADF;
        return;
    }
    int w = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
    std::fseek(static_cast<FILE*>(it->second.handle), static_cast<long>(off), w);
    long pos = std::ftell(static_cast<FILE*>(it->second.handle));
    it->second.position = static_cast<u64>(pos);
    ctx->gpr[3] = static_cast<s64>(pos);
}

void Lv2::HandleFsStat(PpuContext* ctx) {
    // sys_fs_stat(path, CellFsStat*): minimal fields (size at offset 0x28)
    u32 path_ptr = static_cast<u32>(Arg(*ctx, 0));
    u32 st_ptr = static_cast<u32>(Arg(*ctx, 1));
    std::string guest;
    if (!vm_.ReadString(path_ptr, 4096, &guest)) {
        ctx->gpr[3] = CELL_EFAULT;
        return;
    }
    if (!FsAllowed(guest)) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    std::string host = FsMapPath(guest, app_home_, config_dir_ + "/hdd0");
    FILE* f = std::fopen(host.c_str(), "rb");
    if (!f) {
        ctx->gpr[3] = CELL_EINVAL;
        return;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fclose(f);
    vm_.Write64(st_ptr + 0x28, static_cast<u64>(sz));  // st_size
    vm_.Write64(st_ptr + 0x08, 0x7FFFFFFFULL);           // ctime-ish
    ctx->gpr[3] = CELL_OK;
}

void Lv2::HandleFsOpendir(PpuContext* ctx) {
    // Directory enumeration is not implemented yet (no game needs it in the
    // current test set); return the documented "not supported" error.
    ctx->gpr[3] = CELL_ENOSYS;
}

void Lv2::HandleFsReaddir(PpuContext* ctx) { ctx->gpr[3] = CELL_ENOSYS; }
void Lv2::HandleFsClosedir(PpuContext* ctx) { ctx->gpr[3] = CELL_ENOSYS; }

}  // namespace anyps3
