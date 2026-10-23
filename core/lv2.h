#pragma once
// LV2 syscall HLE framework (high-level emulation of the PS3 hypervisor
// syscall layer). Implemented: registration table + dispatch, a small
// honest set of syscalls (process, TTY, basic memory). Everything
// unregistered returns CELL_ENOSYS and is logged — never silently faked.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace anyps3 {

// LV2 error codes (public, well-documented values).
inline constexpr int64_t CELL_OK = 0;
inline constexpr int64_t CELL_ENOSYS = (int64_t)0x80010003;
inline constexpr int64_t CELL_EINVAL = (int64_t)0x80010002;
inline constexpr int64_t CELL_ENOMEM = (int64_t)0x80010004;

// Well-known LV2 syscall numbers (preliminary subset; documented as such).
inline constexpr uint32_t SYS_PROCESS_EXIT = 322;
inline constexpr uint32_t SYS_PROCESS_GETPID = 322;  // provisional
inline constexpr uint32_t SYS_TTY_WRITE = 403;
inline constexpr uint32_t SYS_MEMORY_ALLOCATE = 348;  // provisional
inline constexpr uint32_t SYS_MEMORY_FREE = 349;      // provisional

using SyscallFn = std::function<int64_t(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                        uint64_t a5, uint64_t a6)>;

struct SyscallEntry {
    uint32_t id;
    std::string name;
    SyscallFn fn;
};

class Lv2 {
public:
    Lv2();  // registers built-ins

    void register_syscall(uint32_t id, const std::string& name, SyscallFn fn);
    bool known(uint32_t id) const;
    const std::string& name(uint32_t id) const;

    // Dispatch. Unknown ids -> CELL_ENOSYS + log entry.
    int64_t call(uint32_t id, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0,
                 uint64_t a4 = 0, uint64_t a5 = 0, uint64_t a6 = 0);

    // HLE memory manager (simple bump allocator over one reservation).
    int64_t mem_allocate(uint64_t size, uint64_t* out_addr = nullptr);
    int64_t mem_free(uint64_t addr);

    // TTY sink for sys_tty_write (captured for the in-app log).
    std::string tty_log;
    uint64_t last_exit_code = 0;
    bool exited = false;
    std::vector<std::string> call_log;  // honest dispatch history

    std::vector<SyscallEntry> table;

private:
    uint64_t mem_base_ = 0x10000000;
    uint64_t mem_size_ = 64 * 1024 * 1024;
    uint64_t mem_cursor_ = 0;
    std::string unknown_name_ = "<unknown>";
    std::vector<std::pair<uint64_t, uint64_t>> mem_allocs_;  // addr -> size
};

}  // namespace anyps3
