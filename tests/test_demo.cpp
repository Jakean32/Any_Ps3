// Any_Ps3 — built-in demo entry tests: install through the real import
// pipeline, then boot + run through the Emulator orchestration exactly like
// the app shells do.
#include "framework.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "anyps3/config.h"
#include "anyps3/cpu/ppu/decode.h"
#include "anyps3/demo/hello.h"
#include "anyps3/loader/elf.h"
#include "anyps3/loader/import.h"
#include "anyps3/log.h"
#include "anyps3/platform.h"
#include "anyps3/runtime/emulator.h"

#include "fixtures/ppu_fixtures.h"

using namespace anyps3;

namespace {

std::string TempDir(const char* tag) {
    static int counter = 0;
    std::string d = std::string("/tmp/anyps3_demo_test_") + tag + "_" +
                    std::to_string(++counter);
    platform::MakeDirs(d);
    return d;
}
std::string WriteSampleElf(const std::string& dir) {
    std::string path = dir + "/hello.elf";
    FILE* f = std::fopen(path.c_str(), "wb");
    CHECK_TRUE(f != nullptr);
    if (!f) return "";
    std::fwrite(khello_elf, 1, khello_elf_size, f);
    std::fclose(f);
    return path;
}

}  // namespace

TEST(demo_install_creates_library_entry) {
    std::string cfg = TempDir("install");
    std::string elf = WriteSampleElf(cfg);
    GameInfo info;
    std::string err;
    CHECK_TRUE(anyps3::demo::InstallHelloDemo(cfg, elf, &info, &err));
    CHECK_STR_EQ(err, "");
    CHECK_STR_EQ(info.title_id, anyps3::demo::kDemoTitleId);
    CHECK_TRUE(info.title.find("Hello World") != std::string::npos);
    CHECK_STR_EQ(info.content_root, "");

    // The entry must be visible through the normal library listing.
    anyps3::GameLibrary lib(cfg);
    std::vector<anyps3::LibraryEntry> entries;
    CHECK_TRUE(lib.List(&entries));
    CHECK_EQ(entries.size(), 1u);
    CHECK_STR_EQ(entries[0].info.title_id, "DEMO00001");

    // Idempotent re-install.
    CHECK_TRUE(anyps3::demo::InstallHelloDemo(cfg, elf, nullptr, &err));
    CHECK_TRUE(lib.List(&entries));
    CHECK_EQ(entries.size(), 1u);
}

TEST(demo_install_missing_sample_fails_cleanly) {
    std::string cfg = TempDir("missing");
    std::string err;
    CHECK_TRUE(!anyps3::demo::InstallHelloDemo(cfg, cfg + "/no_such.elf", nullptr, &err));
    CHECK_TRUE(!err.empty());
}

TEST(demo_boots_and_runs_through_emulator) {
    std::string cfg = TempDir("run");
    std::string elf = WriteSampleElf(cfg);
    std::string err;
    CHECK_TRUE(anyps3::demo::InstallHelloDemo(cfg, elf, nullptr, &err));

    anyps3::AppConfig app{cfg};
    CHECK_TRUE(app.global.LoadFromFile(cfg + "/anyps3.ini") || true);
    app.AcceptFirstRun();

    // Capture the boot log the same way the UIs do: cb.log for boot notes and
    // the global log sink for core/runtime/TTY lines.
    std::vector<std::string> lines;
    std::vector<std::string> sink_lines;
    anyps3::SetLogSink([&](anyps3::LogLevel, const std::string& msg) {
        sink_lines.push_back(msg);
    });
    anyps3::Emulator emu(app);
    anyps3::EmulatorCallbacks cb;
    cb.log = [&](const std::string& l) { lines.push_back(l); };
    cb.progress = [](int, const std::string&) {};

    anyps3::GameSettings gs;  // defaults; Emulator forces interpreter where AOT is invalid
    anyps3::BootResult r = emu.Boot(anyps3::demo::kDemoTitleId, gs, cb);
    CHECK_TRUE(r.ok);
    if (!r.ok) {
        std::fprintf(stderr, "  boot failed: %s\n", r.error.c_str());
        return;
    }
    int code = emu.Run();
    CHECK_EQ(code, 42);
    anyps3::SetLogSink(nullptr);  // restore default sink

    std::string log;
    for (const auto& l : lines) log += l + "\n";
    std::string sink_log;
    for (const auto& l : sink_lines) sink_log += l + "\n";
    CHECK_TRUE(sink_log.find("Hello from Any_Ps3!") != std::string::npos);
    CHECK_TRUE(log.find("plain ELF") != std::string::npos);
}
