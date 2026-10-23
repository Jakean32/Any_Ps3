// Any_Ps3 Windows shell — Dear ImGui (Win32 + Direct3D11) with the same
// dark theme as the Android app. Static x64 build with embedded .ico.
//
// Architecture: the SAME C++ core (core/) that powers the Android app is
// linked directly here. No-copy import works on plain filesystem paths.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <cstdio>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// (this ImGui version intentionally ships the handler decl inside '#if 0';
//  applications forward-declare it themselves)
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                            WPARAM wParam, LPARAM lParam);

// stb_image declarations only; the implementation lives in core/thumbs.cpp
#include "../core/stb/stb_image.h"

#include "../core/cache_manifest.h"
#include "../core/game_import.h"
#include "../core/library.h"
#include "../core/recompiler.h"
#include "../core/thumbs.h"
#include "../core/util.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#ifndef WM_APP_LAUNCH
#define WM_APP_LAUNCH 0
#endif

// ---------------------------------------------------------------- globals
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND g_hwnd = nullptr;

struct Tex {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    bool ok = false;
};

static bool make_texture(const unsigned char* rgba, int w, int h, Tex* out);
static bool load_png_resource(const char* path_on_disk, Tex* out);
static void unload(Tex* t) {
    if (t->srv) t->srv->Release();
    t->srv = nullptr;
    t->ok = false;
}

static Tex g_logo;            // full logo (splash + wizard)
static Tex g_logo_small;      // header
static std::string g_exe_dir;

// app state
static const char* kLegalVer = "0.3.0";
enum class Page { Splash, Wizard, Main };
static Page g_page = Page::Splash;
static float g_splash_t = 0.0f;

// wizard state
static int g_wiz_step = 0;
static bool g_wiz_accept = false;
static std::string g_wiz_folder;
static bool g_wiz_warn = false;

static anyps3::AppConfig g_cfg;
static anyps3::Library* g_lib = nullptr;

enum class Nav { Library, Settings, Logs };
static Nav g_nav = Nav::Library;

struct GameCard {
    anyps3::GameRecord rec;
    bool has_manifest;
    anyps3::CacheManifest manifest;
    Tex cover;
    bool cover_loaded = false;
};
static std::vector<GameCard> g_cards;

static int g_selected_card = -1;
static std::string g_ui_log;
static std::vector<std::string> g_log_lines;

// compile worker
static volatile bool g_compiling = false;
static volatile float g_compile_frac = 0;
static std::string g_compile_stage, g_compile_note;
static std::string g_compile_target;

static void log_line(const std::string& s) {
    g_log_lines.push_back(s);
    if (g_log_lines.size() > 2000) g_log_lines.erase(g_log_lines.begin());
}

// ---------------------------------------------------------------- helpers
static std::string exe_dir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p = buf;
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? "." : p.substr(0, s);
}

static std::string default_meta_dir() { return anyps3::path_join(g_exe_dir, "AnyPs3Data"); }
static std::string default_cache_dir() { return anyps3::path_join(g_exe_dir, "AnyPs3Cache"); }

static const char* status_ui(anyps3::CacheStatus s) {
    using namespace anyps3;
    switch (s) {
        case CacheStatus::NotCompiled: return "Not compiled";
        case CacheStatus::Compiling: return "Compiling...";
        case CacheStatus::Ready: return "Ready";
        case CacheStatus::Stale: return "Out of date";
        case CacheStatus::Failed: return "Failed";
    }
    return "?";
}
static ImVec4 status_color(anyps3::CacheStatus s, bool missing) {
    using namespace anyps3;
    if (missing) return ImVec4(0.78f, 0.36f, 0.36f, 1);
    switch (s) {
        case CacheStatus::Ready: return ImVec4(0.37f, 0.75f, 0.47f, 1);
        case CacheStatus::Stale: return ImVec4(0.85f, 0.55f, 0.25f, 1);
        case CacheStatus::Compiling: return ImVec4(0.79f, 0.65f, 0.40f, 1);
        case CacheStatus::Failed: return ImVec4(0.78f, 0.36f, 0.36f, 1);
        default: return ImVec4(0.54f, 0.58f, 0.64f, 1);
    }
}

// Folder picker via IFileDialog (Vista+)
static bool pick_folder(std::string* out) {
    *out = "";
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileDialog* dlg = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR wpath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) {
                    int n = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
                    std::string path(n > 0 ? n - 1 : 0, '\0');
                    if (n > 0)
                        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, &path[0], n, nullptr, nullptr);
                    CoTaskMemFree(wpath);
                    *out = path;
                    ok = !path.empty();
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    CoUninitialize();
    return ok;
}

// Multi-select file picker (ISOs)
static std::vector<std::string> pick_isos() {
    std::vector<std::string> out;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
        COMDLG_FILTERSPEC filters[] = {{L"PS3 disc images (ISO)", L"*.iso"},
                                       {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, filters);
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dlg->GetResults(&items))) {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD i = 0; i < count; i++) {
                    IShellItem* it = nullptr;
                    if (SUCCEEDED(items->GetItemAt(i, &it))) {
                        PWSTR wpath = nullptr;
                        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) {
                            int n = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0,
                                                        nullptr, nullptr);
                            std::string path(n > 0 ? n - 1 : 0, '\0');
                            if (n > 0)
                                WideCharToMultiByte(CP_UTF8, 0, wpath, -1, &path[0], n,
                                                    nullptr, nullptr);
                            CoTaskMemFree(wpath);
                            if (!path.empty()) out.push_back(path);
                        }
                        it->Release();
                    }
                }
                items->Release();
            }
        }
        dlg->Release();
    }
    CoUninitialize();
    return out;
}

// ---------------------------------------------------------------- core glue
static void ensure_library() {
    if (g_lib) return;
    std::string meta = default_meta_dir();
    g_cfg.load(meta);
    if (g_cfg.cache_root.empty()) g_cfg.cache_root = default_cache_dir();
    g_lib = new anyps3::Library(g_cfg);
    g_lib->set_sink(anyps3::PlatformLogSink{[](const std::string& l) { log_line(l); }});
}

static void reload_cards() {
    using namespace anyps3;
    // free covers not reused
    for (auto& c : g_cards)
        if (c.cover_loaded) unload(&c.cover);
    g_cards.clear();
    if (!g_lib) return;
    auto entries = g_lib->entries();
    for (const auto& e : entries) {
        GameCard c;
        c.rec = e.rec;
        c.has_manifest = e.has_manifest;
        c.manifest = e.manifest;
        g_cards.push_back(std::move(c));
    }
    // load cover textures lazily (small cached thumbnails)
    for (auto& c : g_cards) {
        std::string p = c.rec.icon_path.empty() ? std::string() : c.rec.icon_path;
        if (p.empty() && !c.rec.pic1_path.empty()) p = c.rec.pic1_path;
        if (!p.empty() && anyps3::file_exists(p)) {
            c.cover_loaded = load_png_resource(p.c_str(), &c.cover);
        }
    }
}

static void do_refresh() {
    if (!g_lib) return;
    std::vector<std::string> lines;
    g_lib->refresh(&lines, nullptr);
    for (auto& l : lines) log_line(l);
    reload_cards();
}

static void start_compile(const std::string& id, bool recompile) {
    if (g_compiling || !g_lib) return;
    g_compiling = true;
    g_compile_frac = 0;
    g_compile_stage = "starting";
    g_compile_note = "";
    g_compile_target = id;
    std::thread t([id, recompile]() {
        anyps3::ProgressFn cb = [](const anyps3::CompileProgress& p) {
            g_compile_stage = p.stage;
            g_compile_frac = (float)p.fraction;
            g_compile_note = p.note;
        };
        std::string err;
        bool ok = recompile ? g_lib->recompile(id, cb, &err)
                            : g_lib->compile(id, cb, &err);
        log_line((recompile ? "[recompile] " : "[compile] ") + id + (ok ? " finished" : " FAILED")
                 + (err.empty() ? "" : " — " + err));
        g_compiling = false;
    });
    t.detach();
}

// ---------------------------------------------------------------- d3d11
static bool make_texture(const unsigned char* rgba, int w, int h, Tex* out) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {rgba, (UINT)(w * 4), (UINT)(w * h * 4)};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_device->CreateTexture2D(&desc, &init, &tex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = desc.Format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    bool ok = SUCCEEDED(g_device->CreateShaderResourceView(tex, &srvd, &out->srv));
    tex->Release();
    out->w = w;
    out->h = h;
    out->ok = ok;
    return ok;
}

static bool load_png_resource(const char* path_on_disk, Tex* out) {
    int w, h, comp;
    unsigned char* px = stbi_load(path_on_disk, &w, &h, &comp, 4);
    if (!px) return false;
    bool ok = make_texture(px, w, h, out);
    stbi_image_free(px);
    return ok;
}

static void create_swap_rtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return;
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
    return ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp) != 0
               ? 0
               : DefWindowProcW(hWnd, msg, wp, lp);
}

// ---------------------------------------------------------------- theme
static void apply_theme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 8.f;
    st.FrameRounding = 7.f;
    st.GrabRounding = 6.f;
    st.PopupRounding = 10.f;
    st.ScrollbarRounding = 9.f;
    st.WindowBorderSize = 0.f;
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 8);

    ImVec4 bg(0.071f, 0.082f, 0.102f, 1.f);        // #12151A
    ImVec4 surface(0.106f, 0.122f, 0.149f, 1.f);   // #1B1F26
    ImVec4 surface2(0.137f, 0.157f, 0.2f, 1.f);    // #232833
    ImVec4 stroke(0.18f, 0.208f, 0.259f, 1.f);     // #2E3542
    ImVec4 text(0.91f, 0.902f, 0.882f, 1.f);       // #E8E6E1
    ImVec4 text2(0.659f, 0.635f, 0.604f, 1.f);     // #A8A29A
    ImVec4 accent(0.671f, 0.529f, 0.302f, 1.f);    // #AB874D
    ImVec4 accentB(0.788f, 0.647f, 0.4f, 1.f);     // #C9A566

    ImGui::GetStyle().Colors[ImGuiCol_Text] = text;
    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = text2;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = bg;
    ImGui::GetStyle().Colors[ImGuiCol_ChildBg] = surface;
    ImGui::GetStyle().Colors[ImGuiCol_PopupBg] = ImVec4(surface.x, surface.y, surface.z, 0.98f);
    ImGui::GetStyle().Colors[ImGuiCol_Border] = stroke;
    ImGui::GetStyle().Colors[ImGuiCol_FrameBg] = surface2;
    ImGui::GetStyle().Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(surface2.x + 0.03f, surface2.y + 0.03f, surface2.z + 0.03f, 1.f);
    ImGui::GetStyle().Colors[ImGuiCol_TitleBg] = surface;
    ImGui::GetStyle().Colors[ImGuiCol_TitleBgActive] = surface;
    ImGui::GetStyle().Colors[ImGuiCol_Button] = surface2;
    ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] = accent;
    ImGui::GetStyle().Colors[ImGuiCol_ButtonActive] = accentB;
    ImGui::GetStyle().Colors[ImGuiCol_Header] = surface2;
    ImGui::GetStyle().Colors[ImGuiCol_HeaderHovered] = accent;
    ImGui::GetStyle().Colors[ImGuiCol_HeaderActive] = accentB;
    ImGui::GetStyle().Colors[ImGuiCol_CheckMark] = accentB;
    ImGui::GetStyle().Colors[ImGuiCol_SliderGrab] = accentB;
    ImGui::GetStyle().Colors[ImGuiCol_SliderGrabActive] = accentB;
    ImGui::GetStyle().Colors[ImGuiCol_ResizeGrip] = stroke;
    ImGui::GetStyle().Colors[ImGuiCol_Tab] = surface;
    ImGui::GetStyle().Colors[ImGuiCol_TabHovered] = surface2;
    ImGui::GetStyle().Colors[ImGuiCol_TabSelected] = surface2;
    ImGui::GetStyle().Colors[ImGuiCol_Separator] = stroke;
    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

static void draw_nav(bool* open) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vp.x, 52));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 10));
    ImGui::Begin("##header", open,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (g_logo_small.ok) {
        float s = 30.f / g_logo_small.h;
        ImGui::Image((ImTextureID)g_logo_small.srv, ImVec2(g_logo_small.w * s, 30));
        ImGui::SameLine();
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 6);
    ImGui::TextColored(ImVec4(0.91f, 0.9f, 0.88f, 1), "Any_Ps3");
    ImGui::SameLine();
    ImGui::TextDisabled("v%s", anyps3::kVersion);

    // nav tabs right-aligned
    float w = 250.f;
    ImGui::SameLine(ImGui::GetWindowWidth() - w);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    struct NavItem { const char* label; Nav nav; };
    NavItem items[] = {{"Library", Nav::Library}, {"Settings", Nav::Settings},
                       {"Logs", Nav::Logs}};
    for (auto& it : items) {
        bool sel = g_nav == it.nav;
        ImGui::PushStyleColor(ImGuiCol_Text,
                              sel ? ImVec4(0.788f, 0.647f, 0.4f, 1)
                                  : ImVec4(0.42f, 0.404f, 0.373f, 1));
        if (ImGui::Button(it.label, ImVec2(76, 32))) g_nav = it.nav;
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleVar();
}

// page: library grid
static void page_library() {
    ImGui::SetNextWindowPos(ImVec2(0, 52));
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vp.x, vp.y - 52));
    ImGui::Begin("##library", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

    // action row
    if (ImGui::Button("+ Import ISO...")) {
        auto isos = pick_isos();
        int ok = 0;
        for (auto& p : isos) {
            std::string err;
            if (g_lib->import_iso_path(p, &err)) ok++;
            else log_line("import failed: " + p + " — " + err);
        }
        log_line("imported " + std::to_string(ok) + "/" + std::to_string(isos.size()) + " ISO(s)");
        reload_cards();
    }
    ImGui::SameLine();
    if (ImGui::Button("Scan folder...")) {
        std::string dir;
        if (pick_folder(&dir)) {
            std::string err;
            auto st = g_lib->scan_folder(dir, true, &err);
            log_line("scan of " + dir + ": found=" + std::to_string(st.found) + " imported=" +
                     std::to_string(st.imported) + " updated=" + std::to_string(st.updated));
            reload_cards();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) do_refresh();
    ImGui::SameLine();
    if (!g_lib->config().cache_root.empty()) {
        ImGui::TextDisabled("cache: %s", g_lib->config().cache_root.c_str());
    }
    ImGui::Separator();

    if (g_cards.empty()) {
        ImVec2 center(ImGui::GetWindowWidth() * 0.5f, ImGui::GetWindowHeight() * 0.42f);
        ImGui::SetCursorPos(ImVec2(center.x - 60, center.y - 80));
        if (g_logo.ok) {
            float s = 120.f / g_logo.h;
            ImGui::Image((ImTextureID)g_logo.srv, ImVec2(g_logo.w * s, 120));
        }
        ImGui::SetCursorPos(ImVec2(center.x - 80, center.y + 60));
        ImGui::Text("No games yet");
        ImGui::SetCursorPos(ImVec2(center.x - 230, center.y + 88));
        ImGui::TextDisabled(
            "Import PS3 games you own (ISO images or folder dumps). Nothing is copied:");
        ImGui::SetCursorPos(ImVec2(center.x - 230, center.y + 104));
        ImGui::TextDisabled("Any_Ps3 keeps a reference and reads the original files in place.");
    } else {
        // grid of cards
        float card_w = 300.f;
        int cols = (int)(ImGui::GetWindowWidth() / (card_w + 12));
        if (cols < 1) cols = 1;
        int i = 0;
        for (auto& c : g_cards) {
            if (i > 0 && i % cols != 0) ImGui::SameLine();
            ImGui::PushID(i);
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.106f, 0.122f, 0.149f, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
            ImGui::BeginChild("card", ImVec2(card_w, 196), ImGuiChildFlags_Borders);
            // cover 16:9
            ImVec2 cover_size(card_w - 20, (card_w - 20) * 9.f / 16.f);
            if (c.cover_loaded) {
                float scale = cover_size.y / c.cover.h;
                ImVec2 sz(c.cover.w * scale, cover_size.y);
                float clip_x = sz.x - cover_size.x;
                ImGui::Image((ImTextureID)c.cover.srv, cover_size, ImVec2(0, 0),
                             ImVec2(cover_size.x / sz.x, 1));
            } else {
                ImGui::Dummy(cover_size);
            }
            ImGui::TextWrapped("%s", c.rec.title.c_str());
            ImGui::TextDisabled("%s", c.rec.title_id.c_str());
            ImGui::SameLine();
            auto st = c.has_manifest ? c.manifest.status : anyps3::CacheStatus::NotCompiled;
            ImVec4 col = status_color(st, c.rec.missing);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::SameLine(ImGui::GetWindowWidth() - 110);
            ImGui::Text("%s", c.rec.missing ? "File missing" : status_ui(st));
            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            if (ImGui::IsItemClicked()) g_selected_card = i;
            ImGui::PopID();
            i++;
        }
    }
    ImGui::End();

    // detail popup
    if (g_selected_card >= 0 && g_selected_card < (int)g_cards.size()) {
        ImGui::OpenPopup("##detail");
        ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_Appearing);
        bool open = true;
        if (ImGui::BeginPopupModal("##detail", &open, ImGuiWindowFlags_NoTitleBar)) {
            auto& c = g_cards[g_selected_card];
            ImGui::Text("%s", c.rec.title.c_str());
            ImGui::TextDisabled("%s  ·  %s  ·  %s", c.rec.title_id.c_str(),
                                c.rec.app_ver.c_str(),
                                c.rec.src_type == anyps3::SourceType::IsoFile
                                    ? "ISO"
                                    : c.rec.src_type == anyps3::SourceType::SafUri ? "SAF URI"
                                                                                  : "folder");
            ImGui::Separator();
            ImGui::TextWrapped("%s", c.rec.src_ref.c_str());
            ImGui::Separator();
            if (c.rec.missing) {
                ImGui::TextColored(ImVec4(0.78f, 0.36f, 0.36f, 1),
                                   "File missing — reselect the source. The record is kept.");
            }
            if (c.has_manifest) {
                for (auto& s : c.manifest.stages) {
                    const char* mark = "?";
                    ImVec4 col(0.54f, 0.58f, 0.64f, 1);
                    if (s.status == "ok") { mark = "[ok]"; col = ImVec4(0.37f, 0.75f, 0.47f, 1); }
                    else if (s.status == "not_implemented") { mark = "[--]"; col = ImVec4(0.54f, 0.58f, 0.64f, 1); }
                    else if (s.status == "blocked") { mark = "[!!]"; col = ImVec4(0.85f, 0.55f, 0.25f, 1); }
                    else if (s.status == "failed") { mark = "[XX]"; col = ImVec4(0.78f, 0.36f, 0.36f, 1); }
                    ImGui::TextColored(col, "%s %s", mark, s.name.c_str());
                    if (s.status == "not_implemented")
                        ImGui::SameLine(), ImGui::TextDisabled("Compiler stage not implemented yet");
                    else if (!s.note.empty())
                        ImGui::SameLine(), ImGui::TextDisabled("%s", s.note.c_str());
                }
                ImGui::Separator();
                ImGui::TextWrapped("status: %s — %s", status_ui(c.manifest.status),
                                   c.manifest.note.c_str());
            } else {
                ImGui::TextDisabled(
                    "Analysis not run yet — press Compile to scan the game's EBOOT.BIN.");
            }
            ImGui::Separator();
            if (ImGui::Button("Compile", ImVec2(120, 0))) {
                start_compile(c.rec.title_id, false);
                ImGui::CloseCurrentPopup();
                g_selected_card = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Recompile", ImVec2(120, 0))) {
                start_compile(c.rec.title_id, true);
                ImGui::CloseCurrentPopup();
                g_selected_card = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Launch", ImVec2(100, 0))) {
                log_line("[launch] Launch is not available yet — the compiled-artifact "
                         "loader is not implemented.");
            }
            ImGui::Separator();
            if (ImGui::Button("Delete cache", ImVec2(140, 0))) {
                std::string err;
                g_lib->delete_cache(c.rec.title_id, &err);
                log_line("cache deleted for " + c.rec.title_id);
                do_refresh();
                g_selected_card = -1;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.78f, 0.36f, 0.36f, 1));
            if (ImGui::Button("Remove from library", ImVec2(180, 0))) {
                g_lib->remove_game(c.rec.title_id, true, nullptr);
                log_line("removed from library: " + c.rec.title_id);
                do_refresh();
                g_selected_card = -1;
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        if (!open) g_selected_card = -1;
    }
}

// page: settings
static void page_settings() {
    ImGui::SetNextWindowPos(ImVec2(0, 52));
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vp.x, vp.y - 52));
    ImGui::Begin("##settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);

    ImGui::Text("Settings");
    ImGui::Separator();
    uint64_t free = g_lib ? g_lib->cache_free_bytes() : 0;
    ImGui::Text("Free space in cache location: %.1f GiB", free / (1024.f * 1024.f * 1024.f));
    ImGui::TextDisabled("cache root: %s",
                        g_lib ? g_lib->config().cache_root.c_str() : "(not initialized)");
    ImGui::Spacing();
    if (ImGui::Button("Choose cache folder...", ImVec2(240, 0))) {
        std::string dir;
        if (pick_folder(&dir)) {
            std::string root = anyps3::path_join(dir, "AnyPs3_Cache");
            g_lib->set_cache_root(root);
            log_line("cache root set: " + root);
        }
    }
    if (ImGui::Button("Re-run first-run wizard", ImVec2(240, 0))) {
        g_cfg.wizard_done = false;
        g_page = Page::Wizard;
        g_wiz_step = 0;
        g_wiz_accept = false;
        g_wiz_folder.clear();
    }
    if (ImGui::Button("View legal notice", ImVec2(240, 0))) {
        ImGui::OpenPopup("##legal");
    }
    bool legal_open = true;
    if (ImGui::BeginPopupModal("##legal", &legal_open)) {
        ImGui::TextWrapped("%s", anyps3::kLegalNotice);
        ImGui::TextWrapped(
            "You may only import and compile games that you legally own. This project is "
            "not affiliated with Sony and does not bypass copy protection.");
        ImGui::EndPopup();
    }
    ImGui::Separator();
    ImGui::TextWrapped("Executable-code strategy (Windows): compiled AOT artifacts are "
                       "written to the cache folder as ordinary files and would be loaded "
                       "with executable permission. The artifact loader is not implemented "
                       "yet and is reported honestly by the UI.");
    ImGui::Separator();
    ImGui::TextDisabled("Any_Ps3 %s — %s · core manifest format v%d", anyps3::kVersion,
                        anyps3::kCodename, anyps3::kManifestFormatVersion);
    ImGui::End();
}

// page: logs
static void page_logs() {
    ImGui::SetNextWindowPos(ImVec2(0, 52));
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSize(ImVec2(vp.x, vp.y - 52));
    ImGui::Begin("##logs", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    if (ImGui::Button("Clear view")) g_log_lines.clear();
    ImGui::SameLine();
    ImGui::TextDisabled("in-memory activity log (also written to per-game compile logs)");
    ImGui::Separator();
    ImGui::BeginChild("scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (auto& l : g_log_lines) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
    ImGui::End();
}

// (wizard state declared with globals above)
static void page_wizard() {
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(vp.x * 0.5f - 330, vp.y * 0.5f - 240));
    ImGui::SetNextWindowSize(ImVec2(660, 480));
    ImGui::Begin("Any_Ps3 setup", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize);
    if (g_wiz_step == 0) {
        if (g_logo.ok) {
            float s = 130.f / g_logo.h;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - g_logo.w * s) * 0.5f);
            ImGui::Image((ImTextureID)g_logo.srv, ImVec2(g_logo.w * s, 130));
        }
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Any_Ps3 recompiles PS3 games (PPU/SPU) into native code ahead of time and "
            "caches the result per game.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Your games are NEVER copied. Any_Ps3 reads the original files in place — "
            "even after rebooting.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Honest notes: the LLVM code-generation stage is not implemented yet. You can "
            "import, analyze and manage your library today; compilation runs the real "
            "analyzer and reports the rest honestly. Any_Ps3 contains no Sony firmware, "
            "keys or game content.");
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 110);
        if (ImGui::Button("Next##w0", ImVec2(100, 0))) g_wiz_step = 1;
    } else if (g_wiz_step == 1) {
        ImGui::Text("Choose compile & cache folder");
        ImGui::Separator();
        ImGui::TextWrapped(
            "Pick the folder where compiled caches will be stored. You can also import "
            "games from it later.");
        ImGui::Spacing();
        if (ImGui::Button("Choose folder...", ImVec2(180, 0))) {
            if (pick_folder(&g_wiz_folder)) {
                uint64_t free = anyps3::free_space_bytes(g_wiz_folder);
                g_wiz_warn = free < 2ULL * 1024 * 1024 * 1024;
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(g_wiz_folder.c_str());
        if (!g_wiz_folder.empty()) {
            uint64_t free = anyps3::free_space_bytes(g_wiz_folder);
            ImGui::Text("Free space: %.1f GiB", free / (1024.f * 1024.f * 1024.f));
            if (g_wiz_warn)
                ImGui::TextColored(ImVec4(0.78f, 0.36f, 0.36f, 1),
                                   "Low free space — a compile cache needs several GiB for "
                                   "real games.");
            ImGui::TextDisabled("Cache folder: %s",
                                anyps3::path_join(g_wiz_folder, "AnyPs3_Cache").c_str());
        }
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 230);
        if (ImGui::Button("Back##w1", ImVec2(100, 0))) g_wiz_step = 0;
        ImGui::SameLine();
        ImGui::BeginDisabled(g_wiz_folder.empty());
        if (ImGui::Button("Next##w1", ImVec2(100, 0))) g_wiz_step = 2;
        ImGui::EndDisabled();
    } else {
        ImGui::Text("Legal notice");
        ImGui::Separator();
        ImGui::TextWrapped("%s", anyps3::kLegalNotice);
        ImGui::TextWrapped(
            "You may only import and compile games that you legally own. Dumping games "
            "you own may still be restricted in your country — know your local law. This "
            "project is not affiliated with Sony.");
        ImGui::Spacing();
        ImGui::Checkbox("I understand — only games I own", &g_wiz_accept);
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 230);
        if (ImGui::Button("Back##w2", ImVec2(100, 0))) g_wiz_step = 1;
        ImGui::SameLine();
        ImGui::BeginDisabled(!g_wiz_accept);
        if (ImGui::Button("Start using Any_Ps3", ImVec2(120, 0))) {
            // persist config
            std::string root = anyps3::path_join(g_wiz_folder, "AnyPs3_Cache");
            ensure_library();
            g_lib->set_cache_root(root);
            g_cfg.wizard_done = true;
            g_cfg.legal_version_accepted = kLegalVer;
            g_cfg.save();
            log_line("wizard finished; cache root: " + root);
            g_page = Page::Main;
            reload_cards();
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

// splash with logo animation
static void page_splash() {
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(vp);
    ImGui::Begin("##splash", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    float t = g_splash_t;
    float alpha = t < 0.45f ? t / 0.45f : 1.f;
    float scale = 0.86f + 0.14f * (t < 0.9f ? t / 0.9f : 1.f);
    // gentle settle overshoot
    if (t > 0.6f) {
        float k = (t - 0.6f) / 0.4f;
        scale = 1.0f + 0.02f * sinf(3.14159265f * k) * (1.f - k);
    }
    if (g_logo.ok) {
        int side = (int)(260 * scale);
        ImVec2 sz(side, side);
        ImGui::SetCursorPos(ImVec2(vp.x * 0.5f - side * 0.5f, vp.y * 0.5f - side * 0.5f - 20));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::Image((ImTextureID)g_logo.srv, sz);
        ImGui::PopStyleVar();
        if (alpha >= 1.f) {
            ImGui::SetCursorPos(ImVec2(vp.x * 0.5f - 110, vp.y * 0.5f + side * 0.5f + 8));
            ImGui::TextDisabled("AOT-recompiled PS3 launcher");
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- win main
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_exe_dir = exe_dir();

    // window class with embedded icon
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, hInst, nullptr,
                      nullptr, nullptr, nullptr, L"AnyPs3Window", nullptr};
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"Any_Ps3", WS_OVERLAPPEDWINDOW, 100, 100, 1280,
                           800, nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;
    SetWindowPos(g_hwnd, HWND_TOP, 100, 100, 1280, 800,
                 SWP_SHOWWINDOW | SWP_FRAMECHANGED);

    // DX11 init (standard imgui example pattern)
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT flags_create = 0;
    D3D_FEATURE_LEVEL fl;
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags_create, fls, 2, D3D11_SDK_VERSION,
        &sd, &g_swap, &g_device, &fl, &g_ctx);
    if (FAILED(hr)) {
        // fallback: WARP software rasterizer (works everywhere)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                           flags_create, fls, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap, &g_device, &fl, &g_ctx);
    }
    if (FAILED(hr)) return 2;
    create_swap_rtv();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // don't litter
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_ctx);
    apply_theme();

    // load logo textures from beside the exe (fallback: plain rect)
    std::string logo_path = anyps3::path_join(g_exe_dir, "logo_full.png");
    if (!load_png_resource(logo_path.c_str(), &g_logo)) {
        // try assets dir next to exe
        load_png_resource(anyps3::path_join(g_exe_dir, "assets/logo_full.png").c_str(), &g_logo);
    }
    load_png_resource(logo_path.c_str(), &g_logo_small);

    // config + library
    ensure_library();
    if (g_cfg.wizard_done && g_cfg.legal_version_accepted == kLegalVer) {
        g_page = Page::Main;
        reload_cards();
    } else {
        g_page = Page::Wizard;
    }

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (g_page == Page::Splash) {
            g_splash_t += ImGui::GetIO().DeltaTime;
            page_splash();
            if (g_splash_t > 1.35f) {
                g_page = (g_cfg.wizard_done &&
                          g_cfg.legal_version_accepted == kLegalVer)
                             ? Page::Main
                             : Page::Wizard;
                if (g_page == Page::Main) reload_cards();
            }
        } else if (g_page == Page::Wizard) {
            page_wizard();
        } else {
            bool open = true;
            draw_nav(&open);
            if (!open) done = true;
            if (g_nav == Nav::Library) page_library();
            if (g_nav == Nav::Settings) page_settings();
            if (g_nav == Nav::Logs) page_logs();

            // compile progress modal (real numbers only)
            if (g_compiling) {
                ImGui::OpenPopup("##compiling");
                ImGui::SetNextWindowSize(ImVec2(420, 140), ImGuiCond_Appearing);
                if (ImGui::BeginPopupModal("##compiling", nullptr,
                                           ImGuiWindowFlags_NoTitleBar)) {
                    ImGui::Text("Compiling %s", g_compile_target.c_str());
                    ImGui::ProgressBar(g_compile_frac, ImVec2(-1, 0),
                                       (g_compile_stage + " — " + g_compile_note).c_str());
                    ImGui::TextDisabled("real progress only — stages not implemented are "
                                        "reported, never faked");
                    ImGui::EndPopup();
                }
            }
        }

        // render
        const float clear[4] = {0.071f, 0.082f, 0.102f, 1.f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    // cleanup
    unload(&g_logo);
    unload(&g_logo_small);
    for (auto& c : g_cards)
        if (c.cover_loaded) unload(&c.cover);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_device) g_device->Release();
    DestroyWindow(g_hwnd);
    UnregisterClassW(L"AnyPs3Window", hInst);
    return 0;
}
