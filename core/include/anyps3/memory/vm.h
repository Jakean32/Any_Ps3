// Any_Ps3 — guest virtual memory (PS3 user space).
//
// Any_Ps3 logical memory model (documented in ARCHITECTURE.md):
//   USER_LOW  0x00010000 .. 0x00FFFFFF  ELF segments, static data
//   USER_HIGH 0x01000000 .. 0x0FFFFFFF  LV2 allocations, main stack (top 0x0FF00000)
//   MMAP      0x10000000 .. 0x1FFFFFFF  sys_memory_allocate (64KB/1MB), thread stacks
//
// Guest data in the backing store is BIG-endian (PPU). SPU local stores are
// little-endian and live in separate per-thread buffers (see cpu/spu).
#pragma once

#include "anyps3/types.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace anyps3 {
constexpr u32 kGuestMemBase = 0x00010000;   // lowest mappable address
constexpr u64 kGuestMemSize = 0x20000000ULL;  // 512 MiB virtual window
constexpr u32 kGuestMemEnd = static_cast<u32>(kGuestMemBase + kGuestMemSize);
constexpr u32 kMainStackTop = 0x0FF00000;   // main PPU thread stack top
constexpr u32 kMainStackSize = 0x00100000;  // 1 MiB
constexpr u32 kMmapBase = 0x10000000;        // sys_memory_allocate region
constexpr u32 kPageSize64K = 0x10000;
constexpr u32 kPageSize1M = 0x100000;

class Vm {
public:
    Vm() { Init(); }
    ~Vm() = default;

    void Init() {
        if (backing_.empty()) backing_.resize(static_cast<size_t>(kGuestMemSize), 0);
        std::fill(page_used_.begin(), page_used_.end(), 0);
        std::fill(page_exec_.begin(), page_exec_.end(), 0);
    }

    static bool IsValid(u32 addr, u64 size = 1) {
        return addr >= kGuestMemBase && static_cast<u64>(addr) + size <= kGuestMemEnd &&
               static_cast<u64>(addr) + size >= addr;
    }

    u8* Ptr(u32 addr, u64 size = 1) {
        if (!IsValid(addr, size)) return nullptr;
        return backing_.data() + static_cast<size_t>(addr);
    }
    const u8* Ptr(u32 addr, u64 size = 1) const {
        if (!IsValid(addr, size)) return nullptr;
        return backing_.data() + static_cast<size_t>(addr);
    }

    // 64KB-page helpers -------------------------------------------------------
    bool PageAlloc(u32 addr, u64 size) {
        if (!IsValid(addr, size)) return false;
        u32 first = (addr - kGuestMemBase) / kPageSize64K;
        u32 count = static_cast<u32>((size + kPageSize64K - 1) / kPageSize64K);
        for (u32 i = 0; i < count; i++) {
            if (page_used_[first + i]) return false;
        }
        for (u32 i = 0; i < count; i++) page_used_[first + i] = 1;
        u8* p = backing_.data() + static_cast<size_t>(first) * kPageSize64K;
        std::memset(p, 0, static_cast<size_t>(count) * kPageSize64K);
        return true;
    }
    bool PageFree(u32 addr) {
        if (!IsValid(addr)) return false;
        u32 first = (addr - kGuestMemBase) / kPageSize64K;
        if (addr != kGuestMemBase + first * kPageSize64K) return false;  // not page-aligned
        if (!page_used_[first]) return false;
        page_used_[first] = 0;
        return true;
    }
    bool PageIsAllocated(u32 addr) const {
        if (!IsValid(addr)) return false;
        return page_used_[(addr - kGuestMemBase) / kPageSize64K] != 0;
    }

    // Executable-page tracking: the AOT layer invalidates cached blocks on writes.
    bool PageIsExec(u32 addr) const {
        if (!IsValid(addr)) return false;
        return page_exec_[(addr - kGuestMemBase) / kPageSize64K] != 0;
    }
    void PageSetExec(u32 addr, bool on) {
        if (!IsValid(addr)) return;
        page_exec_[(addr - kGuestMemBase) / kPageSize64K] = on ? 1 : 0;
    }

    // Guest (big-endian) typed access -----------------------------------------
    u8 Read8(u32 a) const { const u8* p = Ptr(a); return p ? *p : 0; }
    u16 Read16(u32 a) const { const u8* p = Ptr(a); return p ? ReadBE16(p) : 0; }
    u32 Read32(u32 a) const { const u8* p = Ptr(a); return p ? ReadBE32(p) : 0; }
    u64 Read64(u32 a) const { const u8* p = Ptr(a); return p ? ReadBE64(p) : 0; }
    void Write8(u32 a, u8 v) { u8* p = Ptr(a); if (p) *p = v; }
    void Write16(u32 a, u16 v) { u8* p = Ptr(a); if (p) WriteBE16(p, v); }
    void Write32(u32 a, u32 v) { u8* p = Ptr(a); if (p) WriteBE32(p, v); }
    void Write64(u32 a, u64 v) { u8* p = Ptr(a); if (p) WriteBE64(p, v); }

    bool ReadBytes(u32 addr, u32 size, void* out) const {
        const u8* p = Ptr(addr, size);
        if (!p) return false;
        std::memcpy(out, p, size);
        return true;
    }
    bool WriteBytes(u32 addr, const void* src, u32 size) {
        u8* p = Ptr(addr, size);
        if (!p) return false;
        std::memcpy(p, src, size);
        return true;
    }

    // Read a NUL-terminated guest string.
    bool ReadString(u32 addr, u32 max, std::string* out) const {
        out->clear();
        for (u32 i = 0; i < max; i++) {
            u8 c = Read8(addr + i);
            if (c == 0) return true;
            out->push_back(static_cast<char>(c));
        }
        return true;
    }

    size_t used_bytes() const {
        size_t n = 0;
        for (char b : page_used_) {
            if (b) n += kPageSize64K;
        }
        return n;
    }
    const std::vector<u8>& backing() const { return backing_; }
    u8* mutable_data() { return backing_.data(); }  // AOT fast-path base

private:
    std::vector<u8> backing_;
    std::vector<char> page_used_ = std::vector<char>(static_cast<size_t>(kGuestMemSize / kPageSize64K), 0);
    std::vector<char> page_exec_ = std::vector<char>(static_cast<size_t>(kGuestMemSize / kPageSize64K), 0);
};

}  // namespace anyps3
