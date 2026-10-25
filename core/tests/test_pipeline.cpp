// Tests: fallback interpreter + LV2 HLE (the "hello world" execution path),
// cache manifest (REQUIRED new test area #3), and the honest AOT pipeline.
#include "harness.h"
#include "testdata.h"
#include "any_ps3/interp.h"
#include "any_ps3/manifest.h"
#include "any_ps3/pipeline.h"
#include "any_ps3/source.h"
#include "any_ps3/sha256.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace any_ps3 {

using testdata::put32be;

// ---- interpreter / LV2 ----

static void interp_hello_world_via_hle(TestState& st) {
    auto f = testdata::make_hello_elf("Hello from Any_Ps3!");
    ElfImage img;
    try { img = elf_parse(f.data(), f.size()); }
    catch (const Error& e) { st.fail(e.what(), __FILE__, __LINE__); return; }
    PpuMemory mem;
    AP_CHECK(mem.load(img, f.data(), f.size()));

    Lv2Hle hle;
    hle.mem = &mem;
    PpuInterpreter interp(mem, hle);
    PpuInterpreter::RunStats stats;
    PpuInterpreter::Exit rc = interp.run(0x10000, 10000, &stats);

    AP_CHECK_EQ(rc, PpuInterpreter::Exit::Exited);
    AP_CHECK_EQ(hle.captured_stdout, std::string("Hello from Any_Ps3!"));
    AP_CHECK(stats.steps > 0);
    AP_CHECK(stats.note.empty() || stats.note.find("not implemented") == std::string::npos);
}

static void interp_illegal_insn_honest(TestState& st) {
    // Build an ELF whose first instruction is an unimplemented one (dcbz, op 31/1014).
    auto f = testdata::make_hello_elf();
    put32be(f, 0x1000, (31u << 26) | (3u << 21) | (4u << 16) | (1014u << 1));
    ElfImage img;
    try { img = elf_parse(f.data(), f.size()); }
    catch (const Error& e) { st.fail(e.what(), __FILE__, __LINE__); return; }
    PpuMemory mem;
    AP_CHECK(mem.load(img, f.data(), f.size()));
    Lv2Hle hle;
    hle.mem = &mem;
    PpuInterpreter interp(mem, hle);
    PpuInterpreter::RunStats stats;
    PpuInterpreter::Exit rc = interp.run(0x10000, 1000, &stats);
    AP_CHECK_EQ(rc, PpuInterpreter::Exit::IllegalInstruction);
    AP_CHECK(stats.note.find("not implemented in fallback interpreter") != std::string::npos);
}

static void lv2_known_and_unknown(TestState& st) {
    AP_CHECK(lv2_syscall_name(1) != nullptr);
    AP_CHECK_EQ(std::string(lv2_syscall_name(1)), std::string("sys_process_exit"));
    AP_CHECK(lv2_syscall_name(988) != nullptr);
    AP_CHECK_EQ(std::string(lv2_syscall_name(988)), std::string("sys_tty_write"));
    AP_CHECK(lv2_syscall_name(403) == nullptr); // not yet in our verified table
    AP_CHECK_EQ(lv2_syscall_display(403), std::string("unknown syscall 0x193"));
    AP_CHECK_EQ(lv2_syscall_display(988), std::string("sys_tty_write"));
}

static void interp_unhandled_syscall_stops(TestState& st) {
    // sys_tty_read (987) is NOT implemented in our HLE -> honest stop.
    auto f = testdata::make_hello_elf();
    put32be(f, 0x1000, testdata::ppu_li(11, 987)); // li r11, 987
    put32be(f, 0x1004, 0x44000002);                // sc
    ElfImage img;
    try { img = elf_parse(f.data(), f.size()); }
    catch (const Error& e) { st.fail(e.what(), __FILE__, __LINE__); return; }
    PpuMemory mem;
    AP_CHECK(mem.load(img, f.data(), f.size()));
    Lv2Hle hle;
    hle.mem = &mem;
    PpuInterpreter interp(mem, hle);
    PpuInterpreter::RunStats stats;
    PpuInterpreter::Exit rc = interp.run(0x10000, 1000, &stats);
    AP_CHECK_EQ(rc, PpuInterpreter::Exit::SyscallUnhandled);
    AP_CHECK(stats.note.find("sys_tty_read") == std::string::npos); // 987 is unknown to us
    AP_CHECK(stats.note.find("not implemented in HLE") != std::string::npos);
}

// ---- manifest ----

static Manifest sample_manifest() {
    Manifest m;
    m.game_id = "NPUP00001";
    m.title = "Any Ps3 Demo";
    m.source_kind = "iso";
    m.source_hash = Sha256::hex_of("source", 6);
    m.source_size = 12345678;
    m.engine_version = kEngineVersion;
    m.created_utc = "2026-10-23T00:00:00Z";
    m.updated_utc = "2026-10-23T00:00:00Z";
    m.status = "analyzed";
    m.set_stage("inspect", "done", "sha256 " + m.source_hash);
    m.set_stage("analyze", "done", "functions=1 decoded=11");
    m.set_stage("lift", "not_implemented", "Compiler stage not implemented yet");
    return m;
}

static void manifest_json_roundtrip(TestState& st) {
    Manifest m = sample_manifest();
    std::string text = m.to_json();
    Manifest back;
    std::string err;
    AP_CHECK(Manifest::from_json(text, back, err));
    if (!err.empty()) st.fail(err.c_str(), __FILE__, __LINE__);
    AP_CHECK_EQ(back.game_id, m.game_id);
    AP_CHECK_EQ(back.title, m.title);
    AP_CHECK_EQ(back.source_kind, m.source_kind);
    AP_CHECK_EQ(back.source_hash, m.source_hash);
    AP_CHECK_EQ(back.source_size, m.source_size);
    AP_CHECK_EQ(back.status, m.status);
    AP_CHECK_EQ(back.stages.size(), (size_t)3);
    AP_CHECK_EQ(back.stages[2].state, std::string("not_implemented"));
    // strictness: bad format rejected, missing game_id rejected
    AP_CHECK(!Manifest::from_json("{\"format\":0,\"game_id\":\"X\"}", back, err));
    AP_CHECK(!Manifest::from_json("{\"format\":1}", back, err));
    AP_CHECK(!Manifest::from_json("{\"format\":1,\"game_id\":\"X\",\"bad\":1.5}", back, err));
}

static void manifest_fd_roundtrip(TestState& st) {
    std::string path = "/tmp/anyps3_manifest.json";
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    AP_CHECK(fd >= 0);
    {
        Manifest m = sample_manifest();
        std::string err;
        AP_CHECK(manifest_write_fd(fd, m, err));
        if (!err.empty()) st.fail(err.c_str(), __FILE__, __LINE__);
        Manifest back;
        AP_CHECK(manifest_read_fd(fd, back, err));
        AP_CHECK_EQ(back.game_id, m.game_id);
        AP_CHECK_EQ(back.source_hash, m.source_hash);
        AP_CHECK(!back.updated_utc.empty()); // write stamps updated_utc
        AP_CHECK(!back.created_utc.empty());
    }
    ::close(fd);
    ::unlink(path.c_str());
}

static void manifest_outdated_logic(TestState& st) {
    Manifest m = sample_manifest();
    AP_CHECK(!manifest_is_outdated(m, m.source_hash));      // same source
    AP_CHECK(manifest_is_outdated(m, Sha256::hex_of("other", 5))); // changed
    // empty hash never marks outdated
    Manifest m2 = m;
    m2.source_hash.clear();
    AP_CHECK(!manifest_is_outdated(m2, Sha256::hex_of("x", 1)));
    // status "error" is not "outdated" territory
    Manifest m3 = m;
    m3.status = "error";
    AP_CHECK(!manifest_is_outdated(m3, Sha256::hex_of("other", 5)));
}

// ---- pipeline ----

static void pipeline_registry_honesty(TestState& st) {
    const auto& stages = pipeline_stages();
    AP_CHECK_EQ(stages.size(), (size_t)5);
    AP_CHECK(std::string(stages[0].id) == "inspect" && stages[0].implemented);
    AP_CHECK(std::string(stages[1].id) == "analyze" && stages[1].implemented);
    AP_CHECK(std::string(stages[2].id) == "lift" && !stages[2].implemented);
    AP_CHECK(std::string(stages[3].id) == "codegen" && !stages[3].implemented);
    AP_CHECK(std::string(stages[4].id) == "link" && !stages[4].implemented);
}

static std::unique_ptr<IsoFileSource> open_test_iso_fd(int& fd) {
    testdata::IsoBuilder b;
    auto sfo = testdata::make_sfo({{"TITLE", "Any Ps3 Demo"},
                                   {"TITLE_ID", "NPUP00001"},
                                   {"CATEGORY", "GD"}});
    b.add_file("PARAM.SFO", sfo);
    b.add_file("ICON0.PNG", std::vector<uint8_t>(16, 1));
    b.add_file("EBOOT.BIN", testdata::make_hello_elf());
    auto img = b.build();
    std::string path = "/tmp/anyps3_pipe.iso";
    write_file_path(path, img.data(), img.size());
    fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return nullptr;
    std::string err;
    return IsoFileSource::open(std::make_unique<FdSource>(fd), err);
}

static void pipeline_compile_stops_at_lift(TestState& st) {
    int fd = -1;
    auto src = open_test_iso_fd(fd);
    AP_CHECK(src != nullptr);
    if (!src) return;
    int progress_lines = 0;
    auto result = pipeline_compile(*src, "", "iso", [&](const PipelineProgress& p) {
        progress_lines++;
    });
    ::close(fd);

    // inspect + analyze really ran
    AP_CHECK(!result.manifest.source_hash.empty());
    AP_CHECK(result.manifest.source_size > 0);
    AP_CHECK_EQ(result.manifest.game_id, std::string("NPUP00001"));
    AP_CHECK_EQ(result.manifest.title, std::string("Any Ps3 Demo"));
    AP_CHECK(result.manifest.stage("inspect") != nullptr);
    AP_CHECK_EQ(result.manifest.stage("inspect")->state, std::string("done"));
    AP_CHECK(result.manifest.stage("analyze") != nullptr);
    AP_CHECK_EQ(result.manifest.stage("analyze")->state, std::string("done"));
    // honest stop at lift
    AP_CHECK(!result.completed);
    AP_CHECK_EQ(result.stopped_at, std::string("lift"));
    AP_CHECK(result.reason.find("not implemented") != std::string::npos);
    AP_CHECK_EQ(result.manifest.status, std::string("analyzed"));
    AP_CHECK(progress_lines >= 4); // real log lines were produced
}

static void pipeline_outdated_detection(TestState& st) {
    int fd = -1;
    auto src = open_test_iso_fd(fd);
    AP_CHECK(src != nullptr);
    if (!src) return;
    auto result = pipeline_compile(*src, "", "iso", nullptr);
    ::close(fd);

    Manifest m = result.manifest;
    // recompile from identical source => up to date
    int fd2 = -1;
    auto src2 = open_test_iso_fd(fd2);
    AP_CHECK(src2 != nullptr);
    if (src2) {
        AP_CHECK_EQ(pipeline_check_outdated(*src2, m, nullptr), 0);
        ::close(fd2);
    }
    // tampered hash => outdated
    Manifest m2 = m;
    m2.source_hash = Sha256::hex_of("tampered", 8);
    int fd3 = -1;
    auto src3 = open_test_iso_fd(fd3);
    AP_CHECK(src3 != nullptr);
    if (src3) {
        AP_CHECK_EQ(pipeline_check_outdated(*src3, m2, nullptr), 1);
        ::close(fd3);
    }
    ::unlink("/tmp/anyps3_pipe.iso");
}

static void pipeline_encrypted_eboot_honest(TestState& st) {
    // ISO whose EBOOT.BIN is an encrypted SELF -> analyze must fail honestly.
    testdata::IsoBuilder b;
    b.add_file("PARAM.SFO", testdata::make_sfo({{"TITLE", "Retail"}, {"TITLE_ID", "BLUS30000"}}));
    b.add_file("EBOOT.BIN", {'S', 'E', 'L', 'F', 1, 2, 3, 4});
    auto img = b.build();
    std::string path = "/tmp/anyps3_self.iso";
    write_file_path(path, img.data(), img.size());
    int fd = ::open(path.c_str(), O_RDONLY);
    AP_CHECK(fd >= 0);
    if (fd >= 0) {
        std::string err;
        auto src = IsoFileSource::open(std::make_unique<FdSource>(fd), err);
        AP_CHECK(src != nullptr);
        if (src) {
            auto result = pipeline_compile(*src, "", "iso", nullptr);
            AP_CHECK(!result.completed);
            AP_CHECK_EQ(result.stopped_at, std::string("analyze"));
            AP_CHECK(result.reason.find("encrypted") != std::string::npos);
            AP_CHECK(result.reason.find("keys") != std::string::npos);
            AP_CHECK_EQ(result.manifest.status, std::string("error"));
        }
        ::close(fd);
    }
    ::unlink(path.c_str());
}

static const TestCase kTests[] = {
    AP_TEST(interp_hello_world_via_hle),
    AP_TEST(interp_illegal_insn_honest),
    AP_TEST(lv2_known_and_unknown),
    AP_TEST(interp_unhandled_syscall_stops),
    AP_TEST(manifest_json_roundtrip),
    AP_TEST(manifest_fd_roundtrip),
    AP_TEST(manifest_outdated_logic),
    AP_TEST(pipeline_registry_honesty),
    AP_TEST(pipeline_compile_stops_at_lift),
    AP_TEST(pipeline_outdated_detection),
    AP_TEST(pipeline_encrypted_eboot_honest),
};

const TestCase* pipeline_tests() { return kTests; }
size_t pipeline_tests_count() { return sizeof kTests / sizeof kTests[0]; }

} // namespace any_ps3
