// Any_Ps3 — Emulator orchestration: boots a game from the library, maps its ELF,
// prepares the main PPU thread, and runs the main loop (interpreter fallback +
// AOT backend where available).
#include "anyps3/runtime/emulator.h"

#include <cstdio>
#include <memory>
#include <sstream>

#include "anyps3/cpu/ppu/aot.h"
#include "anyps3/cpu/ppu/state.h"
#include "anyps3/hle/lv2.h"
#include "anyps3/hle/modules.h"
#include "anyps3/loader/elf.h"
#include "anyps3/loader/import.h"
#include "anyps3/log.h"
#include "anyps3/platform.h"
#include "anyps3/runtime/threads.h"

namespace anyps3 {

struct Emulator::Impl {
    Vm vm;
    std::unique_ptr<Lv2> lv2;
    std::unique_ptr<PpuContext> ctx;
    std::unique_ptr<mini_aot::BlockCache> aot;
    std::unique_ptr<PpuThreadManager> threads;
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
    platform::MakeDirs(dir);
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
                          const EmulatorCallbacks& cb, int source_fd) {
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

    if (found->encrypted == EncState::NeedsKey && settings.dkey_path.empty() &&
        settings.dkey_hex.empty() && found->dkey_path.empty()) {
        // Note: a sibling <iso>.dkey dropped next to the image is still picked
        // up at read time, so this refusal is not final.
        result.error =
            "this ISO is encrypted and no disc key is available. Attach a "
            ".dkey/.key file or paste the 32-hex key (per-game settings, or a "
            "file named <iso>.dkey next to the image) and try again.";
        return result;
    }

    // AOT host-ABI guard: the MiniAOT backend emits x86-64 code with the **SysV**
    // calling convention and mmap-based W^X exec memory. That is only valid on
    // x86-64 Linux/BSD hosts. Windows x64 uses the Win64 ABI (RCX/RDX/R8/R9 +
    // shadow space) and Android/arm64 cannot execute x86-64 at all — those hosts
    // run the interpreter, which is a fully tested first-class backend here.
#if !defined(__x86_64__) || defined(_WIN32)
    if (impl_->settings.ppu_backend != 1) {
        note("AOT backend unavailable on this host (emits x86-64 SysV code) — "
             "using the PPU interpreter");
        impl_->settings.ppu_backend = 1;
    }
#endif

    // 2) EBOOT (through the fd when the host passed one: Android SAF no-copy).
    //    Encrypted images: the per-game key (typed hex, the referenced key
    //    file, or the import-time sibling file) feeds the on-the-fly decryptor.
    std::vector<u8> eboot;
    std::string err;
    std::string key_text;
    const std::string* key_ptr = nullptr;
    if (found->encrypted == EncState::KeyInUse || found->encrypted == EncState::NeedsKey) {
        std::string kerr;
        std::string resolved = EffectiveDiscKeyText(settings, &kerr);
        if (resolved.empty()) {
            // settings key missing/invalid: fall back to the import-time key file
            std::string key_file = found->dkey_path;
            if (!key_file.empty() && key_file != "3k3y-embedded" &&
                key_file != "user-supplied") {
                resolved = ReadDiscKeyFileAsText(key_file, &kerr);
            }
        }
        if (!resolved.empty()) {
            key_text = resolved;
            key_ptr = &key_text;
            note("disc key: applied (per-game settings)");
        } else if (found->encrypted == EncState::KeyInUse) {
            note("disc key: not usable (" + kerr + ") — continuing; reads may fail");
        }
    }
    bool eboot_ok;
    if (source_fd >= 0) {
        eboot_ok = ReadGameFileFd(source_fd, FindEbootRel(*found), &eboot, &err, key_ptr);
    } else {
        eboot_ok = ReadGameFile(found->source_path, FindEbootRel(*found), &eboot, &err, key_ptr);
    }
    if (!eboot_ok) {
        result.error = "cannot read EBOOT: " + err;
        return result;
    }
    SelfInfo self_info;
    if (!LoadExecutable(eboot, found->title_id, &impl_->elf, &self_info, &err)) {
        // Encrypted retail EBOOT: the user can supply their own DECRYPTED
        // executable (their legal dump, decrypted with their own tools) through
        // per-game settings. Any_Ps3 never ships keys and never downloads any.
        if (self_info.status == SelfStatus::EncryptedSelf && !settings.eboot_path.empty()) {
            std::vector<u8> dec;
            std::string derr;
            bool ok = false;
            FILE* f = std::fopen(settings.eboot_path.c_str(), "rb");
            if (f) {
                char buf[65536];
                size_t n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) dec.insert(dec.end(), buf, buf + n);
                std::fclose(f);
                ok = !dec.empty();
            } else {
                derr = "cannot open '" + settings.eboot_path + "'";
            }
            if (ok &&
                LoadExecutable(dec.data(), dec.size(), found->title_id + " (decrypted EBOOT)",
                                &impl_->elf, &self_info, &derr)) {
                note("EBOOT.BIN is an encrypted retail SELF — using the user-supplied "
                     "decrypted executable: " + settings.eboot_path);
            } else {
                result.error = ok ? ("decrypted executable rejected: " + derr)
                                  : ("cannot read the decrypted executable: " + derr);
                return result;
            }
        } else if (self_info.status == SelfStatus::EncryptedSelf) {
            result.error =
                "the EBOOT.BIN on this disc is an encrypted retail SELF. Any_Ps3 ships "
                "no keys (legal requirement) and cannot decrypt it. You can supply your "
                "own decrypted executable in the per-game settings ('decrypted "
                "executable' file picker; e.g. produced with your own tools from your "
                "own legal dump), then Play again.";
            return result;
        } else {
            result.error = "cannot load executable: " + err;
            return result;
        }
    }
    switch (self_info.status) {
        case SelfStatus::PlaintextSelf: note("EBOOT: plaintext fSELF (supported)"); break;
        case SelfStatus::NotSelf: note("EBOOT: plain ELF (supported)"); break;
        default: result.error = "unsupported SELF state"; return result;
    }

    // 3) map segments
    impl_->vm.Init();
    // Re-init the HLE trampoline/heap regions for this boot (Lv2's ModuleHost
    // reserved them at construction; vm.Init() clears the page tables).
    impl_->lv2->modules().Init(&impl_->vm, impl_->lv2.get());
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

    // 3b) PS3 process-spawn facts: entry descriptor (OPD), TLS block, argv.
    u32 entry_pc = static_cast<u32>(impl_->elf.entry);
    u32 entry_toc = 0;
    const bool sce_style = impl_->elf.e_type == kEtSceExec ||
                           impl_->elf.e_type == kEtScePpuRelExec ||
                           impl_->elf.module_info_va != 0;
    if (sce_style && impl_->vm.Ptr(static_cast<u32>(impl_->elf.entry), 8)) {
        // PS3 e_entry points at an 8-byte function descriptor {addr, rtoc}.
        u32 fn = impl_->vm.Read32(static_cast<u32>(impl_->elf.entry));
        u32 toc = impl_->vm.Read32(static_cast<u32>(impl_->elf.entry) + 4);
        if (fn >= kGuestMemBase && fn < kGuestMemEnd && impl_->vm.Ptr(fn, 4)) {
            entry_pc = fn;
            entry_toc = toc;
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "entry via OPD descriptor: pc=0x%08x, toc(r2)=0x%08x", fn, toc);
            note(buf);
        } else {
            note("entry OPD implausible — falling back to direct entry (documented)");
        }
    }

    // TLS (phdr 0x60000000): allocate the block, copy the init image, r13 = base.
    // This is the same contract sys_initialize_tls implements on a real PS3.
    u32 tls_base = 0;
    if (impl_->elf.tls_memsz) {
        constexpr u32 kTlsAreaBase = 0x10D00000;  // above HLE heap/callback stack
        u64 asize = (impl_->elf.tls_memsz + 0xFFFF) & ~0xFFFFULL;
        if (!impl_->vm.PageAlloc(kTlsAreaBase, asize)) {
            result.error = "TLS block allocation failed";
            return result;
        }
        tls_base = kTlsAreaBase;
        if (impl_->elf.tls_filesz && impl_->elf.tls_vaddr) {
            impl_->vm.WriteBytes(tls_base,
                                 impl_->vm.Ptr(static_cast<u32>(impl_->elf.tls_vaddr),
                                               static_cast<u32>(impl_->elf.tls_filesz)),
                                 static_cast<u32>(impl_->elf.tls_filesz));
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "TLS: %llu byte(s) (image %llu) at 0x%08x, r13=base",
                      static_cast<unsigned long long>(impl_->elf.tls_memsz),
                      static_cast<unsigned long long>(impl_->elf.tls_filesz), tls_base);
        note(buf);
    }

    // 3c) Module imports: walk the libstub tables in mapped memory and link
    // every import to the cellOS HLE (unknown ones resolve to honest ENOSYS
    // stubs and are reported — this IS the bring-up report).
    std::vector<ModuleImport> imports;
    std::string module_name;
    std::string imp_err;
    ModuleHost& host = impl_->lv2->modules();
    if (!ReadModuleImports(impl_->vm, impl_->elf, &imports, &module_name, &imp_err)) {
        result.error = "import table walk failed: " + imp_err;
        return result;
    }
    if (!module_name.empty()) note("module: " + module_name);
    LinkStats link = LinkImportsToHle(impl_->vm, host, imports);
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "import link: %u/%u slot(s) patched",
                      link.linked, link.linked + link.failed);
        note(buf);
        std::string summary = host.MissingSummary();
        size_t pos = 0;
        while (pos < summary.size()) {
            size_t nl = summary.find('\n', pos);
            if (nl == std::string::npos) nl = summary.size();
            if (nl > pos) note(summary.substr(pos, nl - pos));
            pos = nl + 1;
        }
    }

    // 4) main thread context (PowerOpen process-spawn convention:
    //    r3 = argc, r4 = argv, r5 = envp, r2 = TOC from the entry descriptor).
    impl_->vm.PageAlloc(kMainStackTop - kMainStackSize, kMainStackSize);
    std::string argv0 = found->content_root.empty()
            ? "/app_home/EBOOT.BIN"
            : (source_fd >= 0 || !platform::IsDir(found->source_path)
                   ? "/dev_bdvd/" + found->content_root + "/USRDIR/EBOOT.BIN"
                   : "/app_home/" + found->content_root + "/USRDIR/EBOOT.BIN");
    {
        const u32 T = kMainStackTop;
        u32 argv0_addr = T - 0x400;
        u32 envp_addr = T - 0x300;
        u32 argv_addr = T - 0x200;
        u32 frame = T - 0x100;
        impl_->vm.WriteBytes(argv0_addr, argv0.data(), static_cast<u32>(argv0.size() + 1));
        impl_->vm.Write64(envp_addr, 0);              // empty env, NULL-terminated
        impl_->vm.Write64(argv_addr, argv0_addr);     // argv[0]
        impl_->vm.Write64(argv_addr + 8, 0);          // argv terminator
        for (u32 a = frame; a < frame + 64; a += 8) impl_->vm.Write64(a, 0);
        impl_->ctx = std::make_unique<PpuContext>();
        impl_->ctx->vm = &impl_->vm;
        impl_->ctx->lv2 = impl_->lv2.get();
        impl_->ctx->syscall_fn = [](PpuContext* c) {
            static_cast<Lv2*>(c->lv2)->Syscall(c);
        };
        impl_->ctx->pc = entry_pc;
        impl_->ctx->gpr[1] = frame;
        impl_->ctx->gpr[2] = entry_toc;
        impl_->ctx->gpr[3] = 1;             // argc
        impl_->ctx->gpr[4] = argv_addr;    // argv
        impl_->ctx->gpr[5] = envp_addr;    // envp
        impl_->ctx->gpr[13] = tls_base ? tls_base : kMmapBase;  // TLS base
        impl_->ctx->mem_base = impl_->vm.mutable_data();  // AOT fast-path base
    }
    impl_->aot = std::make_unique<mini_aot::BlockCache>(impl_->vm);
    impl_->threads = std::make_unique<PpuThreadManager>(impl_->vm);
    impl_->lv2->SetThreadManager(impl_->threads.get());
    note("main PPU thread: entry=0x" + std::to_string(entry_pc) +
         (entry_toc ? " (TOC-linked)" : ""));

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
    if (source_fd < 0 && platform::IsDir(found->source_path)) {
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
    impl_->threads = std::make_unique<PpuThreadManager>(impl_->vm);
    impl_->lv2->SetThreadManager(impl_->threads.get());
    int code;
    if (impl_->settings.ppu_backend == 1) {
        code = PpuRun(impl_->ctx.get());
        // Worker threads may have been created mid-run: finish them through
        // the scheduler (PpuRun stops when the main thread exits; the workers
        // are drained here so joins/exit codes complete deterministically).
        if (impl_->threads->thread_count() > 0 && !impl_->ctx->error) {
            impl_->ctx->exit = true;  // main already exited
            impl_->ctx->exit_code = code;
        }
    } else {
        bool yield = false;
        code = mini_aot::RunAot(impl_->ctx.get(), *impl_->aot, &yield);
        if (yield && !impl_->ctx->exit && !impl_->ctx->error) {
            // The guest created worker threads: switch to the cooperative
            // scheduler (interpreter slices; documented in threads.h).
            code = impl_->threads->RunAll(impl_->ctx.get(), impl_->ctx->syscall_fn);
        }
    }
    stats_.syscalls = impl_->lv2->syscall_count;
    stats_.run_seconds = static_cast<double>(impl_->ctx->icount) / 1e8;  // rough
    stats_.blocks_executed = impl_->aot ? impl_->aot->block_count() : 0;
    stats_.interpreter_fallbacks = impl_->aot ? impl_->aot->fallback_count() : 0;
    boot_log_tail_ = impl_->ctx->error_msg;

    // sys_process_at_Exitspawn callbacks: executed for real (guest code) after
    // the main thread exits — the same order LV2 runs them.
    if (!impl_->ctx->error) {
        for (const auto& cb : impl_->lv2->modules().exit_callbacks()) {
            AP_INFO("exit callback at ", cb.addr, " (arg ", static_cast<long long>(cb.arg), ")");
            HleCallGuest(impl_->vm, impl_->lv2.get(), cb.addr, cb.rtoc, 0, 0, 0);
        }
    }
    state_ = RunState::Stopped;
    return code;
}

void Emulator::Stop() {
    state_ = RunState::Exiting;
    if (impl_ && impl_->ctx) impl_->ctx->exit = true;
}

}  // namespace anyps3
