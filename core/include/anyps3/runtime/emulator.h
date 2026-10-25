// Any_Ps3 — Emulator orchestration API (implemented in core/src/runtime/emulator.cpp).
// Hosts (Dear ImGui desktop app, Android app) drive the emulation through this header.
#pragma once

#include "anyps3/config.h"
#include "anyps3/loader/import.h"
#include "anyps3/types.h"

#include <atomic>
#include <functional>
#include <string>

namespace anyps3 {

enum class RunState { Stopped, Booting, Running, Exiting };

struct EmuStats {
    u64 frames = 0;
    u64 syscalls = 0;
    u64 blocks_executed = 0;
    u64 interpreter_fallbacks = 0;
    double run_seconds = 0.0;
};

struct EmulatorCallbacks {
    // UI-facing callbacks (must be thread-safe; called from the emu thread).
    std::function<void(const std::string&)> log;          // line of log output
    std::function<void(int percent, const std::string&)> progress;  // AOT compile progress
    std::function<void(RunState)> state_changed;
};

struct BootResult {
    bool ok = false;
    std::string error;
    // Diagnostics describing what happened during boot (e.g. "AOT cache: 128 blocks
    // loaded", "interpreter fallback enabled for region ...").
    std::vector<std::string> notes;
};

class Emulator {
public:
    explicit Emulator(AppConfig& app);
    ~Emulator();
    Emulator(const Emulator&) = delete;
    Emulator& operator=(const Emulator&) = delete;

    // Loads the game manifest from the library, mounts its PS3FS root, unwraps
    // EBOOT.BIN (SELF/ELF), maps segments into the guest VM, warms/fills the AOT
    // block cache (reporting progress), and prepares the main PPU thread.
    // Does not start execution.
    // source_fd: when >= 0 the game source (ISO) is read through this file
    // descriptor for the whole boot (Android SAF, no-copy). The caller keeps
    // ownership of the fd and must keep it open for the whole Boot() call.
    BootResult Boot(const std::string& title_id, const GameSettings& settings,
                    const EmulatorCallbacks& cb, int source_fd = -1);

    // Runs the main loop until the guest calls _sys_process_exit (or Stop()).
    // Call from a dedicated host thread. Returns the guest exit code.
    int Run();

    void Stop();

    RunState state() const { return state_.load(std::memory_order_relaxed); }
    const EmuStats& stats() const { return stats_; }
    const std::string& boot_log_tail() const { return boot_log_tail_; }

private:
    AppConfig& app_;
    std::atomic<RunState> state_{RunState::Stopped};
    EmuStats stats_;
    EmulatorCallbacks cb_;
    std::string boot_log_tail_;
    std::string tty_out_;
    struct Impl;
    Impl* impl_ = nullptr;  // internal runtime state (memory, HLE, cpu)

    friend struct Impl;
};

// AOT cache manager — per-game translated-code cache (manifest + native blobs).
// Kept as a separate class so the compile/import screen can inspect and manage it.
class AotCache {
public:
    explicit AotCache(std::string cache_root) : root_(std::move(cache_root)) {}

    struct Manifest {
        std::string game_id;
        u64 module_hash = 0;      // FNV-1a over loaded executable code segments
        u64 lifter_version = 0;   // bumped when IR semantics change
        u64 backend_id = 0;       // 1 = MiniAOT x64, 2 = LLVM
        u32 block_count = 0;
        u32 interpreter_only = 0; // blocks that permanently fell back
    };

    // Hash of current module; matches cache => reuse, else rebuild.
    static u64 HashModule(const struct ElfImage& img);

    bool Load(const std::string& game_id, Manifest* out) const;
    bool Store(const std::string& game_id, const Manifest& m,
               const std::vector<u8>& blob) const;
    bool Clear(const std::string& game_id) const;
    std::string BlobPath(const std::string& game_id) const;

private:
    std::string root_;
};

}  // namespace anyps3
