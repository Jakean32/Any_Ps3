// TASK-4 tests: the log-level setting genuinely changes the output.
// Levels: 0 = Info+ (default), 1 = Warn+, 2 = everything incl. debug,
// 3 = errors only. Also proves AP_DEBUG is compiled (runtime-filtered) and
// that the level persists through the global config file.
#include "framework.h"

#include <string>
#include <vector>

#include "anyps3/config.h"
#include "anyps3/log.h"
#include "anyps3/platform.h"

using namespace anyps3;

namespace {
struct Captured {
    std::vector<LogLevel> levels;
    std::vector<std::string> msgs;
};
}  // namespace

TEST(logging_levels_really_filter) {
    Captured cap;
    SetLogSink([&cap](LogLevel lv, const std::string& msg) {
        cap.levels.push_back(lv);
        cap.msgs.push_back(msg);
    });

    auto counts = [&cap]() {
        struct { int dbg = 0, info = 0, warn = 0, err = 0; } c;
        for (auto lv : cap.levels) {
            if (lv == LogLevel::Debug) c.dbg++;
            else if (lv == LogLevel::Info) c.info++;
            else if (lv == LogLevel::Warn) c.warn++;
            else c.err++;
        }
        return c;
    };

    // Level 0 (default, Info+): debug suppressed, the rest pass.
    SetLogLevel(0);
    cap.levels.clear(); cap.msgs.clear();
    AP_DEBUG("debug line one");
    AP_INFO("info line");
    AP_WARN("warn line");
    AP_ERROR("error line");
    {
        auto c = counts();
        CHECK_EQ(c.dbg, 0);
        CHECK_EQ(c.info, 1);
        CHECK_EQ(c.warn, 1);
        CHECK_EQ(c.err, 1);
    }

    // Level 1 (Warn+): only warnings and errors.
    SetLogLevel(1);
    cap.levels.clear(); cap.msgs.clear();
    AP_DEBUG("debug line two");
    AP_INFO("info line");
    AP_WARN("warn line");
    AP_ERROR("error line");
    {
        auto c = counts();
        CHECK_EQ(c.dbg, 0);
        CHECK_EQ(c.info, 0);
        CHECK_EQ(c.warn, 1);
        CHECK_EQ(c.err, 1);
    }

    // Level 2 (Debug/everything): all four through — AP_DEBUG is compiled in.
    SetLogLevel(2);
    cap.levels.clear(); cap.msgs.clear();
    AP_DEBUG("debug line three");
    AP_INFO("info line");
    AP_WARN("warn line");
    AP_ERROR("error line");
    {
        auto c = counts();
        CHECK_EQ(c.dbg, 1);
        CHECK_EQ(c.info, 1);
        CHECK_EQ(c.warn, 1);
        CHECK_EQ(c.err, 1);
        // the debug line actually reached the sink with its text
        CHECK_EQ(cap.msgs.size(), 4u);
        CHECK_STR_EQ(cap.msgs[0], "debug line three");
    }

    // Level 3 (errors only).
    SetLogLevel(3);
    cap.levels.clear(); cap.msgs.clear();
    AP_DEBUG("debug line four");
    AP_INFO("info line");
    AP_WARN("warn line");
    AP_ERROR("error line");
    {
        auto c = counts();
        CHECK_EQ(c.dbg, 0);
        CHECK_EQ(c.info, 0);
        CHECK_EQ(c.warn, 0);
        CHECK_EQ(c.err, 1);
        CHECK_STR_EQ(cap.msgs[0], "error line");
    }

    // Restore defaults so later tests keep a predictable log state.
    SetLogLevel(0);
    SetLogSink(nullptr);
}

TEST(logging_level_clamps) {
    SetLogLevel(-5);
    CHECK_EQ(GetLogLevel(), 0);
    SetLogLevel(99);
    CHECK_EQ(GetLogLevel(), 3);
    SetLogLevel(0);
}

TEST(logging_level_persists_in_config) {
    std::string dir = "/tmp/anyps3_lib_logcfg";
    std::string cmd = std::string("rm -rf '") + dir + "'";
    if (std::system(cmd.c_str()) != 0) {
    }
    platform::MakeDirs(dir);
    AppConfig app{dir};
    app.SetLogLevel(3);
    app.global.SaveToFile(dir + "/anyps3.ini");
    AppConfig loaded{dir};
    loaded.global.LoadFromFile(dir + "/anyps3.ini");
    CHECK_EQ(loaded.LogLevel(), 3);
    // all four values round-trip
    for (int v = 0; v <= 3; v++) {
        app.SetLogLevel(v);
        app.global.SaveToFile(dir + "/anyps3.ini");
        AppConfig again{dir};
        again.global.LoadFromFile(dir + "/anyps3.ini");
        CHECK_EQ(again.LogLevel(), v);
    }
    cmd = std::string("rm -rf '") + dir + "'";
    if (std::system(cmd.c_str()) != 0) {
    }
}
