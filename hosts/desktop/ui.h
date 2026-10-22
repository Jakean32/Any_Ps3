// Any_Ps3 desktop host — UI logic. Dear ImGui windows:
//   - first-run legal consent
//   - game library (list + search + covers)
//   - add game (text path; drag-drop lands in the same flow)
//   - per-game settings
//   - import/compile screen with progress + log
//   - about/limitations
#pragma once

#include <string>
#include <vector>

#include "anyps3/config.h"
#include "anyps3/loader/import.h"

namespace anyps3 {
namespace desktop {

struct UiState {
    std::string config_dir;
    std::string pending_import_path;  // set by drag-drop or CLI arg
    bool quit = false;

    // library cache
    std::vector<LibraryEntry> games;
    std::string search;

    // settings screen
    bool show_settings_for = false;
    std::string settings_title_id;
    GameSettings editing;

    // import screen
    bool importing = false;
    std::string import_status;
    std::string import_game_id;

    // runtime
    bool running = false;
    std::string running_title_id;
    std::string runtime_log;
    AppConfig app{"."};
};

// Helpers shared with main.cpp
std::string DefaultConfigDir(int argc, char** argv);

// Draws one frame. renderer may be used by the future game surface.
struct SDL_Renderer;
void UiMain(UiState* ui, SDL_Renderer** renderer);

}  // namespace desktop
}  // namespace anyps3
