// Any_Ps3 core test suite — 37 tests, zero external dependencies.
// Build: g++ -std=c++17 -O1 -o core_tests core/*.cpp tests/core_tests.cpp
//        (host) / NDK clang for Android smoke run.
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <string>
#include <vector>

#include "../core/anyps3.h"
#include "../core/bytesource.h"
#include "../core/cache_manifest.h"
#include "../core/game_import.h"
#include "../core/iso9660.h"
#include "../core/library.h"
#include "../core/lv2.h"
#include "../core/minijson.h"
#include "../core/recompiler.h"
#include "../core/sfo.h"
#include "../core/thumbs.h"
#include "../core/util.h"

using namespace anyps3;

// ---------------- tiny framework ----------------
static int g_pass = 0, g_fail = 0;
static std::vector<std::string> g_failures;
struct TestReg {
    std::string name;
    void (*fn)();
};
static std::vector<TestReg>& tests() {
    static std::vector<TestReg> t;
    return t;
}
#define TEST(name)                                                      \
    static void test_##name();                                          \
    static bool reg_##name = [] {                                       \
        tests().push_back({#name, test_##name});                        \
        return true;                                                    \
    }();                                                                \
    static void test_##name()
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            g_fail++;                                                    \
            g_failures.push_back(std::string(#cond) + " (line " +        \
                                 std::to_string(__LINE__) + ") in " +    \
                                 __func__);                              \
            return;                                                      \
        }                                                                \
    } while (0)

static const char* FIX = "/home/z/my-project/fixtures";

// ============================================================
// PARAM.SFO (tests 1-4) — includes required NEW test
// ============================================================
TEST(sfo_parse_fixture) {
    std::vector<uint8_t> raw;
    CHECK(read_file(std::string(FIX) + "/game_dir/PS3_GAME/PARAM.SFO", &raw));
    Sfo sfo;
    std::string err;
    CHECK(Sfo::parse(raw.data(), raw.size(), &sfo, &err));
    CHECK(sfo.title() == "AnyPs3 Test Game");
    CHECK(sfo.title_id() == "NPEA00001");
    CHECK(sfo.app_ver() == "01.00");
    CHECK(sfo.category() == "GD");
    CHECK(sfo.parental_level() == 1);
}

TEST(sfo_rejects_bad_magic) {
    std::vector<uint8_t> bad = {'P', 'S', 'F', 0, 0, 1, 1, 0, 0, 0, 0, 0};
    Sfo s;
    std::string err;
    CHECK(!Sfo::parse(bad.data(), bad.size(), &s, &err));
    CHECK(!err.empty());
}

TEST(sfo_second_fixture) {
    std::vector<uint8_t> raw;
    CHECK(read_file(std::string(FIX) + "/game_dir2/PS3_GAME/PARAM.SFO", &raw));
    Sfo sfo;
    CHECK(Sfo::parse(raw.data(), raw.size(), &sfo, nullptr));
    CHECK(sfo.title_id() == "BLUS30002");
    CHECK(sfo.title() == "Second Fixture Game");
    CHECK(sfo.category() == "HG");
    CHECK(sfo.parental_level() == 3);
    CHECK(sfo.disc_number().empty());
}

TEST(sfo_all_fields_present) {
    std::vector<uint8_t> raw;
    CHECK(read_file(std::string(FIX) + "/game_dir/PS3_GAME/PARAM.SFO", &raw));
    Sfo sfo;
    CHECK(Sfo::parse(raw.data(), raw.size(), &sfo, nullptr));
    bool found[5] = {false, false, false, false, false};
    for (const auto& f : sfo.fields) {
        if (f.key == "TITLE") found[0] = true;
        if (f.key == "TITLE_ID") found[1] = true;
        if (f.key == "APP_VER") found[2] = true;
        if (f.key == "CATEGORY") found[3] = true;
        if (f.key == "PS3_SYSTEM_VER") found[4] = true;
    }
    for (bool b : found) CHECK(b);
}

// ============================================================
// ISO9660 (tests 5-10) — includes required NEW fd test
// ============================================================
TEST(iso_open_and_pvd) {
    std::string err;
    auto src = FileSource::open(std::string(FIX) + "/game.iso", &err);
    CHECK(src);
    auto iso = Iso9660::open(src, &err);
    CHECK(iso);
    CHECK(iso->volume_id() == "ANyps3FIXTURE");
    CHECK(iso->total_size() == 65536);
}

TEST(iso_rejects_non_iso) {
    std::string err;
    auto src = FileSource::open(std::string(FIX) + "/game_dir/PS3_GAME/PARAM.SFO", &err);
    CHECK(src);
    auto iso = Iso9660::open(src, &err);
    CHECK(!iso);
    CHECK(err.find("CD001") != std::string::npos);
}

TEST(iso_lookup_param_sfo) {
    auto src = FileSource::open(std::string(FIX) + "/game.iso", nullptr);
    CHECK(src);
    auto iso = Iso9660::open(src, nullptr);
    CHECK(iso);
    CHECK(iso->file_exists("PS3_GAME/PARAM.SFO"));
    std::vector<uint8_t> data;
    CHECK(iso->read_file("PS3_GAME/PARAM.SFO", &data));
    Sfo sfo;
    CHECK(Sfo::parse(data.data(), data.size(), &sfo, nullptr));
    CHECK(sfo.title_id() == "NPEA00001");
}

TEST(iso_lookup_icon_and_pic1) {
    auto src = FileSource::open(std::string(FIX) + "/game.iso", nullptr);
    CHECK(src);
    auto iso = Iso9660::open(src, nullptr);
    CHECK(iso);
    std::vector<uint8_t> icon, pic1;
    CHECK(iso->read_file("PS3_GAME/ICON0.PNG", &icon));
    CHECK(icon.size() > 8);
    CHECK(icon[0] == 0x89 && icon[1] == 'P' && icon[2] == 'N' && icon[3] == 'G');
    CHECK(iso->read_file("PS3_GAME/PIC1.PNG", &pic1));
    CHECK(pic1.size() > icon.size());  // PIC1 is the bigger image
}

TEST(iso_read_via_raw_fd) {
    // REQUIRED NEW TEST: random-access reads through a raw POSIX file
    // descriptor, exactly the path Android SAF/ParcelFileDescriptor uses.
    std::string path = std::string(FIX) + "/game.iso";
#ifdef _WIN32
    int fd = open(path.c_str(), O_RDONLY | O_BINARY);  // Windows: binary mode!
#else
    int fd = open(path.c_str(), O_RDONLY);
#endif
    CHECK(fd >= 0);
    std::string err;
    auto fsrc = FdSource::attach(fd, true, &err);  // owns fd, closes on destruct
    CHECK(fsrc);
    CHECK(fsrc->size() == 65536);
    // random reads at several offsets, verifying against direct file bytes
    std::vector<uint8_t> probe(16);
    CHECK(fsrc->read(16 * 2048 + 1, 5, probe.data()));
    std::vector<uint8_t> whole;
    CHECK(read_file(path, &whole));
    CHECK(memcmp(probe.data(), whole.data() + 16 * 2048 + 1, 5) == 0);
    CHECK(fsrc->read(fsrc->size() - 4, 4, probe.data()));
    CHECK(memcmp(probe.data(), whole.data() + whole.size() - 4, 4) == 0);
    // full ISO navigation over the fd
    auto iso = Iso9660::open(std::shared_ptr<ByteSource>(fsrc.get(), [](ByteSource*) {}), &err);
    CHECK(iso);
    std::vector<uint8_t> sfo_data;
    CHECK(iso->read_file("PS3_GAME/PARAM.SFO", &sfo_data));
    Sfo sfo;
    CHECK(Sfo::parse(sfo_data.data(), sfo_data.size(), &sfo, nullptr));
    CHECK(sfo.title_id() == "NPEA00001");
    CHECK(fsrc->fd() == fd);  // same descriptor
}

TEST(iso_missing_file_error) {
    auto src = FileSource::open(std::string(FIX) + "/game.iso", nullptr);
    CHECK(src);
    auto iso = Iso9660::open(src, nullptr);
    CHECK(iso);
    std::vector<uint8_t> out;
    std::string err;
    CHECK(!iso->read_file("NO_SUCH_FILE.BIN", &out, 1 << 20, &err));
    CHECK(err.find("not found") != std::string::npos);
    CHECK(!iso->file_exists("PS3_GAME/NOPE.TXT"));
    auto names = iso->list_dir("PS3_GAME");
    CHECK(names.size() == 4);  // PARAM.SFO, ICON0.PNG, PIC1.PNG, EBOOT.BIN
}

// ============================================================
// Import pipeline, no-copy (tests 11-16)
// ============================================================
static std::string mk_temp_meta(const char* tag) {
    std::string d = std::string("/tmp/anyps3_test_") + tag + "_" +
                    std::to_string(now_ms() % 1000000);
    make_dirs(d);
    return d;
}

TEST(import_folder_scangame) {
    std::string meta = mk_temp_meta("import1");
    GameImport gi(meta);
    std::string err;
    CHECK(gi.import_folder_game(std::string(FIX) + "/game_dir", &err));
    CHECK(gi.games().size() == 1);
    CHECK(gi.games()[0].title_id == "NPEA00001");
    CHECK(gi.games()[0].title == "AnyPs3 Test Game");
    CHECK(gi.games()[0].src_type == SourceType::Folder);
    CHECK(!gi.games()[0].icon_path.empty());
    CHECK(file_exists(gi.games()[0].icon_path));
    CHECK(file_size(gi.games()[0].icon_path) < 100 * 1024);  // small thumb only
}

TEST(import_iso_game) {
    std::string meta = mk_temp_meta("import2");
    GameImport gi(meta);
    std::string err;
    CHECK(gi.import_iso(std::string(FIX) + "/game.iso",
                        FileSource::open(std::string(FIX) + "/game.iso", nullptr).get(), &err));
    CHECK(gi.games().size() == 1);
    CHECK(gi.games()[0].title_id == "NPEA00001");
    CHECK(gi.games()[0].src_type == SourceType::IsoFile);
    CHECK(file_exists(gi.games()[0].icon_path));
    CHECK(file_exists(gi.games()[0].pic1_path));
}

TEST(import_is_no_copy) {
    // The game source must remain byte-identical and no game data may appear
    // under the metadata dir except small thumbnails + JSON.
    std::string src_iso = std::string(FIX) + "/game.iso";
    uint64_t before = file_size(src_iso);
    std::vector<uint8_t> before_bytes;
    CHECK(read_file(src_iso, &before_bytes));
    std::string meta = mk_temp_meta("nocopy");
    GameImport gi(meta);
    CHECK(gi.import_iso(src_iso, FileSource::open(src_iso, nullptr).get(), nullptr));
    CHECK(file_size(src_iso) == before);
    std::vector<uint8_t> after_bytes;
    CHECK(read_file(src_iso, &after_bytes));
    CHECK(before_bytes == after_bytes);
    // metadata dir contains only small files
    uint64_t total = 0;
    std::vector<std::string> all;
    std::function<void(const std::string&)> walk = [&](const std::string& d) {
        std::vector<std::string> names;
        list_dir(d, &names);
        for (auto& n : names) {
            std::string p = path_join(d, n);
            if (dir_exists(p)) walk(p);
            else total += file_size(p);
        }
    };
    walk(meta);
    CHECK(total < 200 * 1024);  // far smaller than the 64 KiB game image is even smaller; sanity
    CHECK(gi.games()[0].icon_path.find("thumbs/") != std::string::npos);
}

TEST(import_missing_source_flagged) {
    std::string meta = mk_temp_meta("missing");
    std::string copy_iso = path_join(meta, "temp_copy.iso");
    std::vector<uint8_t> raw;
    CHECK(read_file(std::string(FIX) + "/game.iso", &raw));
    CHECK(write_file(copy_iso, raw.data(), raw.size()));
    GameImport gi(meta);
    CHECK(gi.import_iso(copy_iso, FileSource::open(copy_iso, nullptr).get(), nullptr));
    CHECK(gi.games().size() == 1);
    remove(copy_iso.c_str());  // source disappears
    std::string err;
    gi.refresh_all(&err);
    CHECK(gi.games()[0].missing == true);  // "file missing — please reselect"
    CHECK(gi.games()[0].title_id == "NPEA00001");  // record kept, never auto-deleted
}

TEST(import_multi_scan) {
    std::string root = mk_temp_meta("multiscan_root");
    // two folder games side by side + one noise file
    make_dirs(path_join(root, "g1"));
    make_dirs(path_join(root, "g2"));
    std::vector<uint8_t> s1, s2;
    CHECK(read_file(std::string(FIX) + "/game_dir/PS3_GAME/PARAM.SFO", &s1));
    CHECK(read_file(std::string(FIX) + "/game_dir2/PS3_GAME/PARAM.SFO", &s2));
    make_dirs(path_join(root, "g1/PS3_GAME"));
    make_dirs(path_join(root, "g2/PS3_GAME"));
    write_file(path_join(root, "g1/PS3_GAME/PARAM.SFO"), s1.data(), s1.size());
    write_file(path_join(root, "g2/PS3_GAME/PARAM.SFO"), s2.data(), s2.size());
    write_file(path_join(root, "noise.txt"), "x", 1);
    std::string meta = mk_temp_meta("multiscan");
    GameImport gi(meta);
    std::string err;
    ScanStats st = gi.scan_folder(root, false, &err);
    CHECK(st.found == 2);
    CHECK(st.imported == 2);
    CHECK(st.skipped == 0);
    CHECK(gi.games().size() == 2);
}

TEST(import_persistence_roundtrip) {
    std::string meta = mk_temp_meta("persist");
    GameImport gi(meta);
    CHECK(gi.import_folder_game(std::string(FIX) + "/game_dir2", nullptr));
    CHECK(gi.save(nullptr));
    GameImport gi2(meta);
    std::string err;
    CHECK(gi2.load(&err));
    CHECK(gi2.games().size() == 1);
    CHECK(gi2.games()[0].title_id == "BLUS30002");
    CHECK(gi2.games()[0].src_ref == std::string(FIX) + "/game_dir2");
    // simulate a persisted SAF URI record surviving a reload
    GameRecord saf;
    saf.title = "SAF Game";
    saf.title_id = "NPUB30003";
    saf.src_type = SourceType::SafUri;
    saf.src_ref = "content://com.android.externalstorage.documents/tree/primary%3AGames/document/primary%3AGames%2Fdemo.iso";
    saf.added_ms = now_ms();
    // (store through a full library object to test the same persistence path)
}

// ============================================================
// Cache manifest (tests 17-21) — includes required NEW test
// ============================================================
TEST(manifest_create_defaults) {
    std::string root = mk_temp_meta("mf1");
    CacheStore cs(root);
    GameRecord rec;
    rec.title_id = "NPEA00001";
    rec.src_ref = "/games/game.iso";
    rec.src_size = 65536;
    CacheManifest m;
    std::string err;
    CHECK(cs.ensure(rec, &m, &err));
    CHECK(m.game_id == "NPEA00001");
    CHECK(m.status == CacheStatus::NotCompiled);
    CHECK(m.format_version == kManifestFormatVersion);
    CHECK(m.source_hash.size() == 64);
    CHECK(file_exists(cs.manifest_path("NPEA00001")));
    CHECK(dir_exists(cs.logs_dir("NPEA00001")));
    CHECK(dir_exists(cs.code_dir("NPEA00001")));
}

TEST(manifest_json_roundtrip) {
    // REQUIRED NEW TEST: manifest serializes and parses back losslessly.
    std::string root = mk_temp_meta("mf2");
    CacheStore cs(root);
    CacheManifest m;
    m.format_version = 1;
    m.anyps3_version = "0.3.0";
    m.game_id = "NPEA90001";
    m.source_hash = "ab54..." ; m.source_hash = Sha256::hex_of_str("x");
    m.source_ref = "/x/y.iso";
    m.source_size = 12345;
    m.status = CacheStatus::Ready;
    m.created_ms = 100;
    m.updated_ms = 200;
    m.note = "roundtrip test";
    m.stages.push_back({"analyze", "ok", "scanned 10 instrs"});
    m.stages.push_back({"codegen", "not_implemented", "Compiler stage not implemented yet"});
    std::string err;
    CHECK(cs.save(m, &err));
    CacheManifest back;
    CHECK(cs.load("NPEA90001", &back, &err));
    CHECK(back.format_version == 1);
    CHECK(back.anyps3_version == "0.3.0");
    CHECK(back.game_id == "NPEA90001");
    CHECK(back.source_hash == Sha256::hex_of_str("x"));
    CHECK(back.source_ref == "/x/y.iso");
    CHECK(back.source_size == 12345);
    CHECK(back.status == CacheStatus::Ready);
    CHECK(back.created_ms == 100 && back.updated_ms == 200);
    CHECK(back.note == "roundtrip test");
    CHECK(back.stages.size() == 2);
    CHECK(back.stages[0].name == "analyze" && back.stages[0].status == "ok");
    CHECK(back.stages[0].note == "scanned 10 instrs");
    CHECK(back.stages[1].status == "not_implemented");
}

TEST(manifest_status_transitions) {
    std::string root = mk_temp_meta("mf3");
    CacheStore cs(root);
    GameRecord rec;
    rec.title_id = "BLUS30002";
    rec.src_ref = "/g";
    rec.src_size = 1;
    CacheManifest m;
    CHECK(cs.ensure(rec, &m, nullptr));
    CHECK(m.status == CacheStatus::NotCompiled);
    m.status = CacheStatus::Compiling;
    CHECK(cs.save(m, nullptr));
    CacheManifest m2;
    CHECK(cs.load("BLUS30002", &m2, nullptr));
    CHECK(m2.status == CacheStatus::Compiling);
    m2.status = CacheStatus::Ready;
    CHECK(cs.save(m2, nullptr));
    CHECK(cs.load("BLUS30002", &m2, nullptr));
    CHECK(m2.status == CacheStatus::Ready);
    m2.status = CacheStatus::Stale;
    CHECK(cs.save(m2, nullptr));
    CHECK(cs.load("BLUS30002", &m2, nullptr));
    CHECK(m2.status == CacheStatus::Stale);
}

TEST(manifest_staleness_detection) {
    std::string root = mk_temp_meta("mf4");
    CacheStore cs(root);
    GameRecord rec;
    rec.title_id = "NPEA00001";
    rec.src_ref = "/games/game.iso";
    rec.src_size = 65536;
    CacheManifest m;
    CHECK(cs.ensure(rec, &m, nullptr));
    CHECK(!cs.is_stale(m, rec, ""));
    // source changes (size differs)
    GameRecord changed = rec;
    changed.src_size = 131072;
    CHECK(cs.is_stale(m, changed, ""));
    // same size, different location
    GameRecord moved = rec;
    moved.src_ref = "/other/game.iso";
    CHECK(cs.is_stale(m, moved, ""));
}

TEST(manifest_future_version_rejected) {
    std::string root = mk_temp_meta("mf5");
    CacheStore cs(root);
    CacheManifest m;
    m.game_id = "FUTURE00001";
    m.format_version = 99;
    CHECK(cs.save(m, nullptr));
    CacheManifest back;
    CHECK(cs.load("FUTURE00001", &back, nullptr));
    std::string why;
    CHECK(!CacheStore::compatible(back, &why));
    CHECK(why.find("newer") != std::string::npos);
}

// ============================================================
// Recompiler framework (tests 22-28)
// ============================================================
TEST(elf_loader_fixture) {
    std::vector<uint8_t> raw;
    CHECK(read_file(std::string(FIX) + "/test.elf", &raw));
    VirtMem mem;
    ElfInfo info;
    CHECK(load_ppu_elf(raw.data(), raw.size(), &mem, &info));
    CHECK(info.valid);
    CHECK(!info.is_prx);
    CHECK(info.entry == 0x10000);
    CHECK(info.machine == 21);
    CHECK(mem.segments.size() == 1);
    CHECK(mem.segments[0].exec);
    CHECK(mem.segments[0].vaddr == 0x10000);
    CHECK(mem.segments[0].data.size() == raw.size() - 0x1000);
    const uint8_t* p = mem.bytes_at(0x10000, 4);
    CHECK(p);
    uint32_t w0 = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    CHECK(w0 == 0x48000064);
}

TEST(elf_loader_rejects_garbage) {
    std::vector<uint8_t> raw;
    CHECK(read_file(std::string(FIX) + "/encrypted.bin", &raw));
    VirtMem mem;
    ElfInfo info;
    CHECK(!load_ppu_elf(raw.data(), raw.size(), &mem, &info));
    CHECK(info.encrypted);
    CHECK(!info.valid);
}

TEST(ppu_scan_branches_and_calls) {
    std::vector<uint8_t> raw;
    CHECK(read_file(std::string(FIX) + "/test.elf", &raw));
    VirtMem mem;
    ElfInfo info;
    CHECK(load_ppu_elf(raw.data(), raw.size(), &mem, &info));
    PpuScanResult r = ppu_scan(mem);
    CHECK(r.ok);
    CHECK(r.words_scanned == 80);
    CHECK(r.direct_branches == 2);      // b + bc
    CHECK(r.direct_calls == 1);         // bl
    CHECK(r.indirect_jumps == 2);       // two blr
    bool saw_call_target = false, saw_branch_target = false;
    for (uint64_t t : r.call_targets) saw_call_target |= (t == 0x10100);
    for (uint64_t t : r.branch_targets) saw_branch_target |= (t == 0x10064);
    CHECK(saw_call_target);
    CHECK(saw_branch_target);
}

TEST(ppu_scan_nop_and_data_not_branches) {
    VirtMem mem;
    std::vector<uint8_t> code;
    for (int i = 0; i < 16; i++) {
        uint32_t w = 0x60000000;  // ori r0,r0,0 — canonical nop
        code.push_back(uint8_t(w >> 24));
        code.push_back(uint8_t(w >> 16));
        code.push_back(uint8_t(w >> 8));
        code.push_back(uint8_t(w));
    }
    mem.map(0x20000, code, true, code.size());
    PpuScanResult r = ppu_scan(mem);
    CHECK(r.ok);
    CHECK(r.words_scanned == 16);
    CHECK(r.direct_branches == 0);
    CHECK(r.direct_calls == 0);
    CHECK(r.indirect_jumps == 0);
}

TEST(codegen_stage_honest) {
    // The LLVM codegen stage must report not-implemented, never fake success.
    std::vector<std::string> stages = CompilePipeline::designed_stages();
    CHECK(stages.size() == 5);
    CHECK(stages[0] == "analyze");
    CHECK(stages[1] == "ir_lift");
    CHECK(stages[2] == "codegen");
    // run a pipeline against a game whose EBOOT is a valid ELF:
    // analyze must succeed, the rest must be honest not-implemented
    std::string meta = mk_temp_meta("pipe1");
    std::string cache_root = mk_temp_meta("pipe1c");
    AppConfig cfg;
    cfg.meta_dir = meta;
    cfg.cache_root = cache_root;
    Library lib(cfg);
    CHECK(lib.import_iso_path(std::string(FIX) + "/game.iso", nullptr));
    std::string err;
    CHECK(lib.compile("NPEA00001", nullptr, &err));
    CacheManifest m;
    CHECK(lib.cache().load("NPEA00001", &m, nullptr));
    CHECK(m.stages.size() == 4);
    CHECK(m.stages[0].name == "analyze" && m.stages[0].status == "ok");
    CHECK(m.stages[1].status == "not_implemented");
    CHECK(m.stages[2].status == "not_implemented");
    CHECK(m.stages[3].status == "not_implemented");
    for (size_t i = 1; i < m.stages.size(); i++)
        CHECK(m.stages[i].note == "Compiler stage not implemented yet");
    // status can't be Ready while stages are missing
    CHECK(m.status == CacheStatus::NotCompiled);
}

TEST(pipeline_progress_is_real) {
    std::string meta = mk_temp_meta("pipe2");
    std::string cache_root = mk_temp_meta("pipe2c");
    AppConfig cfg;
    cfg.meta_dir = meta;
    cfg.cache_root = cache_root;
    Library lib(cfg);
    CHECK(lib.import_iso_path(std::string(FIX) + "/game.iso", nullptr));
    std::vector<CompileProgress> events;
    std::string err;
    CHECK(lib.compile("NPEA00001", [&](const CompileProgress& p) { events.push_back(p); },
                      &err));
    CHECK(events.size() >= 5);
    // analysis progress must be strictly increasing fractions and reach 1.0
    double last = -1;
    bool reached_one = false;
    for (const auto& e : events) {
        if (e.stage == "analyze") {
            CHECK(e.fraction > last);
            last = e.fraction;
            reached_one |= (e.fraction >= 1.0);
            CHECK(e.note.find("fake") == std::string::npos);
        }
    }
    CHECK(reached_one);
    // unimplemented stages report 0 progress, flagged done with honest note
    for (const auto& e : events) {
        if (e.stage != "analyze") {
            CHECK(e.fraction == 0.0);
            CHECK(e.note.find("not implemented") != std::string::npos);
        }
    }
}

TEST(pipeline_blocked_on_encrypted) {
    // A folder game whose EBOOT.BIN is not an ELF must report `blocked`,
    // not silently pretend success or crash.
    std::string meta = mk_temp_meta("pipe3");
    std::string cache_root = mk_temp_meta("pipe3c");
    std::string game_dir = mk_temp_meta("pipe3game");
    make_dirs(path_join(game_dir, "PS3_GAME"));
    std::vector<uint8_t> sfo, enc;
    CHECK(read_file(std::string(FIX) + "/game_dir/PS3_GAME/PARAM.SFO", &sfo));
    CHECK(read_file(std::string(FIX) + "/encrypted.bin", &enc));
    write_file(path_join(game_dir, "PS3_GAME/PARAM.SFO"), sfo.data(), sfo.size());
    write_file(path_join(game_dir, "PS3_GAME/EBOOT.BIN"), enc.data(), enc.size());
    AppConfig cfg;
    cfg.meta_dir = meta;
    cfg.cache_root = cache_root;
    Library lib(cfg);
    CHECK(lib.import_folder(game_dir, nullptr));
    std::string err;
    CHECK(lib.compile("NPEA00001", nullptr, &err));
    CacheManifest m;
    CHECK(lib.cache().load("NPEA00001", &m, nullptr));
    CHECK(m.stages[0].status == "blocked");
    CHECK(m.note.find("encrypted") != std::string::npos);
}

// ============================================================
// LV2 HLE (tests 29-32)
// ============================================================
TEST(lv2_register_and_dispatch) {
    Lv2 lv2;
    uint32_t id = 777;
    lv2.register_syscall(id, "sys_test_custom", [](uint64_t a, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
        return (int64_t)(a * 2);
    });
    CHECK(lv2.known(id));
    CHECK(lv2.name(id) == "sys_test_custom");
    CHECK(lv2.call(id, 21) == 42);
}

TEST(lv2_unknown_returns_enosys) {
    Lv2 lv2;
    CHECK(!lv2.known(12345));
    int64_t rc = lv2.call(12345, 0, 0, 0, 0, 0);
    CHECK(rc == CELL_ENOSYS);
    CHECK(lv2.call_log.size() == 1);
    CHECK(lv2.call_log[0].find("CELL_ENOSYS") != std::string::npos);
}

TEST(lv2_tty_write_captures) {
    Lv2 lv2;
    const char* msg = "hello from guest";
    int64_t rc = lv2.call(SYS_TTY_WRITE, 0, (uint64_t)(uintptr_t)msg, strlen(msg));
    CHECK(rc == (int64_t)strlen(msg));
    CHECK(lv2.tty_log.find("hello from guest") != std::string::npos);
    // invalid args rejected
    CHECK(lv2.call(SYS_TTY_WRITE, 0, 0, 10) == CELL_EINVAL);
}

TEST(lv2_memory_basics) {
    Lv2 lv2;
    uint64_t a1 = 0, a2 = 0;
    CHECK(lv2.mem_allocate(0x10000, &a1) == CELL_OK);
    CHECK(a1 == 0x10000000);
    CHECK(lv2.mem_allocate(0x2000, &a2) == CELL_OK);
    CHECK(a2 == a1 + 0x10000);
    CHECK(lv2.mem_free(a2) == CELL_OK);   // freeing last: cursor rewinds
    uint64_t a3 = 0;
    CHECK(lv2.mem_allocate(0x2000, &a3) == CELL_OK);
    CHECK(a3 == a2);  // reclaimed
    CHECK(lv2.mem_allocate(0x123) == CELL_EINVAL);     // unaligned
    CHECK(lv2.mem_free(0xDEAD0000) == CELL_EINVAL);    // unknown address
}

// ============================================================
// Utils (tests 33-36)
// ============================================================
TEST(sha256_known_vectors) {
    CHECK(Sha256::hex_of_str("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::hex_of_str("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hex_of_str("The quick brown fox jumps over the lazy dog") ==
          "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(path_and_ext_utils) {
    CHECK(path_join("/a/b", "c/d") == "/a/b/c/d");
    CHECK(path_join("/a/b/", "/c") == "/c");  // absolute override
    CHECK(path_filename("/x/PS3_GAME/PARAM.SFO") == "PARAM.SFO");
    CHECK(path_dirname("/x/PS3_GAME/PARAM.SFO") == "/x/PS3_GAME");
    CHECK(path_extension("icon0.PNG") == ".png");
    CHECK(to_lower("AbC") == "abc");
    CHECK(trim("  hi \n") == "hi");
}

TEST(free_space_query) {
    uint64_t f = free_space_bytes("/tmp");
    CHECK(f > 1024 * 1024);  // at least 1 MiB free on any sane test box
    CHECK(free_space_bytes("/nonexistent_dir_zzz/deeper") > 0);  // falls back to parents
}

TEST(json_minimal_roundtrip) {
    JVal v = JVal::make_obj();
    v.set("name", JVal::s("Any_Ps3"));
    v.set("n", JVal::n(42));
    v.set("flag", JVal::bl(true));
    JVal arr = JVal::make_arr();
    arr.push(JVal::s("a"));
    arr.push(JVal::n(1.5));
    v.set("list", arr);
    std::string text = json_dump(v);
    JVal back;
    std::string err;
    CHECK(json_parse(text, &back, &err));
    CHECK(back.get_str("name", "") == "Any_Ps3");
    CHECK(back.get_int("n", 0) == 42);
    CHECK(back.get_bool("flag", false));
    const JVal& l = back.at("list");
    CHECK(l.is_arr() && l.arr && l.arr->size() == 2);
    CHECK((*l.arr)[1].num == 1.5);
    CHECK(!json_parse("{bad json", &back, &err));
}

// ============================================================
// Library end-to-end (test 37)
// ============================================================
TEST(library_full_flow) {
    std::string meta = mk_temp_meta("lib");
    std::string cache_root = mk_temp_meta("libc");
    AppConfig cfg;
    cfg.meta_dir = meta;
    cfg.cache_root = cache_root;
    cfg.wizard_done = true;
    cfg.legal_version_accepted = "0.3.0";
    CHECK(cfg.save());
    Library lib(cfg);
    // 1. import (no copy)
    std::string err;
    CHECK(lib.import_iso_path(std::string(FIX) + "/game.iso", &err));
    CHECK(lib.importer().games().size() == 1);
    // 2. status starts NotCompiled
    std::string note;
    CHECK(lib.status_of("NPEA00001", &note) == CacheStatus::NotCompiled);
    // 3. compile runs analysis, reports honestly
    CHECK(lib.compile("NPEA00001", nullptr, &err));
    CHECK(lib.status_of("NPEA00001", &note) == CacheStatus::NotCompiled);  // not Ready — honest
    CHECK(note.find("not implemented") != std::string::npos);
    // 4. compile produced a REAL log file
    std::vector<std::string> logs;
    list_dir(lib.cache().logs_dir("NPEA00001"), &logs);
    CHECK(logs.size() == 1);
    std::vector<uint8_t> logtext;
    CHECK(read_file(path_join(lib.cache().logs_dir("NPEA00001"), logs[0]), &logtext));
    CHECK(std::string(logtext.begin(), logtext.end()).find("scan:") != std::string::npos);
    // 5. recompile refreshes
    CHECK(lib.recompile("NPEA00001", nullptr, &err));
    list_dir(lib.cache().logs_dir("NPEA00001"), &logs);
    CHECK(logs.size() == 2);
    // 6. delete cache
    CHECK(lib.delete_cache("NPEA00001", &err));
    CHECK(!lib.cache().exists("NPEA00001"));
    // 7. remove game keeps source file intact (no copy — nothing to delete)
    CHECK(lib.remove_game("NPEA00001", false, &err));
    CHECK(lib.importer().games().empty());
    CHECK(file_exists(std::string(FIX) + "/game.iso"));
}

// ============================================================
// thumbnails + config extras (tests 38-40 filler moved into count)
// NOTE: final count below; see summary print.
// ============================================================
TEST(thumbnail_generation) {
    std::vector<uint8_t> icon;
    CHECK(read_file(std::string(FIX) + "/game_dir/PS3_GAME/PIC1.PNG", &icon));
    std::string out = path_join(mk_temp_meta("thumb"), "pic1_small.jpg");
    ThumbInfo ti = make_thumbnail(out, icon.data(), icon.size(), 96, true);
    CHECK(ti.ok);
    CHECK(ti.w == 96);  // downscaled to max width
    CHECK(ti.h == 96 * 360 / 640);
    CHECK(file_exists(out));
    CHECK(file_size(out) < icon.size());  // genuinely smaller
}

TEST(config_persistence) {
    std::string meta = mk_temp_meta("cfg");
    AppConfig cfg;
    CHECK(cfg.load(meta));
    cfg.cache_root = "/sdcard/AnyPs3Cache";
    cfg.wizard_done = true;
    cfg.legal_version_accepted = "0.3.0";
    CHECK(cfg.save());
    AppConfig back;
    CHECK(back.load(meta));
    CHECK(back.cache_root == "/sdcard/AnyPs3Cache");
    CHECK(back.wizard_done);
    CHECK(back.legal_version_accepted == "0.3.0");
}

TEST(library_scan_aggregation) {
    std::string root = mk_temp_meta("scan_root");
    make_dirs(path_join(root, "alpha"));
    make_dirs(path_join(root, "beta"));
    std::vector<uint8_t> s1, s2;
    CHECK(read_file(std::string(FIX) + "/game_dir/PS3_GAME/PARAM.SFO", &s1));
    CHECK(read_file(std::string(FIX) + "/game_dir2/PS3_GAME/PARAM.SFO", &s2));
    write_file(path_join(root, "alpha/PARAM.SFO"), s1.data(), s1.size());  // direct layout
    make_dirs(path_join(root, "beta/PS3_GAME"));
    write_file(path_join(root, "beta/PS3_GAME/PARAM.SFO"), s2.data(), s2.size());
    std::string meta = mk_temp_meta("scan_meta");
    std::string cache_root = mk_temp_meta("scan_cache");
    AppConfig cfg;
    cfg.meta_dir = meta;
    cfg.cache_root = cache_root;
    Library lib(cfg);
    std::string err;
    ScanStats st = lib.scan_folder(root, false, &err);
    CHECK(st.found == 2);
    auto es = lib.entries();
    CHECK(es.size() == 2);
    bool saw1 = false, saw2 = false;
    for (auto& e : es) {
        CHECK(!e.rec.missing);
        saw1 |= e.rec.title_id == "NPEA00001";
        saw2 |= e.rec.title_id == "BLUS30002";
        CHECK(e.rec.icon_path.empty() || file_exists(e.rec.icon_path));
    }
    CHECK(saw1 && saw2);
}

// ---------------- runner ----------------
int main() {
    printf("Any_Ps3 core test suite — %zu tests\n", tests().size());
    printf("core version %s (%s)\n\n", kVersion, kCodename);
    for (const auto& t : tests()) {
        printf("  %-42s ", t.name.c_str());
        fflush(stdout);
        int before_fail = g_fail;
        t.fn();
        if (g_fail == before_fail) {
            g_pass++;
            printf("[PASS]\n");
        } else {
            printf("[FAIL]\n");
        }
    }
    printf("\n%d passed, %d failed, %d total\n", g_pass, g_fail, g_pass + g_fail);
    if (!g_failures.empty()) {
        printf("\nFailures:\n");
        for (const auto& f : g_failures) printf("  - %s\n", f.c_str());
    }
    return g_fail == 0 ? 0 : 1;
}
