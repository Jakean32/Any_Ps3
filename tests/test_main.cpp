#include "framework.h"

#include <cstdio>
#include <string>

#include "anyps3/log.h"

int main(int argc, char** argv) {
    using namespace anyps3test;
    std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0;
    // Keep test output clean: route Any_Ps3 logs to a sink during tests.
    anyps3::SetLogSink([](anyps3::LogLevel, const std::string& msg) {
        static std::string last;
        if (msg != last) {
            std::fprintf(stderr, "    [log] %s\n", msg.c_str());
            last = msg;
        }
    });

    for (const auto& tc : Registry()) {
        if (!filter.empty() && std::string(tc.name).find(filter) == std::string::npos) continue;
        g_current = tc.name;
        int before = g_failures;
        std::printf("[ RUN ] %s\n", tc.name);
        tc.fn();
        if (g_failures == before) std::printf("[ OK  ] %s\n", tc.name);
        else std::printf("[FAIL ] %s\n", tc.name);
        ran++;
    }
    std::printf("\n%d test(s), %d failure(s)\n", ran, g_failures);
    return g_failures == 0 ? 0 : 1;
}
