// Any_Ps3 — AOT compile pipeline with an HONEST stage registry.
//
// Design (mirrors the project's long-term architecture):
//   inspect : parse PARAM.SFO, hash the source (streamed)      — IMPLEMENTED
//   analyze : load ELF, discover functions, decode stats       — IMPLEMENTED
//   lift    : PPU/SPU -> LLVM IR                               — NOT IMPLEMENTED
//   codegen : LLVM IR -> native arm64/x64 objects              — NOT IMPLEMENTED
//   link    : produce loadable per-game module                 — NOT IMPLEMENTED
//
// pipeline_compile() runs the implemented stages with REAL progress and log
// lines, then STOPS at the first stage that is not implemented and says so.
// There are no fake progress bars and no fake success anywhere.
//
// Android note (documented approach): compiled artifacts live in the user's
// chosen shared-storage folder as DATA files; at launch they are copied into
// the app's private directory and loaded from there (executable code may not
// run from shared storage). This loader stage is part of "link"/launcher and
// is therefore also "not implemented yet" today.
#pragma once

#include "types.h"
#include "source.h"
#include "manifest.h"
#include "elf.h"
#include "ppu.h"
#include "spu.h"

#include <functional>

namespace any_ps3 {

struct StageInfo {
    const char* id;
    const char* name;
    bool implemented;
};

const std::vector<StageInfo>& pipeline_stages();

struct PipelineProgress {
    int stage_index = 0;      // index into pipeline_stages()
    float fraction = 0.f;     // 0..1 within the stage (real work-based)
    std::string line;         // real log line for the UI log view
};

struct PipelineResult {
    bool completed = false;               // true only if ALL stages ran
    std::string stopped_at;               // stage id where it stopped
    std::string reason;                   // honest reason
    Manifest manifest;                    // filled manifest (caller persists it)
};

struct AnalyzeStats {
    uint64_t functions = 0;
    uint64_t insns_decoded = 0;
    uint64_t insns_unknown = 0;
    uint64_t segments = 0;
    uint64_t entry = 0;
    bool has_elf = false;
    bool self_encrypted = false;
};

// Runs implemented stages, stops honestly at unimplemented ones.
// progress callback receives real per-chunk / per-step updates.
PipelineResult pipeline_compile(GameSource& src,
                                const std::string& game_id_hint,
                                const std::string& source_kind,
                                std::function<void(const PipelineProgress&)> progress);

// Recompute the source signature and compare against a stored manifest.
// Returns: 0 = up to date, 1 = outdated, <0 = error (hash unavailable).
int pipeline_check_outdated(GameSource& src, const Manifest& m,
                            std::function<void(float)> progress);

} // namespace any_ps3
