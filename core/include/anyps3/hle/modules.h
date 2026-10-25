// Any_Ps3 — cellOS module HLE: registry, import linking, guest trampolines.
//
// Real PS3 executables are "static" ELFs that import functions from system
// modules (sysPrxForUser, cellSysutil, cellGcmSys, libc, ...) through NID
// tables in their libstub sections. The loader writes the address of an
// 8-byte function descriptor {u32 addr, u32 rtoc} into each importer slot
// (verified against the PSL1GHT stub pattern and public format docs).
//
// Any_Ps3 implements the target functions in C++ (HLE) and points each
// descriptor at a tiny guest trampoline:
//      lis r11, 0x1000        ; r11 = 0x10000000 | index
//      ori r11, r11, index
//      sc                     ; -> LV2 dispatch -> ModuleHost::Dispatch
// so the caller's own PowerOpen stub does the rest (loads addr+rtoc, bctr).
//
// Honesty contract: every import RESOLVES (the game must not jump through a
// null slot), but anything not really implemented resolves to a handler that
// logs "MISSING module.nid (name)" once and returns CELL_ENOSYS. The boot log
// therefore shows exactly how far the game got — the bring-up report.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "anyps3/types.h"
#include "anyps3/hle/module_tables.h"
#include "anyps3/loader/elf.h"  // ModuleImport

namespace anyps3 {

class Vm;
struct PpuContext;
class Lv2;

// Guest-reserved area for trampolines + descriptors (bottom of the MMAP
// region; sys_memory_allocate allocates top-down, thread stacks live at
// 0x1F000000 — no practical conflict). 256 KiB ≈ 10k functions.
constexpr u32 kHleAreaBase = 0x10000000;
constexpr u32 kHleAreaSize = 0x40000;
// r11 values >= this are HLE dispatches (trampoline: lis r11, 0x1000).
constexpr u32 kHleDispatchBase = 0x10000000u;

// Guest exit-callback registration (sys_process_at_Exitspawn etc.).
struct GuestCallback {
    u32 addr = 0;   // guest code address
    u32 rtoc = 0;   // guest r2 for the call
    u64 arg = 0;    // argument passed in r3
};

class ModuleHost {
public:
    // Allocates the trampoline area and builds the (module,nid)->info index.
    void Init(Vm* vm, Lv2* lv2);

    // Resolve one import. Returns the guest address of the 8-byte function
    // descriptor to store into the importer's slot; 0 only if the trampoline
    // area is exhausted (honest hard error). Every resolved import is logged
    // (name known or raw nid) so the boot log doubles as a bring-up report.
    u32 Resolve(const std::string& module_name, u32 nid);

    // True if `module_name` is a known system module (sys_prx load support).
    bool HasModule(const std::string& module_name) const;

    // HLE trampoline dispatch (r11 = kHleDispatchBase + index).
    void Dispatch(u32 index, PpuContext* ctx);

    // ---- sys_prx-style module registry (real bookkeeping over HLE modules) --
    s64 PrxLoadByName(const std::string& module_name, std::string* err);
    s64 PrxStart(s64 id, std::string* err);
    s64 PrxStop(s64 id, std::string* err);
    s64 PrxUnload(s64 id, std::string* err);
    s64 PrxIdByName(const std::string& module_name) const;

    // ---- exit callbacks (sys_process_at_Exitspawn) ------------------------
    void RegisterExitCallback(u32 addr, u32 rtoc, u64 arg);
    const std::vector<GuestCallback>& exit_callbacks() const { return exit_callbacks_; }

    // ---- bring-up report ----------------------------------------------------
    struct ImportStat {
        std::string module_name;
        u32 nid = 0;
        std::string name;      // public name when known, else ""
        bool implemented = false;
        u32 resolve_count = 0; // times resolved (duplicated imports dedup by slot)
        u64 call_count = 0;    // MISSING handler invocations
    };
    const std::vector<ImportStat>& imports() const { return imports_; }
    // One-line-per-missing summary for the boot log (bring-up report).
    std::string MissingSummary() const;

    // True when at least one not-implemented import was actually CALLED.
    bool missing_called() const;

private:
    struct Func {
        const ModuleExportInfo* info = nullptr;  // null = not in public tables
        std::string module_name;                // as imported (may be unknown)
        std::string name;                        // resolved name or "nid_%08x"
        u32 nid = 0;
        bool implemented = false;
        u32 code_addr = 0;                      // trampoline address
        u32 desc_addr = 0;                       // descriptor address
        u64 call_count = 0;
        int stat_index = -1;
    };
    Func* FindByIndex(u32 index);

    Vm* vm_ = nullptr;
    Lv2* lv2_ = nullptr;
    u32 next_trampoline_ = kHleAreaBase;
    u32 next_index_ = 0;

    std::map<std::pair<std::string, u32>, u32> by_module_nid_;  // -> index
    std::vector<Func> funcs_;
    std::vector<ImportStat> imports_;

    // sys_prx registry
    struct LoadedModule {
        s64 id;
        std::string name;
        int start_count = 0;
    };
    std::vector<LoadedModule> loaded_;
    s64 next_prx_id_ = 0x100;

    std::vector<GuestCallback> exit_callbacks_;
};

// Helper for handlers: guest string at r-reg (BE bytes).
std::string HleReadStr(Vm& vm, u32 addr, size_t max = 4096);

// Patch every import slot in guest memory with its resolved HLE descriptor
// (LV2 module-manager behavior: addrs[i] = descriptor address). Returns stats.
struct LinkStats {
    u32 linked = 0;      // slots patched
    u32 failed = 0;      // resolve/write failures
    u32 use_refs = 0;    // secondary use-addresses patched (attribute 0x2000)
};
LinkStats LinkImportsToHle(Vm& vm, ModuleHost& host, const std::vector<ModuleImport>& imports);

// Synchronously run a guest function (qsort comparators, exit callbacks) in a
// temporary PPU context on the shared VM/LV2. Returns the guest r3 result.
s64 HleCallGuest(Vm& vm, Lv2* lv2, u32 addr, u32 rtoc, u64 r3, u64 r4, u64 r5);

}  // namespace anyps3
