// Any_Ps3 Windows shell — diagnostics export (TASK 4).
// Gathers: app version, OS, CPU, GPU (DXGI), Vulkan runtime info, cache
// folder, per-game status and the last 500 log lines; writes a .txt next to
// the exe. Nothing here phones home — the file is only created when the user
// clicks "Export log" / "Export crash log".
#pragma once

#include <string>

namespace anyps3 {
namespace win {

struct DiagContext {
    std::string exe_dir;     // where Any_Ps3.exe lives (output goes here)
    std::string config_dir;  // app data dir (per-game status comes from here)
    std::string cache_dir;   // AOT cache root
};

// Writes anyps3-diagnostics-<yyyymmdd-hhmmss>.txt next to the exe.
//   log_text — current log ring content (last 500 lines are kept)
//   extra    — optional extra block (e.g. the previous crash report)
// Returns the written path, or "" on failure.
std::string ExportDiagnostics(const DiagContext& ctx, const std::string& log_text,
                              const std::string& extra = "");

// Same report as text without writing a file (used by unit-style checks and
// future hosts).
std::string BuildDiagnosticsReport(const DiagContext& ctx, const std::string& log_text,
                                    const std::string& extra = "");

}  // namespace win
}  // namespace anyps3
