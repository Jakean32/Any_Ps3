#pragma once
// AOT recompiler framework. Implemented for real:
//   - PS3 PPU (PowerPC64 big-endian) ELF/PRX loading into a virtual map
//   - PPU branch/call scanning (direct + indirect classification) that
//     feeds the future IR lifter and identifies fallback-interpreter
//     candidates (indirect jumps through registers)
// Honestly NOT implemented yet (reported as such everywhere):
//   - LLVM IR lifting and native arm64/x86-64 code generation
//   - executable-artifact loading (see cache_manifest.h for the
//     documented Android data-file + private-dir strategy)
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "anyps3.h"
#include "bytesource.h"
#include "cache_manifest.h"
#include "game_import.h"
#include "iso9660.h"

namespace anyps3 {

// -------- virtual memory map for loaded PPU images --------
class VirtMem {
public:
    struct Segment {
        uint64_t vaddr = 0;
        uint64_t memsz = 0;
        bool exec = false;
        std::vector<uint8_t> data;  // filesz bytes
    };
    void map(uint64_t vaddr, std::vector<uint8_t> data, bool exec, uint64_t memsz);
    const Segment* find(uint64_t vaddr) const;
    const uint8_t* bytes_at(uint64_t vaddr, uint64_t len) const;
    std::vector<std::pair<uint64_t, uint64_t>> exec_ranges() const;
    std::vector<Segment> segments;
};

struct ElfInfo {
    bool valid = false;
    bool is_prx = false;      // has PS3 PRX overlay program header (0x700000A0)
    bool encrypted = false;   // no valid ELF magic (retail EBOOT.BIN case)
    uint64_t entry = 0;
    uint16_t machine = 0;
    uint8_t cls = 0;
    std::string err;
    std::string note;
};

// Load a PS3 PPU executable (ELF64 big-endian) into `mem`.
bool load_ppu_elf(const uint8_t* buf, size_t size, VirtMem* mem, ElfInfo* info);

// -------- PPU (PowerPC) branch scanner --------
struct PpuScanResult {
    bool ok = false;
    uint64_t words_scanned = 0;
    uint64_t bytes_scanned = 0;
    int direct_branches = 0;      // b / bc (and absolute variants)
    int direct_calls = 0;         // bl / bla
    int indirect_jumps = 0;       // bclr / bcctr / bcecrel? (register targets)
    std::vector<uint64_t> branch_targets;
    std::vector<uint64_t> call_targets;
    std::vector<std::string> log;
    std::string note;
};

PpuScanResult ppu_scan(const VirtMem& mem, const std::function<bool()>& cancelled = nullptr);

// -------- compile pipeline --------
struct CompileProgress {
    std::string stage;
    double fraction = 0.0;    // 0..1, REAL progress only
    std::string note;         // shown in UI log
    bool stage_done = false;
};

using ProgressFn = std::function<void(const CompileProgress&)>;

struct StageOutcome {
    StageStatus status = StageStatus::Pending;
    std::string note;
};

class CompilePipeline {
public:
    CompilePipeline(CacheStore* store, std::string logs_dir);

    // Runs the implemented stages against the game's EBOOT.BIN (from ISO
    // or folder), updating the manifest honestly. Unimplemented stages are
    // recorded with status "not_implemented" and notes the UI shows as
    // "Compiler stage not implemented yet".
    CacheStatus run(const GameRecord& rec, const ProgressFn& progress,
                    CacheManifest* manifest, std::string* err);

    // List the full designed pipeline (used by UI to render stage states).
    static std::vector<std::string> designed_stages();

private:
    StageOutcome run_analyze(const GameRecord& rec, const ProgressFn& progress,
                             std::string* log_text);
    StageOutcome run_not_implemented(const std::string& stage, const ProgressFn& progress);

    CacheStore* store_;
    std::string logs_dir_;
};

// Reads the game's EBOOT.BIN bytes from an ISO or folder (no copying).
bool read_eboot(const GameRecord& rec, std::vector<uint8_t>* out, std::string* err);

}  // namespace anyps3
