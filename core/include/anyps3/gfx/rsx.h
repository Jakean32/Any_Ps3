// Any_Ps3 — RSX abstraction: GCM command consumption, backends, flip tracking.
//
// ⚠ Honest scope (see PHASE_REPORTS.md): the real GCM command format parsing
// and shader decompilation are future work. What IS implemented and tested:
//   * the backend interface (null + guarded Vulkan skeleton)
//   * a documented *test command stream* format consumed by the null backend
//   * present/flip bookkeeping
//   * a content-addressed shader cache with store/load/invalidate and a
//     manifest (the layer a future GCM shader translator will plug into).
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "anyps3/types.h"

namespace anyps3 {

// Backend interface. The null backend validates and records; the Vulkan
// backend (guarded by ANYPS3_WITH_VULKAN) is a device/swapchain skeleton.
class RsxBackend {
public:
    virtual ~RsxBackend() = default;
    virtual std::string name() const = 0;
    virtual bool Initialize(void* host_window) = 0;
    virtual void Present() = 0;   // flip
    virtual void Shutdown() = 0;
};

RsxBackend* CreateNullBackend();
#if defined(ANYPS3_WITH_VULKAN)
RsxBackend* CreateVulkanBackend();
#endif

// Flip/stat bookkeeping shared by backends and the runtime.
struct RsxStats {
    u64 flips = 0;
    u64 draws = 0;      // reported by backends when they see draw commands
    u64 unknown_cmds = 0;
};

// ---------------------------------------------------------------------------
// Test command stream (Any_Ps3 test format — NOT the real GCM format):
//   u32 cmd | 0xFF000000 : PRESENT (flip)
//   u32 cmd | 0x01000000 : DRAW (count = low 24 bits)
//   u32 cmd | 0xF0000000 : LABEL (value = low 24 bits; recorded)
// Everything else counts toward unknown_cmds. This exists so the consuming
// pipeline (parser -> backend dispatch -> stats) is exercised end-to-end while
// the real GCM table is being built; the format is deliberately synthetic and
// documented as such.
// ---------------------------------------------------------------------------
class RsxCommandProcessor {
public:
    explicit RsxCommandProcessor(RsxBackend* backend) : backend_(backend) {}

    // Consume a command buffer (big-endian u32s from guest memory).
    void Execute(const u32* cmds, size_t count);

    const RsxStats& stats() const { return stats_; }
    u64 last_label() const { return last_label_; }

private:
    RsxBackend* backend_;
    RsxStats stats_;
    u64 last_label_ = 0;
};

// ---------------------------------------------------------------------------
// Shader cache: content-addressed blob store. Key = FNV-1a of the GCM program
// data; value = decompiled/recompiled shader (whatever the future translator
// produces). On-disk layout: <root>/shaders/<key-hi>/<key>.blob + index file.
// ---------------------------------------------------------------------------
class ShaderCache {
public:
    explicit ShaderCache(std::string root) : root_(std::move(root)) {}

    static u64 Key(const void* data, size_t len) { return Fnv1a64(data, len); }

    // Returns true on cache hit (blob filled). No hit leaves `blob` empty.
    bool Lookup(u64 key, std::vector<u8>* blob) const;
    // Store a blob (created or recompiled); overwrites silently.
    void Store(u64 key, const void* data, size_t len);
    bool Invalidate(u64 key);
    size_t EntryCount() const;

private:
    std::string PathFor(u64 key) const;
    std::string root_;
    mutable std::map<u64, bool> index_;
};

}  // namespace anyps3
