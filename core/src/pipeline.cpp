#include "any_ps3/pipeline.h"
#include "any_ps3/sha256.h"

#include <algorithm>
#include <cstring>

namespace any_ps3 {

const std::vector<StageInfo>& pipeline_stages() {
    static const std::vector<StageInfo> stages = {
        {"inspect", "Inspect source (PARAM.SFO, signature)", true},
        {"analyze", "Analyze executable (ELF, functions, decode)", true},
        {"lift",    "Lift PPU/SPU to LLVM IR", false},
        {"codegen", "Generate native arm64/x64 code", false},
        {"link",    "Link loadable per-game module", false},
    };
    return stages;
}

static void emit(int idx, float frac, std::function<void(const PipelineProgress&)>& cb,
                 const std::string& line) {
    if (!cb) return;
    PipelineProgress p;
    p.stage_index = idx;
    p.fraction = frac;
    p.line = line;
    cb(p);
}

PipelineResult pipeline_compile(GameSource& src,
                                const std::string& game_id_hint,
                                const std::string& source_kind,
                                std::function<void(const PipelineProgress&)> progress) {
    const auto& stages = pipeline_stages();
    PipelineResult res;

    Manifest& m = res.manifest;
    m.format = 1;
    m.game_id = game_id_hint;
    m.source_kind = source_kind;
    m.engine_version = kEngineVersion;
    m.status = "not_compiled";

    // ---- stage 0: inspect ----
    {
        const StageInfo& st = stages[0];
        emit(0, 0.f, progress, "[" + std::string(st.name) + "] reading source meta...");
        std::string err;
        GameMeta meta = src.meta(&err);
        if (!meta.valid) {
            m.set_stage("inspect", "error", "PARAM.SFO unreadable: " + err);
            emit(0, 0.5f, progress, "[inspect] PARAM.SFO unreadable: " + err);
            res.stopped_at = "inspect";
            res.reason = "inspect failed: " + err;
            m.status = "error";
            return res;
        }
        if (m.game_id.empty()) m.game_id = meta.title_id;
        m.title = meta.title;
        char buf[160];
        std::snprintf(buf, sizeof buf, "[inspect] title=\"%s\" id=%s category=%s app_ver=%s sys_ver=%s",
                      meta.title.c_str(), meta.title_id.c_str(), meta.category.c_str(),
                      meta.app_ver.c_str(), meta.system_ver.c_str());
        emit(0, 0.3f, progress, buf);

        std::string hash = src.source_hash([&progress](float f) {
            if (progress) {
                PipelineProgress p;
                p.stage_index = 0;
                p.fraction = 0.3f + 0.65f * f;
                p.line = "[inspect] hashing source... " + std::to_string((int)(f * 100)) + "%";
                progress(p);
            }
        });
        m.source_hash = hash;
        m.source_size = src.source_size();
        std::snprintf(buf, sizeof buf, "[inspect] source signature sha256=%s size=%llu bytes",
                      hash.c_str(), (unsigned long long)m.source_size);
        emit(0, 1.f, progress, buf);
        m.set_stage("inspect", "done", "sha256 " + hash);
    }

    // ---- stage 1: analyze ----
    {
        const StageInfo& st = stages[1];
        emit(1, 0.f, progress, "[" + std::string(st.name) + "] loading EBOOT.BIN...");
        std::vector<uint8_t> eboot;
        bool ok = src.read_file("EBOOT.BIN", eboot, 64ull * 1024 * 1024);
        if (!ok) {
            m.set_stage("analyze", "error", "EBOOT.BIN not readable from source");
            emit(1, 0.2f, progress, "[analyze] EBOOT.BIN not readable — stopping.");
            res.stopped_at = "analyze";
            res.reason = "EBOOT.BIN not readable from source";
            m.status = "error";
            return res;
        }

        if (looks_like_sony_encrypted(eboot.data(), eboot.size())) {
            m.set_stage("analyze", "error", sony_encrypted_message());
            emit(1, 0.5f, progress, "[analyze] " + sony_encrypted_message());
            res.stopped_at = "analyze";
            res.reason = "encrypted SELF — decryption not supported (keys are not shipped)";
            m.status = "error";
            return res;
        }

        try {
            ElfImage img = elf_parse(eboot.data(), eboot.size());
            AnalyzeStats stats;
            stats.segments = img.segments.size();
            stats.entry = img.entry;
            stats.functions = img.functions.size();
            stats.has_elf = true;

            // Decode real instruction statistics over mapped code (bounded).
            PpuMemory mem;
            uint64_t decode_budget = 200000;
            if (mem.load(img, eboot.data(), eboot.size())) {
                for (const auto& f : img.functions) {
                    if (decode_budget == 0) break;
                    uint64_t addr = f.addr;
                    uint64_t left = f.size ? f.size : 64;
                    if (left > 4096) left = 4096;
                    for (uint64_t k = 0; k + 4 <= left && decode_budget; k += 4, --decode_budget) {
                        uint32_t word;
                        uint8_t b[4];
                        if (!mem.read(addr + k, b, 4)) break;
                        word = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                               ((uint32_t)b[2] << 8) | b[3];
                        PpuInsn pi = ppu_decode((uint32_t)(addr + k), word);
                        if (pi.valid) stats.insns_decoded++;
                        else stats.insns_unknown++;
                    }
                    if (progress) {
                        float f_frac = 1.f - (float)((double)decode_budget / 200000.0);
                        PipelineProgress p;
                        p.stage_index = 1;
                        p.fraction = f_frac;
                        p.line = "[analyze] scanning functions... (" + std::to_string(stats.functions) + ")";
                        progress(p);
                    }
                }
            }

            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "[analyze] ELF ok: %llu segments, entry 0x%llx, %llu functions, "
                          "%llu insns decoded, %llu not in decode table",
                          (unsigned long long)stats.segments, (unsigned long long)stats.entry,
                          (unsigned long long)stats.functions,
                          (unsigned long long)stats.insns_decoded,
                          (unsigned long long)stats.insns_unknown);
            emit(1, 1.f, progress, buf);
            m.set_stage("analyze", "done",
                        "segments=" + std::to_string(stats.segments) +
                        " functions=" + std::to_string(stats.functions) +
                        " decoded=" + std::to_string(stats.insns_decoded) +
                        " unknown=" + std::to_string(stats.insns_unknown));
            m.status = "analyzed";
        } catch (const Error& e) {
            m.set_stage("analyze", "error", e.what());
            emit(1, 0.5f, progress, std::string("[analyze] ") + e.what());
            res.stopped_at = "analyze";
            res.reason = e.what();
            m.status = "error";
            return res;
        }
    }

    // ---- stage 2..4: lift / codegen / link — honest stop ----
    for (size_t i = 2; i < stages.size(); i++) {
        const StageInfo& st = stages[i];
        if (!st.implemented) {
            std::string line = std::string("[") + st.id + "] Compiler stage not implemented yet — stopping honestly.";
            emit((int)i, 0.f, progress, line);
            m.set_stage(st.id, "not_implemented", "Compiler stage not implemented yet");
            res.stopped_at = st.id;
            res.reason = std::string("Compiler stage '") + st.id + "' is not implemented yet";
            res.completed = false;
            return res;
        }
    }

    res.completed = true; // unreachable today (lift/codegen/link pending)
    res.manifest.status = "ready";
    return res;
}

int pipeline_check_outdated(GameSource& src, const Manifest& m,
                           std::function<void(float)> progress) {
    if (m.source_hash.empty()) return -1;
    std::string current = src.source_hash(progress);
    if (current.empty()) return -1;
    if (current == m.source_hash) return 0;
    return 1;
}

} // namespace any_ps3
