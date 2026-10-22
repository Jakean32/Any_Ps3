// Any_Ps3 — SPU (Cell SPE) thread state: 128 registers of 128 bits, 256 KiB
// little-endian local store, MFC DMA engine, mailboxes, signal registers.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "anyps3/memory/vm.h"

namespace anyps3 {

// 128-bit register viewed as 4 little-endian 32-bit lanes.
struct SpuVReg {
    u32 w[4] = {};
};

constexpr u32 kSpuLsSize = 256 * 1024;

enum class SpuRunState { Ready, Running, Stopped };

struct SpuThread {
    explicit SpuThread(u32 id, std::string name) : id(id), name(std::move(name)) {}
    ~SpuThread();

    u32 id = 0;
    std::string name;
    std::array<SpuVReg, 128> gpr{};
    std::vector<u8> ls = std::vector<u8>(kSpuLsSize, 0);
    u32 pc = 0;               // byte offset into LS
    u64 entry = 0;            // entry LS address
    u64 arg = 0;              // r3-equivalent (passed in regs by loader)
    std::atomic<SpuRunState> state{SpuRunState::Ready};
    u32 exit_status = 0;
    std::string error;

    // channels --------------------------------------------------------------
    // inbound mailbox (PPU -> SPU): 4-deep like hardware
    std::mutex ch_lock;
    std::condition_variable ch_cv;
    std::deque<u32> in_mbox;
    // outbound mailbox (SPU -> PPU): single-slot, blocking write
    u32 out_mbox = 0;
    bool out_mbox_valid = false;
    std::vector<u32> intr_mbox;    // SPU_WrOutIntrMbox values (event routing: Phase 6)
    // signal notification registers
    u64 snr[2] = {};
    bool snr_valid[2] = {false, false};

    // MFC DMA state -----------------------------------------------------------
    struct Mfc {
        u32 lsa = 0;
        u64 eah = 0, eal = 0;
        u32 size = 0, tag = 0, cmd = 0;
        u32 tag_mask = 0;
        u64 dma_count = 0;  // stats
    } mfc;

    Vm* guest_vm = nullptr;  // bound by the SPU manager before group start

    u64 icount = 0;

    // Host-side execution hook (set by the spu manager).
    void Run();   // defined in interp.cpp: runs until STOP/STOPD/error
    bool Step();  // one instruction; false on error/stop

    // channel helpers (thread-safe)
    bool WrCh(u32 ch, u32 value, std::string* err);
    bool RdCh(u32 ch, u32* value, std::string* err);
    u32 ChCount(u32 ch, std::string* err);

    void PushInMbox(u32 v) {
        std::unique_lock<std::mutex> lk(ch_lock);
        in_mbox.push_back(v);
        if (in_mbox.size() > 4) in_mbox.pop_front();
        ch_cv.notify_all();
    }
    void TakeOutMbox() {  // called by PPU side after reading out_mbox
        std::unique_lock<std::mutex> lk(ch_lock);
        out_mbox_valid = false;
        ch_cv.notify_all();
    }
    u32 ReadOutMbox() { return out_mbox; }

    // Performs one DMA (MFC put/get) between LS and guest memory.
    bool DmaExecute(std::string* err);
};

struct SpuGroup {
    u32 id = 0;
    std::string name;
    std::vector<std::unique_ptr<SpuThread>> threads;
    std::atomic<bool> started{false};
    std::atomic<int> running{0};
    u32 join_status = 0;
    bool joined = false;
};

// ---------------------------------------------------------------------------
// SPU ELF (ELF32 little-endian, EM_SPU = 23) loading into a local store.
// ---------------------------------------------------------------------------
bool LoadSpuElf32(const u8* data, size_t size, SpuThread* out, std::string* err);
inline bool LoadSpuElf32(const std::vector<u8>& data, SpuThread* out, std::string* err) {
    return LoadSpuElf32(data.data(), data.size(), out, err);
}

// Test/tool builder for tiny SPU ELFs.
class SpuElfBuilder {
public:
    void SetEntry(u32 entry) { entry_ = entry; }
    void AddSegment(u32 vaddr, std::vector<u8> bytes);
    std::vector<u8> Build() const;

private:
    u32 entry_ = 0;
    struct Seg {
        u32 vaddr;
        std::vector<u8> bytes;
    };
    std::vector<Seg> segs_;
};

}  // namespace anyps3
