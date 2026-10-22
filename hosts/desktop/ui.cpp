// Any_Ps3 desktop host — UI implementation (Dear ImGui).
#include "ui.h"

#include <cstdio>
#include <sys/stat.h>

#include "imgui.h"

#include "anyps3/log.h"
#include "anyps3/runtime/emulator.h"

namespace anyps3 {
namespace desktop {

static AppConfig& EmuApp() { static AppConfig c{"."}; return c; }
static Emulator g_emu{EmuApp()};

std::string DefaultConfigDir(int argc, char** argv) {
    (void)argc;
    (void)argv;
#ifdef _WIN32
    return ".\\anyps3_data";  // portable: next to the exe
#else
    const char* home = getenv("HOME");
    if (home) return std::string(home) + "/.local/share/anyps3";
    return "./anyps3_data";
#endif
}

static void EnsureDir(const std::string& path) {
    std::string cur;
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        cur = path.substr(0, pos);
        ::mkdir(cur.c_str(), 0755);
    }
    ::mkdir(path.c_str(), 0755);
}

static void RefreshLibrary(UiState* ui) {
    ui->app.config_dir = ui->config_dir;
    EnsureDir(ui->config_dir);
    GameLibrary lib(ui->config_dir);
    lib.List(&ui->games);
}

static void AppendLog(UiState* ui, const std::string& line) {
    ui->runtime_log += line;
    ui->runtime_log += "\n";
    if (ui->runtime_log.size() > 40000) ui->runtime_log.erase(0, ui->runtime_log.size() - 40000);
}

static void DrawFirstRun(UiState* ui) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720, 0), ImGuiCond_Always);
    ImGui::Begin("Welcome to Any_Ps3", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::TextWrapped("Any_Ps3 runs PlayStation 3 software on your PC or phone by "
                       "translating it ahead-of-time. It contains no Sony firmware, no "
                       "decryption keys, and no game content.");
    ImGui::Spacing();
    ImGui::TextWrapped("To use it you must confirm all of the following:");
    ImGui::BulletText("I own the games I import and my dumps are legal.");
    ImGui::BulletText("I will not import content I do not own.");
    ImGui::BulletText("I understand encrypted retail executables cannot be loaded "
                      "(Any_Ps3 ships no keys) and homebrew/debug executables are supported.");
    ImGui::Spacing();
    if (ImGui::Button("I agree — continue", ImVec2(240, 0))) {
        EnsureDir(ui->config_dir);
        ui->app.config_dir = ui->config_dir;
        ui->app.AcceptFirstRun();
        ui->app.global.SaveToFile(ui->config_dir + "/anyps3.ini");
        RefreshLibrary(ui);
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(120, 0))) ui->quit = true;
    ImGui::End();
}

static void DrawLibrary(UiState* ui) {
    ImGui::Begin("Game Library");
    if (ImGui::Button("Add game...")) ImGui::OpenPopup("add_game");
    ImGui::SameLine();
    if (ui->importing) {
        ImGui::TextDisabled("(importing %s...)", ui->import_game_id.c_str());
    }
    ImGui::InputTextWithHint("##search", "search", &ui->search);
    ImGui::Separator();

    if (ui->games.empty()) {
        ImGui::TextDisabled("No games yet. Use 'Add game...' or drop an ISO/folder on the "
                            "window.");
    }
    int idx = 0;
    for (const auto& entry : ui->games) {
        std::string label = entry.info.title + "  [" + entry.info.title_id + "]";
        if (!ui->search.empty() &&
            label.find(ui->search) == std::string::npos)
            continue;
        ImGui::PushID(idx++);
        if (ImGui::CollapsingHeader(label.c_str())) {
            ImGui::Text("Category: %s   App: %s   System: %s", entry.info.category.c_str(),
                        entry.info.app_ver.c_str(), entry.info.system_ver.c_str());
            ImGui::TextWrapped("Source: %s", entry.info.source_path.c_str());
            if (ImGui::Button("Settings")) {
                ui->settings_title_id = entry.info.title_id;
                ui->editing = GameSettings::Load(ui->app, entry.info.title_id);
                ui->show_settings_for = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Clear AOT cache")) {
                AotCache cache(ui->app.CacheRoot());
                cache.Clear(entry.info.title_id);
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove from library")) {
                GameLibrary lib(ui->config_dir);
                std::string err;
                lib.Remove(entry.info.title_id, &err);
                RefreshLibrary(ui);
            }
            ImGui::Spacing();
            if (!ui->running) {
                if (ImGui::Button("Run", ImVec2(160, 40))) {
                    ui->running = true;
                    ui->running_title_id = entry.info.title_id;
                    ui->runtime_log.clear();
                    // Boot + run on a helper path; the emulator runs on its own thread
                    // inside Boot/Run (see EmulatorCallbacks below).
                    EmulatorCallbacks cb;
                    cb.log = [ui](const std::string& line) { AppendLog(ui, line); };
                    cb.progress = [ui](int pct, const std::string& note) {
                        AppendLog(ui, "[" + std::to_string(pct) + "%] " + note);
                    };
                    GameSettings gs = GameSettings::Load(ui->app, entry.info.title_id);
                    BootResult r = g_emu.Boot(entry.info.title_id, gs, cb);
                    for (const auto& n : r.notes) AppendLog(ui, n);
                    if (!r.ok) {
                        AppendLog(ui, "BOOT FAILED: " + r.error);
                        ui->running = false;
                    }
                }
            } else if (ui->running_title_id == entry.info.title_id) {
                if (ImGui::Button("Stop", ImVec2(160, 40))) {
                    g_emu.Stop();
                    ui->running = false;
                }
            }
        }
        ImGui::PopID();
    }

    // Add game dialog: path text (drag-drop also lands here)
    if (ImGui::BeginPopupModal("add_game", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char path[1024] = "";
        if (!ui->pending_import_path.empty()) {
            std::snprintf(path, sizeof(path), "%s", ui->pending_import_path.c_str());
            ui->pending_import_path.clear();
        }
        ImGui::Text("Path to a PS3 ISO image or a game folder (JB dump):");
        ImGui::InputText("##path", path, sizeof(path));
        if (ImGui::Button("Import", ImVec2(140, 0))) {
            ui->importing = true;
            ui->import_status.clear();
            GameLibrary lib(ui->config_dir);
            GameInfo info;
            std::string err;
            if (lib.Import(path, &info, &err)) {
                ui->import_status = "Imported: " + info.title + " (" + info.title_id + ")";
                ui->import_game_id = info.title_id;
                RefreshLibrary(ui);
                ui->importing = false;
            } else {
                ui->import_status = "Import failed: " + err;
                ui->importing = false;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        if (!ui->import_status.empty()) ImGui::TextUnformatted(ui->import_status.c_str());
        ImGui::EndPopup();
    }
    ImGui::End();
}

static void DrawSettings(UiState* ui) {
    if (!ui->show_settings_for) return;
    ImGui::Begin(("Settings — " + ui->settings_title_id).c_str(), &ui->show_settings_for);
    ImGui::SliderInt("Resolution scale", &ui->editing.resolution_scale, 1, 4);
    ImGui::SliderInt("Frame limit (0 = off)", &ui->editing.frame_limit, 0, 120);
    ImGui::Checkbox("Shader cache", reinterpret_cast<bool*>(&ui->editing.shader_cache_enabled));
    const char* backends[] = {"Auto (AOT + interpreter fallback)", "Interpreter only"};
    ImGui::Combo("PPU backend", &ui->editing.ppu_backend, backends, 2);
    ImGui::Separator();
    ImGui::Text("Controller mapping (guest button = host code)");
    static const char* pad_buttons[] = {"cross", "circle", "square", "triangle", "start",
                                        "select", "l1", "r1", "l2", "r2", "l3", "r3", "up",
                                        "down", "left", "right"};
    for (const char* btn : pad_buttons) {
        int v = ui->editing.controller.count(btn) ? ui->editing.controller[btn] : -1;
        ImGui::InputInt((std::string("##") + btn).c_str(), &v, 0, 0);
        ImGui::SameLine();
        ImGui::TextUnformatted(btn);
        ui->editing.controller[btn] = v;
    }
    ImGui::Separator();
    if (ImGui::Button("Save")) {
        ui->editing.Save(ui->app, ui->settings_title_id);
        ui->show_settings_for = false;
    }
    ImGui::End();
}

static void DrawRuntime(UiState* ui) {
    ImGui::Begin("Runtime", nullptr, ImGuiWindowFlags_NoCollapse);
    if (!ui->running) {
        ImGui::TextDisabled("no game running");
    } else {
        ImGui::Text("Running: %s", ui->running_title_id.c_str());
        const EmuStats& s = g_emu.stats();
        ImGui::Text("blocks: %llu   fallbacks: %llu   syscalls: %llu",
                    (unsigned long long)s.blocks_executed,
                    (unsigned long long)s.interpreter_fallbacks,
                    (unsigned long long)s.syscalls);
    }
    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(ui->runtime_log.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

void UiMain(UiState* ui, SDL_Renderer** renderer) {
    (void)renderer;  // game surface (RSX presentation) arrives with the graphics phase
    if (ui->app.config_dir.empty()) ui->app.config_dir = ui->config_dir;
    static bool first_frame = true;
    if (first_frame) {
        first_frame = false;
        EnsureDir(ui->config_dir);
        ui->app.config_dir = ui->config_dir;
        ui->app.global.LoadFromFile(ui->config_dir + "/anyps3.ini");
        // route core logs into the runtime window
        SetLogSink([ui](LogLevel, const std::string& msg) { AppendLog(ui, msg); });
        RefreshLibrary(ui);
    }

    if (!ui->app.FirstRunAccepted()) {
        DrawFirstRun(ui);
        return;
    }
    ImGui::BeginMainMenuBar();
    if (ImGui::BeginMenu("Any_Ps3")) {
        if (ImGui::MenuItem("About / limitations")) ImGui::OpenPopup("about");
        if (ImGui::MenuItem("Quit")) ui->quit = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();

    DrawLibrary(ui);
    DrawSettings(ui);
    DrawRuntime(ui);

    if (ImGui::BeginPopupModal("about", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(
            "Any_Ps3 is an experimental AOT-recompilation-based PS3 launcher.\n"
            "Status: import/library/settings subsystems are functional; the execution "
            "runtime, SPU and graphics phases are under active development. See "
            "PHASE_REPORTS.md for an honest capability matrix.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace desktop
}  // namespace anyps3
