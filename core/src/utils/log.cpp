#include "anyps3/log.h"

#include <cstdio>

namespace anyps3 {

static LogSink g_sink;

void SetLogSink(LogSink sink) { g_sink = std::move(sink); }

void LogWrite(LogLevel level, const std::string& msg) {
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
