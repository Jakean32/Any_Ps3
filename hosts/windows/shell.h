// Any_Ps3 Windows shell — app state + screens (Dear ImGui + Win32 + D3D11).
// The window/D3D11 plumbing lives in main.cpp; ShellFrame() below draws one
// UI frame. No emulation is claimed: "Play" runs the real boot pipeline and
// shows its log; only the built-in hello-world demo actually executes, and
// every run displays "Emulation not implemented yet".
#pragma once

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "anyps3/config.h"
#include "anyps3/loader/import.h"
#include "anyps3/runtime/emulator.h"

#include "imgui.h"

namespace anyps3 {
namespace win {

// A game cover: decoded ICON0.PNG uploaded as a D3D11 texture.
struct Cover {
    ImTextureID tex = 0;  // SRV pointer (ImTextureID is an integer type)
    int w = 0;
    int h = 0;
    bool ok = false;
};

enum class Screen { Library, GameSettings, AppSettings, About };

struct ShellState {
    // paths
    std::string exe_dir;
    std::string config_dir;
    std::string samples_dir;

    // config + library
    AppConfig app{"."};
    std::vector<LibraryEntry> games;
    std::string search;

    // screens
    Screen screen = Screen::Library;
    std::string settings_title_id;
    GameSettings editing;
    std::string about_msg;

    // log ring (shared with worker threads)
    std::mutex log_mu;
    std::deque<std::string> log_lines;
    std::string pending_import_path;
    bool import_busy = false;
    std::string import_status;

    // disc-key picker (per-game settings): main.cpp writes the picked path
    std::string pending_dkey_path;
    std::string pending_eboot_path;   // user-picked decrypted executable

    // disc-key re-read worker ("attach key and re-read the image")
    bool rekey_busy = false;  // guarded by log_mu

    // crash handler: report of a previous session's crash ("" = none)
    std::string crash_report;
    std::string crash_report_path;

    // boot/run screen
    bool running = false;
    bool boot_finished = false;
    std::string running_title_id;
    std::string running_title;
    int exit_code = -1;

    // demo presence
    bool demo_installed = false;

    // per-entry covers keyed by library dir path
    std::vector<std::pair<std::string, Cover>> covers;

    bool quit = false;
};

// thread-safe log append (used from worker threads + log sink)
void ShellLog(ShellState* s, const std::string& line);

// main.cpp calls this after D3D11 init so covers can create textures.
// (typed as void* to keep d3d11.h out of this header)
void ShellSetDevice(void* d3d_device);

// one frame of UI; call between ImGui NewFrame/Render
void ShellFrame(ShellState* s);

// helpers used by main.cpp
void ShellInit(ShellState* s);         // config, sink, library load, demo install
void ShellStartImport(ShellState* s, const std::string& path);
void ShellStartPlay(ShellState* s, const LibraryEntry& entry);
// Re-read the game's ISO with a newly attached disc key and update its
// library entry (title/id/cover become real; state -> "Ready to compile").
void ShellStartRekey(ShellState* s, const std::string& title_id);
void ShellShutdown(ShellState* s);

}  // namespace win
}  // namespace anyps3
