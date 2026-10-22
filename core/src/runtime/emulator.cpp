// Any_Ps3 — Emulator orchestration: boots a game from the library, maps its ELF,
// prepares the main PPU thread, and runs the main loop (interpreter fallback +
// AOT backend where available).
#include "anyps3/runtime/emulator.h"

#include <memory>
#include <sstream>
#include <sys/stat.h>

#include "anyps3/cpu/ppu/aot.h"
#include "anyps3/cpu/ppu/state.h"
#include "anyps3/hle/lv2.h"
#include "anyps3/loader/elf.h"
#include "anyps3/loader/import.h"
#include "anyps3/log.h"

namespace anyps3 {

struct Emulator::Impl {
    Vm vm;
    std::unique_ptr<Lv2> lv2;
    std::unique_ptr<PpuContext> ctx;
    std::unique_ptr<mini_aot::BlockCache> aot;
    ElfImage elf;
    GameSettings settings;
};

static std::string FindEbootRel(const GameInfo& g) {
    return g.content_root.empty() ? "EBOOT.BIN" : g.content_root + "/EBOOT.BIN";
}

// ---------------------------------------------------------------------------
// AotCache — key=value manifest + a single native blob slot per game/module.
// A real implementation shards blobs per module and per block; the manifest
// schema below is the contract the runtime checks before reusing translations.
// ---------------------------------------------------------------------------
static constexpr u64 kLifterVersion = 1;

u64 AotCache::HashModule(const ElfImage& img) {
    u64 h = 0xcbf29ce484222325ULL;
    for (const auto& s : img.segments) {
        if (s.flags & 1) {  // PF_X
            h = Fnv1a64(s.bytes.data(), s.bytes.size(), h);
        }
    }
    return h;
}

bool AotCache::Load(const std::string& game_id, Manifest* out) const {
    IniConfig ini;
    if (!ini.LoadFromFile(root_ + "/" + game_id + "/manifest.ini")) return false;
    out->game_id = ini.Get("game_id");
    out->module_hash = std::stoull(ini.Get("module_hash", "0"));
    out->lifter_version = std::stoull(ini.Get("lifter_version", "0"));
    out->backend_id = std::stoull(ini.Get("backend_id", "0"));
    out->block_count = static_cast<u32>(std::stoul(ini.Get("block_count", "0")));
    out->interpreter_only = static_cast<u32>(std::stoul(ini.Get("interpreter_only", "0")));
    return !out->game_id.empty();
}

bool AotCache::Store(const std::string& game_id, const Manifest& m,
                     const std::vector<u8>& blob) const {
    std::string dir = root_ + "/" + game_id;
    std::string cur;
    size_t pos = 0;
    while ((pos = dir.find('/', pos + 1)) != std::string::npos) {
        cur = dir.substr(0, pos);
        ::mkdir(cur.c_str(), 0755);
    }
    ::mkdir(dir.c_str(), 0755);
    IniConfig ini;
    ini.Set("game_id", m.game_id);
    ini.SetInt("module_hash", static_cast<int>(0));  // 64-bit safe write below
    ini.Set("module_hash", std::to_string(m.module_hash));
    ini.Set("lifter_version", std::to_string(m.lifter_version));
    ini.SetInt("backend_id", static_cast<int>(m.backend_id));
    ini.SetInt("block_count", static_cast<int>(m.block_count));
    ini.SetInt("interpreter_only", static_cast<int>(m.interpreter_only));
    ini.SaveToFile(dir + "/manifest.ini");
    if (!blob.empty()) {
        FILE* f = std::fopen(BlobPath(game_id).c_str(), "wb");
        if (!f) return false;
        std::fwrite(blob.data(), 1, blob.size(), f);
        std::fclose(f);
    }
    return true;
}

bool AotCache::Clear(const std::string& game_id) const {
    std::remove((root_ + "/" + game_id + "/manifest.ini").c_str());
    std::remove(BlobPath(game_id).c_str());
    std::remove((root_ + "/" + game_id).c_str());  // may fail if non-empty: fine
    return true;
}

std::string AotCache::BlobPath(const std::string& game_id) const {
    return root_ + "/" + game_id + "/blocks.bin";
}

// ---------------------------------------------------------------------------
// Emulator
// ---------------------------------------------------------------------------
Emulator::Emulator(AppConfig& app) : app_(app) {
    impl_ = new Impl();
    impl_->lv2 = std::make_unique<Lv2>(impl_->vm);
    impl_->ctx = std::make_unique<PpuContext>();
}

Emulator::~Emulator() {
    delete impl_;
}

BootResult Emulator::Boot(const std::string& title_id, const GameSettings& settings,
                          const EmulatorCallbacks& cb) {
    state_ = RunState::Booting;
    BootResult result;
    if (cb.state_changed) cb.state_changed(RunState::Booting);
    std::vector<std::string> log;
    auto note = [&](const std::string& s) {
        log.push_back(s);
        if (cb.log) cb.log(s);
    };

    // 1) library lookup
    GameLibrary lib(app_.config_dir);
    std::vector<LibraryEntry> entries;
    lib.List(&entries);
    const GameInfo* found = nullptr;
    for (const auto& e : entries) {
        if (e.info.title_id == title_id) found = &e.info;
    }
    if (!found) {
        result.error = "game not found in library: " + title_id;
        return result;
    }
    impl_->settings = settings;

    // 2) EBOOT
    std::vector<u8> eboot;
    std::string err;
    if (!ReadGameFile(found->source_path, FindEbootRel(*found), &eboot, &err)) {
        result.error = "cannot read EBOOT: " + err;
        return result;
    }
    SelfInfo self_info;
    if (!LoadExecutable(eboot, found->title_id, &impl_->elf, &self_info, &err)) {
        result.error = "cannot load executable: " + err;
        return result;
    }
    switch (self_info.status) {
        case SelfStatus::PlaintextSelf: note("EBOOT: plaintext fSELF (supported)"); break;
        case SelfStatus::NotSelf: note("EBOOT: plain ELF (supported)"); break;
        default: result.error = "unsupported SELF state"; return result;
    }

    // 3) map segments
    impl_->vm.Init();
    for (const auto& seg : impl_->elf.segments) {
        u64 asize = (seg.mem_size + 0xFFFF) & ~0xFFFFULL;
        if (!impl_->vm.PageAlloc(static_cast<u32>(seg.vaddr), asize)) {
            result.error = "segment mapping failed (overlaps or out of range)";
            return result;
        }
        if (seg.file_size > 0) {
            if (!impl_->vm.WriteBytes(static_cast<u32>(seg.vaddr), seg.bytes.data(),
                                      static_cast<u32>(seg.bytes.size()))) {
                result.error = "segment copy out of range";
                return result;
            }
        }
        if (seg.flags & 1) {  // PF_X: mark executable pages
            for (u64 off = 0; off < asize; off += kPageSize64K) {
                impl_->vm.PageSetExec(static_cast<u32>(seg.vaddr + off), true);
            }
        }
    }
    note("mapped " + std::to_string(impl_->elf.segments.size()) + " segment(s)");

    // 4) main thread context
    impl_->vm.PageAlloc(kMainStackTop - kMainStackSize, kMainStackSize);
    impl_->ctx = std::make_unique<PpuContext>();
    impl_->ctx->vm = &impl_->vm;
    impl_->ctx->lv2 = impl_->lv2.get();
    impl_->ctx->syscall_fn = [](PpuContext* c) {
        static_cast<Lv2*>(c->lv2)->Syscall(c);
    };
    impl_->ctx->pc = static_cast<u32>(impl_->elf.entry);
    impl_->ctx->gpr[1] = kMainStackTop - 0x100;  // red zone + align
    impl_->ctx->gpr[2] = 0;  // TOC (real games get it from .opd — future work)
    impl_->ctx->gpr[13] = kMmapBase;  // TLS base placeholder
    impl_->ctx->mem_base = impl_->vm.mutable_data();  // AOT fast-path base
    impl_->aot = std::make_unique<mini_aot::BlockCache>(impl_->vm);
    note("main PPU thread: entry=0x" + std::to_string(impl_->elf.entry));

    // 5) AOT cache manifest check (translation itself is wired in Phase 6)
    AotCache cache(app_.CacheRoot());
    AotCache::Manifest m;
    const u64 hash = AotCache::HashModule(impl_->elf);
    if (cache.Load(title_id, &m) && m.module_hash == hash &&
        m.lifter_version == kLifterVersion) {
        note("AOT cache: HIT (reusing " + std::to_string(m.block_count) + " block(s))");
    } else if (settings.ppu_backend == 1) {
        note("AOT cache: bypassed (interpreter-only mode)");
    } else {
        note("AOT cache: MISS (hash " + std::to_string(hash) + "); block translation runs "
             "on first execution (lazy AOT) — interpreter fallback covers the rest");
        AotCache::Manifest fresh;
        fresh.game_id = title_id;
        fresh.module_hash = hash;
        fresh.lifter_version = kLifterVersion;
        fresh.backend_id = settings.ppu_backend == 1 ? 0 : 1;
        fresh.block_count = 0;
        fresh.interpreter_only = 1;  // until the AOT path is enabled per-block
        cache.Store(title_id, fresh, {});
    }
    if (cb.progress) cb.progress(100, "boot ready");

    // app_home: for folder dumps, map the content dir; ISO-backed reads flow
    // through the game-file reader in the FS layer (Phase 6 wires sys_fs to it).
    std::string app_home;
    struct stat st;
    if (::stat(found->source_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        app_home = found->source_path;
        if (!found->content_root.empty()) app_home += "/" + found->content_root;
    }
    impl_->lv2->SetContentRoot(app_home);
    impl_->lv2->SetConfigDir(app_.config_dir);
    result.ok = true;
    result.notes = std::move(log);
    state_ = RunState::Running;
    if (cb.state_changed) cb.state_changed(RunState::Running);
    return result;
}

int Emulator::Run() {
    if (state_.load() != RunState::Running) return -1;
    stats_ = {};
    int code;
    if (impl_->settings.ppu_backend == 1) {
        code = PpuRun(impl_->ctx.get());
    } else {
        code = mini_aot::RunAot(impl_->ctx.get(), *impl_->aot);
    }
    stats_.syscalls = impl_->lv2->syscall_count;
    stats_.run_seconds = static_cast<double>(impl_->ctx->icount) / 1e8;  // rough
    stats_.blocks_executed = impl_->aot ? impl_->aot->block_count() : 0;
    stats_.interpreter_fallbacks = impl_->aot ? impl_->aot->fallback_count() : 0;
    boot_log_tail_ = impl_->ctx->error_msg;
    state_ = RunState::Stopped;
    return code;
}

void Emulator::Stop() {
    state_ = RunState::Exiting;
    if (impl_ && impl_->ctx) impl_->ctx->exit = true;
}

}  // namespace anyps3
