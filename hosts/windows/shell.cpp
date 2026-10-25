// Any_Ps3 Windows shell — UI implementation. Dear ImGui screens: first-run
// consent, game library with covers, import (Win32 file dialog), per-game
// settings, app settings, log panel and the honest "boot log" player.
//
// Threading contract:
//  - Worker threads (import, boot/run) only mutate ShellState under log_mu
//    (via Lock/Unlock helpers).
//  - The UI thread snapshots the shared fields under the same mutex at the
//    start of each frame (ShellFrame) and draws from the snapshot.
//  - Covers (D3D11 SRVs) are created/released only on the UI thread; the
//    D3D11 device is created WITHOUT the SINGLETHREADED flag so worker-side
//    core code stays happy, but no D3D resource is ever touched by workers.
#include "shell.h"

#include <cstdint>
#include <cstdio>
#include <thread>

#include "anyps3/demo/hello.h"
#include "anyps3/log.h"
#include "anyps3/platform.h"

#include "diagnostics.h"

// stb_image for ICON0.PNG covers (public domain).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STB_IMAGE_STATIC
#include "stb_image.h"

// ImGui std::string widget overloads (InputText with std::string).
#include "misc/cpp/imgui_stdlib.h"

#include <d3d11.h>

namespace anyps3 {
namespace win {

static Emulator* g_emu = nullptr;
static std::thread g_worker;

static inline void Lock(ShellState* s) { s->log_mu.lock(); }
static inline void Unlock(ShellState* s) { s->log_mu.unlock(); }

// --------------------------------------------------------------------------
void ShellLog(ShellState* s, const std::string& line) {
    std::lock_guard<std::mutex> lock(s->log_mu);
    s->log_lines.push_back(line);
    while (s->log_lines.size() > 2000) s->log_lines.pop_front();
}

static std::string DrainLog(ShellState* s) {
    std::string out;
    std::lock_guard<std::mutex> lock(s->log_mu);
    for (const auto& l : s->log_lines) {
        out += l;
        out += "\n";
    }
    return out;
}

// --------------------------------------------------------------------------
// Covers: ICON0.PNG -> RGBA -> D3D11 SRV (UI thread only).
// --------------------------------------------------------------------------
static ID3D11Device* g_d3d = nullptr;
void ShellSetDevice(void* dev) { g_d3d = reinterpret_cast<ID3D11Device*>(dev); }

static bool UploadCover(const std::vector<unsigned char>& png, Cover* out) {
    if (!g_d3d || png.empty()) return false;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(png.data(), static_cast<int>(png.size()),
                                             &w, &h, &comp, 4);
    if (!px) return false;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(w);
    desc.Height = static_cast<UINT>(h);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = px;
    data.SysMemPitch = static_cast<UINT>(w) * 4;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = g_d3d->CreateTexture2D(&desc, &data, &tex);
    stbi_image_free(px);
    if (FAILED(hr) || !tex) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = desc.Format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    hr = g_d3d->CreateShaderResourceView(tex, &srvd, &srv);
    tex->Release();
    if (FAILED(hr) || !srv) return false;
    out->tex = reinterpret_cast<ImTextureID>(srv);
    out->w = w;
    out->h = h;
    out->ok = true;
    return true;
}

static Cover* FindCover(ShellState* s, const std::string& key) {
    for (auto& kv : s->covers)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

static void EnsureCover(ShellState* s, const LibraryEntry& e) {
    if (FindCover(s, e.dir)) return;
    Cover c;
    UploadCover(e.info.icon_png, &c);
    s->covers.push_back({e.dir, c});  // UI thread only
}

// --------------------------------------------------------------------------
static void RefreshLibrary(ShellState* s) {
    std::vector<LibraryEntry> fresh;
    GameLibrary lib(s->config_dir);
    lib.List(&fresh);
    bool demo = false;
    for (const auto& e : fresh)
        if (e.info.title_id == demo::kDemoTitleId) demo = true;
    Lock(s);
    s->games = std::move(fresh);
    s->demo_installed = demo;
    Unlock(s);
}

static void LoadGlobalConfig(ShellState* s) {
    platform::MakeDirs(s->config_dir);
    s->app.config_dir = s->config_dir;
    s->app.global.LoadFromFile(s->config_dir + "/anyps3.ini");
}

// --------------------------------------------------------------------------
void ShellInit(ShellState* s) {
    LoadGlobalConfig(s);
    SetLogLevel(s->app.LogLevel());
    SetLogSink([s](LogLevel, const std::string& msg) { ShellLog(s, msg); });
    g_emu = new Emulator(s->app);

    // Crash handler: if the previous session left a crash report, show it.
    {
        std::string path = s->config_dir + "/crash_last.txt";
        FILE* f = std::fopen(path.c_str(), "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END);
            long sz = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::string text;
            text.resize(static_cast<size_t>(sz > 0 ? sz : 0));
            if (!text.empty()) {
                size_t got = std::fread(&text[0], 1, text.size(), f);
                text.resize(got);
            }
            std::fclose(f);
            if (!text.empty()) {
                s->crash_report = std::move(text);
                s->crash_report_path = path;
                ShellLog(s, "previous session did not shut down cleanly — crash log found "
                           "(see the dialog; Export writes it next to the exe)");
            }
        }
    }

    if (!s->app.FirstRunAccepted()) return;  // consent screen first

    RefreshLibrary(s);
    // Built-in demo: install once so the library always has one entry whose
    // "Play" genuinely executes (the GNU-assembled PPC64 hello world).
    std::string sample = s->samples_dir + "/hello.elf";
    bool have_demo;
    {
        Lock(s);
        have_demo = s->demo_installed;
        Unlock(s);
    }
    if (!have_demo && platform::IsFile(sample)) {
        std::string err;
        if (demo::InstallHelloDemo(s->config_dir, sample, nullptr, &err)) {
            ShellLog(s, "built-in demo installed (samples/hello.elf)");
        } else {
            ShellLog(s, "demo install failed: " + err);
        }
        RefreshLibrary(s);
    } else if (!have_demo) {
        ShellLog(s, "samples/hello.elf not found — built-in demo unavailable");
    }
}

void ShellShutdown(ShellState* s) {
    if (g_worker.joinable()) {
        if (g_emu) g_emu->Stop();
        g_worker.join();
    }
    // Release cover SRVs while the D3D device is still alive (main.cpp calls
    // ShellShutdown before destroying the device).
    for (auto& kv : s->covers) {
        if (kv.second.ok && kv.second.tex) {
            reinterpret_cast<ID3D11ShaderResourceView*>(
                reinterpret_cast<void*>(static_cast<uintptr_t>(kv.second.tex)))->Release();
            kv.second = Cover{};
        }
    }
    delete g_emu;
    g_emu = nullptr;
    SetLogSink(nullptr);
}

// --------------------------------------------------------------------------
// Import worker: runs the real pipeline on a thread so the UI stays alive.
// --------------------------------------------------------------------------
static void ImportWorker(ShellState* s, std::string path) {
    ShellLog(s, "import: " + path);
    GameLibrary lib(s->config_dir);
    GameInfo info;
    std::string err;
    bool ok = lib.Import(path, &info, &err);
    if (ok) {
        ShellLog(s, "import ok: " + info.title + " [" + info.title_id + "]");
    } else {
        ShellLog(s, "import failed: " + err);
    }
    Lock(s);
    if (ok) s->import_status = "Imported: " + info.title + " (" + info.title_id + ")";
    else s->import_status = "Import failed: " + err;
    Unlock(s);
    RefreshLibrary(s);
    Lock(s);
    s->import_busy = false;
    Unlock(s);
}

void ShellStartImport(ShellState* s, const std::string& path) {
    if (s->import_busy || s->running) return;
    if (g_worker.joinable()) g_worker.join();
    Lock(s);
    s->import_busy = true;
    s->import_status = "importing...";
    Unlock(s);
    g_worker = std::thread(ImportWorker, s, path);
}

// --------------------------------------------------------------------------
// Play worker: boot + run. For real games this honestly stops where the
// implementation ends (e.g. encrypted EBOOT refusal); for the built-in demo it
// executes the GNU-assembled PPC64 hello world through the interpreter.
// --------------------------------------------------------------------------
static void PlayWorker(ShellState* s, LibraryEntry entry) {
    GameSettings gs = GameSettings::Load(s->app, entry.info.title_id);
    EmulatorCallbacks cb;
    cb.log = [s](const std::string& l) { ShellLog(s, l); };
    cb.progress = [s](int pct, const std::string& note) {
        ShellLog(s, "[" + std::to_string(pct) + "%] " + note);
    };
    BootResult r = g_emu->Boot(entry.info.title_id, gs, cb);
    for (const auto& n : r.notes) ShellLog(s, n);
    if (!r.ok) {
        ShellLog(s, "BOOT FAILED: " + r.error);
        ShellLog(s, "Emulation not implemented yet — see PHASE_REPORTS.md for what "
                   "the boot pipeline covers today.");
    } else {
        ShellLog(s, "Emulation not implemented yet — this was the boot pipeline; "
                    "only the built-in hello-world demo executes in this build.");
        int code = g_emu->Run();
        ShellLog(s, "guest exited with code " + std::to_string(code));
        Lock(s);
        s->exit_code = code;
        Unlock(s);
    }
    Lock(s);
    s->boot_finished = true;
    s->running = false;
    Unlock(s);
}

void ShellStartPlay(ShellState* s, const LibraryEntry& entry) {
    if (s->running) return;
    if (g_worker.joinable()) g_worker.join();
    Lock(s);
    s->running = true;
    s->boot_finished = false;
    s->running_title_id = entry.info.title_id;
    s->running_title = entry.info.title;
    s->exit_code = -1;
    Unlock(s);
    ShellLog(s, "---- boot: " + entry.info.title + " [" + entry.info.title_id + "] ----");
    g_worker = std::thread(PlayWorker, s, entry);
}

// ---------------------------------------------------------------------------
// Re-key worker: re-read the game's image with the disc key the user just
// attached. On success the library entry gets its real title/id/cover and the
// game becomes "Ready to compile". On failure the old entry stays untouched
// and the log says exactly what happened (honesty first).
// ---------------------------------------------------------------------------
static void RekeyWorker(ShellState* s, std::string old_id) {
    std::string source_path, source_uri;
    {
        GameLibrary lib(s->config_dir);
        std::vector<LibraryEntry> games;
        lib.List(&games);
        for (const auto& e : games) {
            if (e.info.title_id == old_id) {
                source_path = e.info.source_path;
                source_uri = e.info.source_uri;
            }
        }
    }
    GameSettings gs = GameSettings::Load(s->app, old_id);
    std::string kerr;
    std::string key_text = EffectiveDiscKeyText(gs, &kerr);
    std::string result;
    if (source_path.empty()) {
        result = "re-read failed: game not found in library";
    } else if (key_text.empty()) {
        result = "re-read failed: no usable disc key (" + kerr + ")";
    } else {
        GameInfo info;
        std::string err;
        ShellLog(s, "re-reading the image with the attached disc key...");
        if (!InspectGame(source_path, &info, &err, key_text)) {
            result = "the disc key did NOT unlock this image: " + err;
        } else {
            info.source_uri = source_uri;
            GameLibrary lib(s->config_dir);
            std::string rerr;
            if (ReplaceLibraryEntry(s->app, &lib, info, old_id, &rerr)) {
                result = "disc key accepted — image unlocked: " + info.title + " [" +
                         info.title_id + "] — Ready to compile";
            } else {
                result = "re-read failed: " + rerr;
            }
        }
    }
    ShellLog(s, result);
    RefreshLibrary(s);
    Lock(s);
    s->import_status = result;
    s->rekey_busy = false;
    Unlock(s);
}

void ShellStartRekey(ShellState* s, const std::string& title_id) {
    if (s->import_busy || s->running || s->rekey_busy) return;
    if (g_worker.joinable()) g_worker.join();
    Lock(s);
    s->rekey_busy = true;
    s->import_status = "re-reading the image with the attached disc key...";
    Unlock(s);
    g_worker = std::thread(RekeyWorker, s, title_id);
}

// --------------------------------------------------------------------------
// UI style
// --------------------------------------------------------------------------
static void ApplyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.f;
    st.FrameRounding = 4.f;
    st.GrabRounding = 4.f;
    st.TabRounding = 4.f;
    st.ScrollbarRounding = 6.f;
    st.FramePadding = ImVec2(8, 4);
    st.WindowPadding = ImVec2(10, 8);
    st.ItemSpacing = ImVec2(8, 6);
    ImVec4* c = st.Colors;
    ImVec4 bg(0.063f, 0.063f, 0.078f, 1.f);      // #101014
    ImVec4 panel(0.094f, 0.105f, 0.133f, 1.f);   // #181B22
    ImVec4 panelhi(0.145f, 0.16f, 0.2f, 1.f);
    ImVec4 accent(0.31f, 0.549f, 1.f, 1.f);      // #4F8CFF
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Border] = panelhi;
    c[ImGuiCol_FrameBg] = panel;
    c[ImGuiCol_FrameBgHovered] = panelhi;
    c[ImGuiCol_FrameBgActive] = panelhi;
    c[ImGuiCol_TitleBg] = panel;
    c[ImGuiCol_TitleBgActive] = panel;
    c[ImGuiCol_TitleBgCollapsed] = panel;
    c[ImGuiCol_MenuBarBg] = panel;
    c[ImGuiCol_Header] = panelhi;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.2f, 0.26f, 1.f);
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Button] = panel;
    c[ImGuiCol_ButtonHovered] = panelhi;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Separator] = panelhi;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = panelhi;
    c[ImGuiCol_TabSelected] = panelhi;
    c[ImGuiCol_ScrollbarBg] = bg;
    c[ImGuiCol_ScrollbarGrab] = panelhi;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Text] = ImVec4(0.9f, 0.92f, 0.95f, 1.f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.65f, 1.f);
    c[ImGuiCol_TableHeaderBg] = panel;
}

// --------------------------------------------------------------------------
// Screens
// --------------------------------------------------------------------------
static void DrawFirstRun(ShellState* s) {
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_Always);
    ImGui::Begin("Welcome to Any_Ps3", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::TextWrapped(
        "Any_Ps3 is an experimental PlayStation 3 launcher based on AOT "
        "recompilation. It contains NO Sony firmware, NO decryption keys and NO "
        "game content.");
    ImGui::Spacing();
    ImGui::TextUnformatted("You must confirm all of the following to continue:");
    ImGui::BulletText("I only import game dumps I legally own.");
    ImGui::BulletText("I will not use Any_Ps3 with content I do not own.");
    ImGui::BulletText(
        "I understand encrypted retail executables cannot be loaded (no keys "
        "are shipped); homebrew and debug executables are supported.");
    ImGui::BulletText("This build's 'Play' shows the real boot log — game "
                      "execution is not implemented yet.");
    ImGui::Spacing();
    if (ImGui::Button("I agree - continue", ImVec2(240, 0))) {
        s->app.AcceptFirstRun();
        s->app.global.SaveToFile(s->config_dir + "/anyps3.ini");
        ShellInit(s);  // reload library + install demo
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(120, 0))) s->quit = true;
    ImGui::End();
}

static void DrawGameRow(ShellState* s, const LibraryEntry& e, bool running_any) {
    ImGui::TableNextRow();
    // Per-game ID scope: without this, two visible rows with identical button
    // labels ("Play"/"Settings"/"Remove") collide and ImGui reports
    // "2 visible items with conflicting ID!". The title id is a stable key.
    ImGui::PushID(e.info.title_id.c_str());
    ImGui::TableSetColumnIndex(0);
    ImVec2 cur = ImGui::GetCursorScreenPos();
    float icon_sz = 48.f;
    Cover* cov = FindCover(s, e.dir);
    if (cov && cov->ok) {
        ImGui::Image(cov->tex, ImVec2(icon_sz, icon_sz));
    } else {
        // placeholder: gradient box with title initials
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(cur, ImVec2(cur.x + icon_sz, cur.y + icon_sz),
                          IM_COL32(0x24, 0x2C, 0x3A, 255), 4.f);
        dl->AddRect(cur, ImVec2(cur.x + icon_sz, cur.y + icon_sz),
                    IM_COL32(0x4F, 0x8C, 0xFF, 128), 4.f);
        std::string initials;
        for (char ch : e.info.title) {
            if (ch >= 'A' && ch <= 'Z') initials += ch;
            if (initials.size() == 2) break;
        }
        if (initials.empty() && !e.info.title.empty()) initials = e.info.title.substr(0, 1);
        ImVec2 ts = ImGui::CalcTextSize(initials.c_str());
        dl->AddText(ImVec2(cur.x + (icon_sz - ts.x) * 0.5f, cur.y + (icon_sz - ts.y) * 0.5f),
                    IM_COL32(255, 255, 255, 200), initials.c_str());
        ImGui::Dummy(ImVec2(icon_sz, icon_sz));
    }
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(e.info.title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("[%s]", e.info.title_id.c_str());
    if (!e.info.category.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("cat %s", e.info.category.c_str());
    }
    if (e.info.encrypted == EncState::NeedsKey) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f), "Encrypted - disc key needed");
    } else {
        if (e.info.encrypted == EncState::KeyInUse) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Encrypted (decryption active)");
        }
        // Imported with readable metadata: the compile pipeline can run on it.
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Ready to compile");
    }
    if (!e.info.meta_source.empty() && e.info.meta_source != "PARAM.SFO") {
        ImGui::SameLine();
        ImGui::TextDisabled("id from %s", e.info.meta_source.c_str());
    }
    ImGui::TextDisabled("%s", e.info.source_path.c_str());

    ImGui::TableSetColumnIndex(2);
    ImGui::BeginDisabled(running_any);
    if (ImGui::Button("Play", ImVec2(90, 0))) ShellStartPlay(s, e);
    ImGui::EndDisabled();
    ImGui::TableSetColumnIndex(3);
    if (ImGui::Button("Settings")) {
        s->settings_title_id = e.info.title_id;
        s->editing = GameSettings::Load(s->app, e.info.title_id);
        s->screen = Screen::GameSettings;
    }
    ImGui::TableSetColumnIndex(4);
    ImGui::BeginDisabled(running_any);
    if (ImGui::Button("Remove")) {
        GameLibrary lib(s->config_dir);
        std::string err;
        lib.Remove(e.info.title_id, &err);
        RefreshLibrary(s);
    }
    ImGui::EndDisabled();
    ImGui::PopID();  // per-game ID scope (see top of DrawGameRow)
}

static void DrawLibrary(ShellState* s, const std::vector<LibraryEntry>& games,
                        bool import_busy, const std::string& import_status) {
    ImGui::Begin("Library", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::BeginDisabled(import_busy);
    if (ImGui::Button("Add game (ISO / folder)...")) {
        extern void WinOpenFileDialog(ShellState*);
        WinOpenFileDialog(s);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (import_busy) {
        ImGui::TextDisabled("importing...");
    } else if (!import_status.empty()) {
        ImGui::TextDisabled("%s", import_status.c_str());
    }
    if (!s->pending_import_path.empty() && !import_busy) {
        std::string p = s->pending_import_path;
        s->pending_import_path.clear();
        ShellStartImport(s, p);
    }
    ImGui::SameLine();
    ImGui::PushItemWidth(220);
    ImGui::InputTextWithHint("##search", "search...", &s->search);
    ImGui::PopItemWidth();
    ImGui::Separator();

    if (games.empty()) {
        ImGui::TextDisabled("Library is empty. Add an ISO/folder dump, or check "
                            "samples/hello.elf for the built-in demo.");
    }

    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH;
    if (ImGui::BeginTable("games", 5, tf)) {
        ImGui::TableSetupColumn("cover", ImGuiTableColumnFlags_WidthFixed, 62);
        ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 98);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 98);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 98);
        for (const auto& e : games) {
            EnsureCover(s, e);
            if (!s->search.empty() &&
                e.info.title.find(s->search) == std::string::npos &&
                e.info.title_id.find(s->search) == std::string::npos)
                continue;
            DrawGameRow(s, e, s->running);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

static void DrawGameSettings(ShellState* s) {
    ImGui::Begin("Game settings", nullptr, ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted(s->settings_title_id.c_str());
    ImGui::Separator();

    // Only settings for features that actually exist and work in this build
    // are shown. (Resolution scale / frame limit / shader cache / PPU backend
    // have no effect on this host and are deliberately absent.)
    ImGui::TextUnformatted("Disc key (encrypted ISO images)");
    {
        bool have_key = !s->editing.dkey_path.empty() || !s->editing.dkey_hex.empty();
        if (!have_key) {
            ImGui::TextDisabled("none - encrypted images cannot be read yet");
        } else {
            if (!s->editing.dkey_path.empty())
                ImGui::TextDisabled("key file: %s", s->editing.dkey_path.c_str());
            if (!s->editing.dkey_hex.empty())
                ImGui::TextDisabled("typed key: %d hex chars", (int)s->editing.dkey_hex.size());
        }
    }
    if (ImGui::Button("Select disc key (.dkey / 32 hex characters)...")) {
        extern void WinOpenKeyFileDialog(ShellState*);
        WinOpenKeyFileDialog(s);
    }
    if (!s->pending_dkey_path.empty()) {
        s->editing.dkey_path = s->pending_dkey_path;
        s->editing.dkey_hex.clear();  // a picked file replaces a typed key
        s->pending_dkey_path.clear();
        ShellLog(s, "disc key file chosen: " + s->editing.dkey_path);
    }
    ImGui::PushItemWidth(340);
    ImGui::InputTextWithHint("##dkey_hex", "or paste the 32-hex-character disc key",
                            &s->editing.dkey_hex);
    ImGui::PopItemWidth();
    if (!s->editing.dkey_hex.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear##dkeyhex")) {
            s->editing.dkey_hex.clear();
        }
    }
    if (!s->editing.dkey_hex.empty() && s->editing.dkey_hex.size() != 32) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f),
                           "a disc key is exactly 32 hex characters (16 bytes) — %d so far",
                           (int)s->editing.dkey_hex.size());
    }
    ImGui::TextWrapped(
        "The key never leaves this machine. Any_Ps3 ships no keys and no key "
        "databases; a sibling file named <image>.dkey / <image>.key next to "
        "the ISO is picked up automatically. After Save, the image is re-read "
        "in place with the key — nothing is copied.");

    ImGui::Separator();
    ImGui::TextUnformatted("Decrypted executable (encrypted retail EBOOTs)");
    {
        if (s->editing.eboot_path.empty()) {
            ImGui::TextDisabled("none — retail EBOOT.BIN cannot be decrypted by Any_Ps3");
        } else {
            ImGui::TextDisabled("executable: %s", s->editing.eboot_path.c_str());
        }
    }
    if (ImGui::Button("Select decrypted executable (your own ELF / fSELF)...")) {
        extern void WinOpenEbootDialog(ShellState*);
        WinOpenEbootDialog(s);
    }
    if (!s->pending_eboot_path.empty()) {
        s->editing.eboot_path = s->pending_eboot_path;
        s->pending_eboot_path.clear();
        ShellLog(s, "decrypted executable chosen: " + s->editing.eboot_path);
    }
    if (!s->editing.eboot_path.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Clear##eboot")) {
            s->editing.eboot_path.clear();
            ShellLog(s, "decrypted executable cleared — the disc's EBOOT.BIN will be used");
        }
    }
    ImGui::TextWrapped(
        "Retail discs carry an encrypted EBOOT.BIN. Any_Ps3 ships no keys "
        "(legal requirement), so it cannot decrypt them. If you decrypted "
        "your own legal dump with your own tools, pick that file here and "
        "Play will boot it instead — imports are resolved against the "
        "built-in system-module HLE, and the boot log reports exactly how "
        "far the game gets.");

    ImGui::Separator();
    if (ImGui::Button("Save")) {
        s->editing.Save(s->app, s->settings_title_id);
        ShellLog(s, "settings saved for " + s->settings_title_id);
        // If the game still needs a key and one is now attached, re-read the
        // image and update the entry (-> "Ready to compile").
        bool needs_rekey = false;
        {
            GameLibrary lib(s->config_dir);
            std::vector<LibraryEntry> games;
            lib.List(&games);
            for (const auto& e : games) {
                if (e.info.title_id == s->settings_title_id &&
                    e.info.encrypted == EncState::NeedsKey) {
                    needs_rekey = true;
                }
            }
        }
        std::string kerr;
        s->screen = Screen::Library;
        if (needs_rekey && !EffectiveDiscKeyText(s->editing, &kerr).empty()) {
            ShellStartRekey(s, s->settings_title_id);
        } else if (needs_rekey) {
            ShellLog(s, "game still needs a disc key — none attached");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) s->screen = Screen::Library;
    ImGui::SameLine();
    if (ImGui::Button("Clear AOT cache")) {
        AotCache cache(s->app.CacheRoot());
        cache.Clear(s->settings_title_id);
        ShellLog(s, "AOT cache cleared for " + s->settings_title_id);
    }
    ImGui::End();
}

static void DrawAppSettings(ShellState* s) {
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted("Data folder");
    ImGui::TextDisabled("%s", s->config_dir.c_str());
    ImGui::TextUnformatted("Cache root");
    ImGui::TextDisabled("%s", s->app.CacheRoot().c_str());
    ImGui::Separator();
    {
        const char* levels[] = {"Info + warnings + errors (default)",
                                "Warnings + errors only",
                                "Everything including debug lines",
                                "Errors only"};
        int cur = s->app.LogLevel();
        if (cur < 0 || cur > 3) cur = 0;
        if (ImGui::Combo("Log level", &cur, levels, 4)) {
            s->app.SetLogLevel(cur);
            s->app.global.SaveToFile(s->config_dir + "/anyps3.ini");
            SetLogLevel(cur);
            ShellLog(s, std::string("log level set to: ") + levels[cur] +
                                 " — new lines follow the setting immediately");
        }
    }
    ImGui::Separator();
    ImGui::BeginDisabled(s->running);
    if (ImGui::Button("Clear ALL AOT caches")) {
        std::vector<LibraryEntry> games;
        {
            Lock(s);
            games = s->games;
            Unlock(s);
        }
        for (const auto& e : games) {
            AotCache cache(s->app.CacheRoot());
            cache.Clear(e.info.title_id);
        }
        ShellLog(s, "cleared all AOT caches");
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextWrapped("Backend on this host: PPU interpreter. The x86-64 SysV "
                       "AOT backend is enabled on Linux builds; this Windows shell "
                       "uses the interpreter (tested, correct, slower).");
    ImGui::Separator();
    ImGui::TextWrapped(
        "Version %s. Import: ISO9660 + folder dumps (encrypted Redump/3k3y "
        "images with a user-supplied disc key), plain-ELF / fSELF executables. "
        "PPU interpreter, no graphics yet.",
        anyps3::kEngineVersion);
    if (ImGui::Button("Back to library")) s->screen = Screen::Library;
    ImGui::End();
}

static void DrawLog(ShellState* s, bool running, bool boot_finished, const std::string& running_title,
                    int exit_code, const std::string& log) {
    ImGui::Begin("Log / boot output", nullptr,
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    if (ImGui::Button("Export log")) {
        DiagContext ctx{s->exe_dir, s->config_dir, s->app.CacheRoot()};
        std::string path = ExportDiagnostics(ctx, log);
        if (!path.empty()) {
            ShellLog(s, "diagnostics exported: " + path);
        } else {
            ShellLog(s, "diagnostics export FAILED — cannot write next to the exe "
                       "(folder read-only? try running from a writable folder)");
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("writes version + OS/CPU/GPU/Vulkan + per-game status + last 500 lines next to the exe");
    if (running) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "busy: %s", running_title.c_str());
    } else if (boot_finished) {
        ImGui::Text("last run exited: %d", exit_code);
    } else {
        ImGui::TextDisabled("idle");
    }
    ImGui::Separator();
    ImGui::BeginChild("log_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    static std::string shown;
    if (log != shown) {
        shown = log;
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::TextUnformatted(shown.c_str(), shown.c_str() + shown.size());
    ImGui::EndChild();
    ImGui::End();
}

void ShellFrame(ShellState* s) {
    ApplyStyle();

    if (!s->app.FirstRunAccepted()) {
        DrawFirstRun(s);
        return;
    }

    // Snapshot shared state (mutated by workers under log_mu).
    std::vector<LibraryEntry> games;
    bool import_busy = false, running = false, boot_finished = false;
    std::string import_status, running_title;
    int exit_code = -1;
    std::string log;
    {
        std::lock_guard<std::mutex> lock(s->log_mu);
        games = s->games;
        import_busy = s->import_busy;
        running = s->running;
        boot_finished = s->boot_finished;
        import_status = s->import_status;
        running_title = s->running_title;
        exit_code = s->exit_code;
        for (const auto& l : s->log_lines) {
            log += l;
            log += "\n";
        }
    }
    s->running = running;  // UI-thread cache of the flag (guarded reads below)

    // top bar
    ImGui::BeginMainMenuBar();
    if (ImGui::MenuItem("Library", nullptr, s->screen == Screen::Library))
        s->screen = Screen::Library;
    if (ImGui::MenuItem("Settings", nullptr, s->screen == Screen::AppSettings))
        s->screen = Screen::AppSettings;
    if (ImGui::MenuItem("About / limitations")) s->screen = Screen::About;
    if (ImGui::MenuItem(running ? "Stop" : "Quit")) {
        if (running && g_emu) {
            g_emu->Stop();
        } else {
            s->quit = true;
        }
    }
    ImGui::EndMainMenuBar();

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y * 0.62f));
    if (s->screen == Screen::Library) DrawLibrary(s, games, import_busy, import_status);
    if (s->screen == Screen::GameSettings) DrawGameSettings(s);
    if (s->screen == Screen::AppSettings) DrawAppSettings(s);

    if (s->screen == Screen::About) {
        ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(680, 0), ImGuiCond_Always);
        ImGui::Begin("About Any_Ps3", nullptr, ImGuiWindowFlags_NoResize);
        ImGui::TextWrapped(
            "Any_Ps3 is an experimental PS3 launcher based on AOT recompilation "
            "(PPC64 -> IR -> native code, interpreter fallback). This build is the "
            "app shell: real import pipeline, real boot pipeline, honest logs.");
        ImGui::Separator();
        ImGui::BulletText("Works: ISO/folder import, PARAM.SFO+ICON0 metadata, "
                          "library, per-game settings, boot pipeline, hello-world "
                          "demo executed by the PPU interpreter.");
        ImGui::BulletText("Not implemented yet: graphics (RSX/Vulkan), audio, real "
                          "game execution, encrypted retail EBOOTs (no keys).");
        ImGui::BulletText("'Play' never pretends to run a game: it shows the real "
                          "boot log and 'Emulation not implemented yet'.");
        if (ImGui::Button("Close")) s->screen = Screen::Library;
        ImGui::End();
    }

    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y * 0.62f));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y * 0.38f));
    DrawLog(s, running, boot_finished, running_title, exit_code, log);

    // Crash dialog: shown at the top of the stack when the previous session
    // left a crash report behind. Export appends it to a diagnostics file
    // next to the exe; Dismiss deletes it.
    if (!s->crash_report.empty()) {
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                       vp->WorkPos.y + vp->WorkSize.y * 0.45f),
                                ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.72f, vp->WorkSize.y * 0.7f),
                                 ImGuiCond_Appearing);
        // NOTE: ImGui::End() must be called even when Begin() returns false
        // (a skipped End corrupts the window stack and truncates the frame).
        bool open = ImGui::Begin("Previous session crashed", nullptr,
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
        if (open) {
            ImGui::TextWrapped("Any_Ps3 %s did not shut down cleanly last time. "
                               "The crash log recorded by the crash handler:",
                               anyps3::kEngineVersion);
            ImGui::Separator();
            ImGui::BeginChild("crash_scroll", ImVec2(0, -42));
            ImGui::TextUnformatted(s->crash_report.c_str(),
                                    s->crash_report.c_str() + s->crash_report.size());
            ImGui::EndChild();
            if (ImGui::Button("Export crash log")) {
                DiagContext ctx{s->exe_dir, s->config_dir, s->app.CacheRoot()};
                std::string extra = "--- crash report (previous session) ---\n" +
                                    s->crash_report;
                std::string path = ExportDiagnostics(ctx, log, extra);
                if (!path.empty()) ShellLog(s, "crash log exported: " + path);
                else ShellLog(s, "crash log export FAILED (cannot write next to the exe)");
            }
            ImGui::SameLine();
            if (ImGui::Button("Dismiss")) {
                if (!s->crash_report_path.empty())
                    std::remove(s->crash_report_path.c_str());
                s->crash_report.clear();
                s->crash_report_path.clear();
            }
        }
        ImGui::End();
    }
}

}  // namespace win
}  // namespace anyps3
