#include "recompiler.h"

#include <cstring>
#include <cstdio>

#include "util.h"

namespace anyps3 {

// ================= VirtMem =================
void VirtMem::map(uint64_t vaddr, std::vector<uint8_t> data, bool exec, uint64_t memsz) {
    Segment s;
    s.vaddr = vaddr;
    s.memsz = memsz < data.size() ? data.size() : memsz;
    s.exec = exec;
    s.data = std::move(data);
    segments.push_back(std::move(s));
}

const VirtMem::Segment* VirtMem::find(uint64_t vaddr) const {
    for (const auto& s : segments)
        if (vaddr >= s.vaddr && vaddr < s.vaddr + s.memsz) return &s;
    return nullptr;
}

const uint8_t* VirtMem::bytes_at(uint64_t vaddr, uint64_t len) const {
    for (const auto& s : segments)
        if (vaddr >= s.vaddr && vaddr + len <= s.vaddr + s.data.size())
            return s.data.data() + (vaddr - s.vaddr);
    return nullptr;
}

std::vector<std::pair<uint64_t, uint64_t>> VirtMem::exec_ranges() const {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    for (const auto& s : segments)
        if (s.exec) out.push_back({s.vaddr, s.data.size()});
    return out;
}

// ================= ELF loader =================
static uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

bool load_ppu_elf(const uint8_t* buf, size_t size, VirtMem* mem, ElfInfo* info) {
    info->valid = false;
    if (size < 64) {
        info->encrypted = true;
        info->note = "too small to be an ELF (retail EBOOT.BIN is encrypted)";
        return false;
    }
    if (memcmp(buf, "\x7f""ELF", 4) != 0) {
        info->encrypted = true;
        info->note = "no ELF magic — retail EBOOT.BIN is encrypted and cannot be analyzed yet";
        return false;
    }
    info->cls = buf[4];
    uint8_t data_enc = buf[5];
    if (info->cls != 2 || data_enc != 2) {  // ELF64 + big-endian required for PPU
        info->err = "not an ELF64 big-endian image (PPU requirement)";
        return false;
    }
    info->machine = be16(buf + 18);
    if (info->machine != 21) {  // EM_PPC64
        info->err = "machine is not PowerPC64";
        return false;
    }
    info->entry = be64(buf + 24);
    uint64_t phoff = be64(buf + 32);
    uint16_t phentsize = be16(buf + 54);
    uint16_t phnum = be16(buf + 56);
    if (phoff == 0 || phnum == 0 || phentsize < 56) {
        info->err = "no program headers";
        return false;
    }
    if (phoff + (uint64_t)phnum * phentsize > size) {
        info->err = "program header table out of range";
        return false;
    }
    mem->segments.clear();
    int loaded = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t* ph = buf + phoff + (size_t)i * phentsize;
        uint32_t p_type = be32(ph + 0);
        uint32_t p_flags = be32(ph + 4);
        uint64_t p_offset = be64(ph + 8);
        uint64_t p_vaddr = be64(ph + 16);
        uint64_t p_filesz = be64(ph + 32);
        uint64_t p_memsz = be64(ph + 40);
        if (p_type == 0x700000A0) {  // PS3 PRX overlay
            info->is_prx = true;
            continue;
        }
        if (p_type != 1) continue;  // PT_LOAD only
        if (p_offset + p_filesz > size) {
            info->err = "segment data out of range";
            return false;
        }
        bool exec = (p_flags & 1) != 0;
        std::vector<uint8_t> data(p_filesz);
        if (p_filesz) memcpy(data.data(), buf + p_offset, p_filesz);
        mem->map(p_vaddr, std::move(data), exec, p_memsz);
        loaded++;
    }
    if (loaded == 0) {
        info->err = "no loadable segments";
        return false;
    }
    info->valid = true;
    info->note = info->is_prx ? "PS3 PRX module loaded" : "PPU executable loaded";
    return true;
}

// ================= PPU scanner =================
// PowerPC instruction classification (opcode = top 6 bits):
//   18 (0b010010) I-form  b/ba/bl/bla   — LK=call, AA=absolute
//   16 (0b010000) B-form  bc/bca/bcl/bcla
//   19 (0b010011) XL-form bclr (xo=16), bcctr (xo=528) — register-indirect
PpuScanResult ppu_scan(const VirtMem& mem, const std::function<bool()>& cancelled) {
    PpuScanResult r;
    auto ranges = mem.exec_ranges();
    for (const auto& rg : ranges) {
        const VirtMem::Segment* seg = mem.find(rg.first);
        if (!seg) continue;
        size_t words = seg->data.size() / 4;
        for (size_t i = 0; i < words; i++) {
            if (cancelled && cancelled()) {
                r.note = "scan cancelled";
                return r;
            }
            uint32_t w = be32(seg->data.data() + i * 4);
            uint64_t pc = seg->vaddr + i * 4;
            r.words_scanned++;
            uint32_t op = w >> 26;
            // For big-endian PPC words: primary opcode = bits 0..5 => w>>26.
            // For op 16/18 the AA/LK flags are the two LSBs of the word.
            uint32_t lsb = w & 3;      // AA(bit30) = value>>1, LK(bit31) = value&1
            bool aa = (lsb >> 1) & 1;
            bool link = lsb & 1;
            if (op == 18) {  // b / bl / ba / bla
                uint32_t li = (w >> 2) & 0xFFFFFF;
                int64_t ext = (int32_t)(li << 8) >> 8;  // sign-extend 24-bit
                uint64_t target = aa ? (uint64_t)((int64_t)(li << 2) & 0xFFFFFFFF)
                                     : (pc + (int64_t)(ext << 2));
                if (link) {
                    r.direct_calls++;
                    r.call_targets.push_back(target);
                } else {
                    r.direct_branches++;
                    r.branch_targets.push_back(target);
                }
            } else if (op == 16) {  // bc family
                int32_t li = (int32_t)((w >> 2) & 0x3FFF);
                if (li & 0x2000) li |= (int32_t)0xFFFFC000;  // sign-extend 14-bit
                uint64_t target = aa ? (uint64_t)(li & 0xFFFFFFFF) : (pc + (int64_t)(li << 2));
                if (link) {
                    r.direct_calls++;
                    r.call_targets.push_back(target);
                } else {
                    r.direct_branches++;
                    r.branch_targets.push_back(target);
                }
            } else if (op == 19) {
                uint32_t xo = (w >> 1) & 0x3FF;  // bits 21..30
                if (xo == 16 || xo == 528) {
                    // bclr(16) / bcctr(528): indirect — fallback-interpreter candidates
                    r.indirect_jumps++;
                }
            }
        }
    }
    r.bytes_scanned = r.words_scanned * 4;
    r.ok = true;
    r.note = "scan complete";
    return r;
}

// ================= pipeline =================
CompilePipeline::CompilePipeline(CacheStore* store, std::string logs_dir)
    : store_(store), logs_dir_(std::move(logs_dir)) {}

std::vector<std::string> CompilePipeline::designed_stages() {
    return {"analyze", "ir_lift", "codegen", "link", "cache_write"};
}

bool read_eboot(const GameRecord& rec, std::vector<uint8_t>* out, std::string* err) {
    if (rec.src_type == SourceType::IsoFile) {
        auto fs = FileSource::open(rec.src_ref, err);
        if (!fs) return false;
        auto iso = Iso9660::open(std::shared_ptr<ByteSource>(fs.get(), [](ByteSource*) {}), err);
        if (!iso) return false;
        return iso->read_file("PS3_GAME/EBOOT.BIN", out, 64 * 1024 * 1024, err);
    }
    if (rec.src_type == SourceType::Folder) {
        static const char* kPaths[] = {"PS3_GAME/EBOOT.BIN", "EBOOT.BIN"};
        for (const char* rel : kPaths) {
            std::string p = path_join(rec.src_ref, rel);
            if (read_file(p, out)) return true;
        }
        if (err) *err = "EBOOT.BIN not found in " + rec.src_ref;
        return false;
    }
    if (err) *err = "SAF-URI EBOOT access requires an open fd (platform layer)";
    return false;
}

StageOutcome CompilePipeline::run_analyze(const GameRecord& rec, const ProgressFn& progress,
                                          std::string* log_text) {
    StageOutcome out;
    CompileProgress p;
    p.stage = "analyze";
    p.note = "reading EBOOT.BIN (random access, no copy)";
    p.fraction = 0.1;
    if (progress) progress(p);

    std::vector<uint8_t> eboot;
    std::string err;
    if (!read_eboot(rec, &eboot, &err)) {
        out.status = StageStatus::Failed;
        out.note = "cannot read EBOOT.BIN: " + err;
        return out;
    }
    p.fraction = 0.3;
    p.note = "EBOOT.BIN: " + std::to_string(eboot.size()) + " bytes";
    if (progress) progress(p);

    VirtMem mem;
    ElfInfo info;
    char line[256];
    std::string log;
    snprintf(line, sizeof line, "EBOOT.BIN size: %zu bytes\n", eboot.size());
    log += line;
    if (!load_ppu_elf(eboot.data(), eboot.size(), &mem, &info)) {
        if (info.encrypted) {
            out.status = StageStatus::Blocked;
            out.note = "encrypted EBOOT.BIN — AOT analysis requires decrypted binaries; "
                       "not implemented yet (no Sony keys are used or shipped)";
            log += "ELF magic not found — encrypted retail binary\n";
            log += "analysis blocked honestly; no fake progress\n";
        } else {
            out.status = StageStatus::Failed;
            out.note = info.err;
            log += "ELF load error: " + info.err + "\n";
        }
        if (log_text) *log_text = log;
        return out;
    }
    p.fraction = 0.5;
    p.note = info.note;
    if (progress) progress(p);
    snprintf(line, sizeof line, "%s, entry=0x%llx, %zu segment(s)\n", info.note.c_str(),
             (unsigned long long)info.entry, mem.segments.size());
    log += line;

    PpuScanResult scan = ppu_scan(mem);
    p.fraction = 0.9;
    p.note = "scanned " + std::to_string(scan.words_scanned) + " instructions";
    if (progress) progress(p);
    snprintf(line, sizeof line,
             "scan: %llu words, %d direct branches, %d direct calls, %d indirect jumps\n",
             (unsigned long long)scan.words_scanned, scan.direct_branches, scan.direct_calls,
             scan.indirect_jumps);
    log += line;
    log += "indirect jumps are fallback-interpreter candidates\n";

    p.fraction = 1.0;
    p.stage_done = true;
    p.note = "analysis complete";
    if (progress) progress(p);
    out.status = StageStatus::Ok;
    out.note = "ELF loaded and scanned: " + std::to_string(scan.words_scanned) +
               " instrs, " + std::to_string(scan.direct_calls) + " calls, " +
               std::to_string(scan.indirect_jumps) + " indirect jumps";
    if (log_text) *log_text = log;
    return out;
}

StageOutcome CompilePipeline::run_not_implemented(const std::string& stage,
                                                  const ProgressFn& progress) {
    StageOutcome out;
    out.status = StageStatus::NotImplemented;
    out.note = "Compiler stage not implemented yet";
    CompileProgress p;
    p.stage = stage;
    p.fraction = 0.0;
    p.stage_done = true;
    p.note = "Compiler stage not implemented yet — skipped honestly (no fake progress)";
    if (progress) progress(p);
    return out;
}

CacheStatus CompilePipeline::run(const GameRecord& rec, const ProgressFn& progress,
                                 CacheManifest* manifest, std::string* err) {
    CacheManifest m;
    if (!store_->ensure(rec, &m, err)) return CacheStatus::Failed;
    m.status = CacheStatus::Compiling;
    m.updated_ms = now_ms();
    m.stages.clear();
    store_->save(m, nullptr);

    std::string log;
    StageOutcome a = run_analyze(rec, progress, &log);
    StageInfo sa;
    sa.name = "analyze";
    sa.status = stage_status_name(a.status);
    sa.note = a.note;
    m.stages.push_back(sa);

    StageInfo si;
    si.name = "ir_lift";
    StageOutcome o = run_not_implemented("ir_lift", progress);
    si.status = stage_status_name(o.status);
    si.note = o.note;
    m.stages.push_back(si);

    si.name = "codegen";
    o = run_not_implemented("codegen", progress);
    si.status = stage_status_name(o.status);
    si.note = o.note;
    m.stages.push_back(si);

    si.name = "link";
    o = run_not_implemented("link", progress);
    si.status = stage_status_name(o.status);
    si.note = o.note;
    m.stages.push_back(si);

    // honest final status: Ready requires every stage OK; unreachable today
    bool all_ok = true;
    for (const auto& s : m.stages)
        if (s.status != stage_status_name(StageStatus::Ok)) all_ok = false;
    m.status = all_ok ? CacheStatus::Ready
                      : (a.status == StageStatus::Failed ? CacheStatus::Failed
                                                         : CacheStatus::NotCompiled);
    if (a.status == StageStatus::Blocked)
        m.note = "analysis blocked (encrypted EBOOT.BIN); codegen not implemented yet";
    else if (m.status == CacheStatus::NotCompiled)
        m.note = "analysis complete; remaining compiler stages not implemented yet";
    else
        m.note = "all stages ok";
    m.updated_ms = now_ms();

    // real per-run log file (unique name even for same-millisecond runs)
    make_dirs(logs_dir_);
    std::string lp;
    for (int seq = 0;; seq++) {
        lp = path_join(logs_dir_, "compile_" + std::to_string(m.updated_ms) +
                                    (seq ? "_" + std::to_string(seq) : "") + ".log");
        if (!file_exists(lp)) break;
    }
    write_file(lp, log.data(), log.size());

    store_->save(m, nullptr);
    *manifest = m;
    return m.status;
}

}  // namespace anyps3
