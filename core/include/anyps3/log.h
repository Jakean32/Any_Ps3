// Any_Ps3 — logging. A UI can attach a sink; default sink writes to stderr.
#pragma once

#include <functional>
#include <string>

namespace anyps3 {

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

using LogSink = std::function<void(LogLevel, const std::string&)>;

void SetLogSink(LogSink sink);
void LogWrite(LogLevel level, const std::string& msg);

// Global verbosity filter (real setting, consumed by the core log filter):
//   0 = Info+ (default: info, warnings, errors)
//   1 = Warnings + errors only
//   2 = Everything incl. debug lines
//   3 = Errors only
void SetLogLevel(int level);
int GetLogLevel();

// Dependency-free line formatter: `const char*`/`std::string` stay strings,
// everything else goes through std::to_string. Used by the AP_* macros.
namespace detail {

inline void AppendOne(std::string& out, const char* v) { out += v; }
inline void AppendOne(std::string& out, const std::string& v) { out += v; }
inline void AppendOne(std::string& out, char v) { out += v; }
template <typename T>
inline void AppendOne(std::string& out, const T& v) {
    out += std::to_string(v);
}
inline void AppendAll(std::string&) {}
template <typename T, typename... Rest>
inline void AppendAll(std::string& out, const T& first, const Rest&... rest) {
    AppendOne(out, first);
    AppendAll(out, rest...);
}

}  // namespace detail

template <typename... Args>
inline std::string FormatLog(const Args&... args) {
    std::string out;
    detail::AppendAll(out, args...);
    return out;
}

}  // namespace anyps3

#define AP_LOG(lvl, ...) ::anyps3::LogWrite((lvl), ::anyps3::FormatLog(__VA_ARGS__))
#define AP_INFO(...)     AP_LOG(::anyps3::LogLevel::Info, __VA_ARGS__)
#define AP_WARN(...)     AP_LOG(::anyps3::LogLevel::Warn, __VA_ARGS__)
#define AP_ERROR(...)    AP_LOG(::anyps3::LogLevel::Error, __VA_ARGS__)
// Debug lines are always compiled and filtered at RUNTIME by SetLogLevel()
// (level 2 = "Debug" shows them; the setting genuinely changes the output).
#define AP_DEBUG(...)    AP_LOG(::anyps3::LogLevel::Debug, __VA_ARGS__)
