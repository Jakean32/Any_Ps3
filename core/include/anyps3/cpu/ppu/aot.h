// Any_Ps3 — AOT backends.
//
// MiniAotX64: a self-contained x86-64 machine-code emitter that consumes the
// AOT-IR. It is the development/validation backend proving the full AOT path
// (PPC64 -> IR -> native code -> execute) with zero external dependencies.
//
// The LLVM backend (aot_llvm.cpp, compile-guarded by ANYPS3_WITH_LLVM) targets
// arm64/x64 via the LLVM-C API and is the production backend on Android.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "anyps3/cpu/ppu/ir.h"
#include "anyps3/cpu/ppu/state.h"

namespace anyps3 {

namespace mini_aot {

// Compiled block entry: `fn(ctx)` executes one guest basic block and returns
// with ctx->pc set to the next block entry (or ctx->exit/ctx->error set).
using BlockFn = void (*)(PpuContext*);

struct Code {
    BlockFn fn = nullptr;
    void* mem = nullptr;
    size_t size = 0;
    size_t emitted = 0;
};

// Compile `ir` into executable memory. Returns fn == nullptr on unsupported IR.
// (The result holds its memory; free via Release()/destructor.)
Code Compile(const std::vector<AIROp>& ir);
void Release(Code* c);

class CodeHolder {
public:
    CodeHolder() = default;
    explicit CodeHolder(Code c) : code_(c) {}
    ~CodeHolder() { Release(&code_); }
    CodeHolder(CodeHolder&& o) noexcept : code_(o.code_) { o.code_ = {}; }
    CodeHolder& operator=(CodeHolder&& o) noexcept {
        Release(&code_);
        code_ = o.code_;
        o.code_ = {};
        return *this;
    }
    BlockFn fn() const { return code_.fn; }
    size_t emitted() const { return code_.emitted; }
    bool ok() const { return code_.fn != nullptr; }

private:
    Code code_{};
};

// Per-VM block cache: guest pc -> native block (lazy lift+compile, with
// interpreter fallback for unlift-able blocks).
class BlockCache {
public:
    explicit BlockCache(Vm& vm) : vm_(vm) {}
    ~BlockCache() = default;

    // Returns nullptr when the block could not be lifted/compiled (caller must
    // interpret instead). Caches both successes and failures.
    BlockFn GetOrCompile(u32 pc);

    size_t block_count() const { return blocks_.size(); }
    size_t fallback_count() const { return fallbacks_.size(); }
    void InvalidateRange(u32 lo, u32 hi);  // executable-page writes call this

private:
    Vm& vm_;
    std::unordered_map<u32, CodeHolder> blocks_;
    std::unordered_map<u32, bool> fallbacks_;
};

// Drives the AOT main loop: executes native blocks with interpreter fallback.
// yield_out (optional): set to true when the guest requested a scheduler
// switch (a worker thread was created) — the caller should continue through
// the cooperative thread scheduler instead of the AOT fast path.
int RunAot(PpuContext* ctx, BlockCache& cache, bool* yield_out = nullptr);

// ---- LLVM AOT backend (real implementation only with ANYPS3_WITH_LLVM) ----

// Artifacts of one AOT compilation: the human-readable LLVM IR plus native
// objects for x86-64 and aarch64 (TASK-2: "PPC64 -> LLVM IR -> native").
struct LlvmAotArtifacts {
    std::string ll_path;
    std::string x64_obj_path;
    std::string arm64_obj_path;
    u32 blocks_built = 0;
};

// Emits .ll + native objects for a set of lifted blocks into out_dir.
// Returns false when no block could be lifted (or without the LLVM build).
bool LlvmEmitAot(const char* module_name,
                 const std::vector<std::pair<u32, const std::vector<AIROp>*>>& blocks,
                 const char* out_dir, LlvmAotArtifacts* out);

// Compiles ONE block through LLVM and returns a host-callable block function
// (executed in-process through ORC LLJIT). nullptr when the IR is unsupported.
BlockFn CompileLlvmBlock(u32 block_pc, const std::vector<AIROp>& ir);

// True when this build links the LLVM backend.
bool LlvmAvailable();

}  // namespace mini_aot

}  // namespace anyps3
