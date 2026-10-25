#include "anyps3/log.h"

#include <atomic>
#include <cstdio>

namespace anyps3 {

static LogSink g_sink;
static std::atomic<int> g_level{0};
// 0 = Info+ (default), 1 = Warn+ only, 2 = everything incl. debug, 3 = errors only

void SetLogSink(LogSink sink) { g_sink = std::move(sink); }

void SetLogLevel(int level) {
    g_level.store(level < 0 ? 0 : (level > 3 ? 3 : level), std::memory_order_relaxed);
}

int GetLogLevel() { return g_level.load(std::memory_order_relaxed); }

void LogWrite(LogLevel level, const std::string& msg) {
    const int lv = g_level.load(std::memory_order_relaxed);
    if (lv == 0 && level == LogLevel::Debug) return;  // Info+
    if (lv == 1 && (level == LogLevel::Debug || level == LogLevel::Info)) return;  // Warn+
    if (lv == 3 && level != LogLevel::Error) return;  // errors only
    if (g_sink) {
        g_sink(level, msg);
        return;
    }
    const char* tag = "INFO";
    switch (level) {
        case LogLevel::Debug: tag = "DBG "; break;
        case LogLevel::Info:  tag = "INFO"; break;
        case LogLevel::Warn:  tag = "WARN"; break;
        case LogLevel::Error: tag = "ERR "; break;
    }
    std::fprintf(stderr, "[Any_Ps3][%s] %s\n", tag, msg.c_str());
}

}  // namespace anyps3
