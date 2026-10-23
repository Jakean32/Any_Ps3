#include "lv2.h"

#include <cstring>

namespace anyps3 {

Lv2::Lv2() {
    register_syscall(SYS_TTY_WRITE, "sys_tty_write", [this](uint64_t fd, uint64_t buf, uint64_t len,
                                                            uint64_t, uint64_t, uint64_t) -> int64_t {
        (void)fd;
        // In a real HLE the buffer address points into guest memory; with no
        // guest memory mapped yet we accept only direct-pointer payloads.
        if (buf == 0 || len == 0 || len > (1 << 20)) return CELL_EINVAL;
        const char* p = (const char*)(uintptr_t)buf;
        tty_log.append(p, (size_t)len);
        tty_log.push_back('\n');
        return (int64_t)len;  // bytes written
    });
    register_syscall(SYS_PROCESS_EXIT, "sys_process_exit",
                     [this](uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
                         exited = true;
                         last_exit_code = code;
                         return CELL_OK;
                     });
    register_syscall(SYS_MEMORY_ALLOCATE, "sys_memory_allocate",
                     [this](uint64_t size, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
                         uint64_t addr = 0;
                         int64_t st = mem_allocate(size, &addr);
                         return st == CELL_OK ? (int64_t)addr : st;
                     });
    register_syscall(SYS_MEMORY_FREE, "sys_memory_free",
                     [this](uint64_t addr, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
                         return mem_free(addr);
                     });
}

void Lv2::register_syscall(uint32_t id, const std::string& name, SyscallFn fn) {
    for (auto& e : table) {
        if (e.id == id) {  // replace
            e.name = name;
            e.fn = std::move(fn);
            return;
        }
    }
    table.push_back(SyscallEntry{id, name, std::move(fn)});
}

bool Lv2::known(uint32_t id) const {
    for (const auto& e : table)
        if (e.id == id) return true;
    return false;
}

const std::string& Lv2::name(uint32_t id) const {
    for (const auto& e : table)
        if (e.id == id) return e.name;
    return unknown_name_;
}

int64_t Lv2::call(uint32_t id, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                  uint64_t a6) {
    char line[96];
    for (const auto& e : table) {
        if (e.id == id) {
            snprintf(line, sizeof line, "syscall %u %s -> dispatched", id, e.name.c_str());
            call_log.push_back(line);
            return e.fn(a1, a2, a3, a4, a5, a6);
        }
    }
    snprintf(line, sizeof line, "syscall %u -> CELL_ENOSYS (not implemented)", id);
    call_log.push_back(line);
    return CELL_ENOSYS;
}

int64_t Lv2::mem_allocate(uint64_t size, uint64_t* out_addr) {
    if (size == 0 || (size & 0xFFF) != 0) return CELL_EINVAL;  // page-aligned only
    if (mem_cursor_ + size > mem_size_) return CELL_ENOMEM;
    uint64_t addr = mem_base_ + mem_cursor_;
    mem_cursor_ += size;
    mem_allocs_.push_back({addr, size});
    if (out_addr) *out_addr = addr;
    return CELL_OK;
}

int64_t Lv2::mem_free(uint64_t addr) {
    for (size_t i = 0; i < mem_allocs_.size(); i++) {
        if (mem_allocs_[i].first == addr) {
            // simple bump allocator: freeing the last allocation reclaims
            if (i == mem_allocs_.size() - 1) {
                mem_cursor_ -= mem_allocs_[i].second;
                mem_allocs_.pop_back();
                return CELL_OK;
            }
            // interior free: mark as released but keep the hole (documented)
            mem_allocs_[i].second = 0;
            return CELL_OK;
        }
    }
    return CELL_EINVAL;
}

}  // namespace anyps3
