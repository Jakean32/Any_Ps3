// Any_Ps3 Windows shell — window/main loop (Win32 + D3D11 + Dear ImGui).
// The app is a single statically linked executable: the only DLLs it needs are
// Windows system components (d3d11.dll, d3dcompiler_47.dll, user32...).
//
// Modes:
//   Any_Ps3.exe            -- the app (dark-theme launcher shell)
//   Any_Ps3.exe --selftest -- headless: install demo, boot, run, print the boot
//                             log, exit 0 only if the guest exited with code 42.
//   ANYPS3_GUI_FRAMES=N    -- render N frames then quit (automated UI smoke
//                             test; used under Wine/Xvfb).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>  // IFileOpenDialog (Vista+; MinGW-w64 ships it)

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <d3d11.h>
#include <d3dcompiler.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "anyps3/demo/hello.h"
#include "anyps3/loader/elf.h"
#include "anyps3/loader/import.h"
#include "anyps3/log.h"
#include "anyps3/platform.h"
#include "anyps3/runtime/emulator.h"

#include "shell.h"

#include "diagnostics.h"

// ----------------------------------------------------------------------------
static HWND g_hwnd = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_SIZE: {
            if (g_device && wp != SIZE_MINIMIZED) {
                if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
                g_swap->ResizeBuffers(0, LOWORD(lp) == 0 ? 1 : LOWORD(lp),
                                      HIWORD(lp) == 0 ? 1 : HIWORD(lp),
                                      DXGI_FORMAT_UNKNOWN, 0);
                ID3D11Texture2D* back = nullptr;
                g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
                if (back) {
                    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
                    back->Release();
                }
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return;
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
}

static bool InitD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;  // NOT D3D11_CREATE_DEVICE_SINGLETHREADED: workers exist
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &fl, &g_ctx);
    if (FAILED(hr))  // fall back to WARP (software rasterizer)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                           flags & ~D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_device,
                                           &fl, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupD3D() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

// ----------------------------------------------------------------------------
// Paths: portable app layout next to the exe.
// ----------------------------------------------------------------------------
static std::string ExeDir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf, n);
    size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

static std::string PickConfigDir(const std::string& exe_dir) {
    std::string data = exe_dir + "/data";
    if (anyps3::platform::MakeDirs(data)) return data;
    // read-only install location -> per-user app data
    char* appdata = getenv("LOCALAPPDATA");
    if (appdata && anyps3::platform::MakeDirs(std::string(appdata) + "/Any_Ps3"))
        return std::string(appdata) + "/Any_Ps3";
    return data;
}

// ----------------------------------------------------------------------------
// Crash handler (TASK 4): writes <config>/crash_last.txt on an unhandled
// exception (or a fatal signal) so the next start can show + export it.
// The addresses are raw (no symbols are shipped) — an honest report, not a
// full minidump.
// ----------------------------------------------------------------------------
static void WriteCrashReport(const char* origin, EXCEPTION_POINTERS* ep, int sig) {
    std::string cfg = PickConfigDir(ExeDir());
    FILE* f = std::fopen((cfg + "/crash_last.txt").c_str(), "wb");
    if (!f) return;
    char ts[32] = {0};
    time_t now = std::time(nullptr);
    tm local = {};
    localtime_s(&local, &now);  // MinGW has no localtime_r
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    std::fprintf(f, "Any_Ps3 %s crash report\n", anyps3::kEngineVersion);
    std::fprintf(f, "time: %s\n", ts);
    std::fprintf(f, "origin: %s\n", origin);
    if (ep && ep->ExceptionRecord) {
        std::fprintf(f, "exception code: 0x%08lX at address 0x%p\n",
                     static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                     ep->ExceptionRecord->ExceptionAddress);
    } else {
        std::fprintf(f, "fatal signal: %d\n", sig);
    }
    // Raw stack frames (module-relative debugging only; no symbols shipped).
    void* frames[32] = {0};
    USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
    HMODULE mod = GetModuleHandleA(nullptr);
    std::fprintf(f, "module base: %p, stack frames (raw addresses):\n",
                 reinterpret_cast<void*>(mod));
    for (USHORT i = 0; i < n; i++) {
        std::fprintf(f, "  [%02d] %p (image+%lld)\n", i, frames[i],
                     static_cast<long long>(
                         reinterpret_cast<char*>(frames[i]) -
                         reinterpret_cast<char*>(mod)));
    }
    std::fprintf(f,
        "note: raw addresses, no symbols are shipped. Attach the exported "
        "diagnostics (next start) for version/OS/CPU/GPU context.\n");
    std::fclose(f);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    WriteCrashReport("unhandled exception", ep, 0);
    return EXCEPTION_EXECUTE_HANDLER;  // let the process die honestly
}

static void CrashSignalHandler(int sig) {
    WriteCrashReport("fatal signal", nullptr, sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void InstallCrashHandler() {
    SetUnhandledExceptionFilter(CrashFilter);
    signal(SIGSEGV, &CrashSignalHandler);
    signal(SIGABRT, &CrashSignalHandler);
    signal(SIGFPE, &CrashSignalHandler);
}

// ----------------------------------------------------------------------------
// Win32 file/folder picker (IFileOpenDialog).
// ----------------------------------------------------------------------------
static std::wstring Utf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
static std::string WideToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

namespace anyps3 { namespace win {
void WinOpenKeyFileDialog(ShellState* s) {
    // Picks a user-owned .dkey / .key file for the game being edited.
    // Any_Ps3 ships no keys; this only reads a file the user provides.
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) return;
    dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    COMDLG_FILTERSPEC filters[] = {
        {L"PS3 disc keys", L"*.dkey;*.key;*.bin;*.txt"},
        {L"All files", L"*.*"},
    };
    dlg->SetFileTypes(2, filters);
    dlg->SetTitle(L"Select the disc key (.dkey / .key) for this game");
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                s->pending_dkey_path = WideToUtf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

void WinOpenEbootDialog(ShellState* s) {
    // Picks the user's own DECRYPTED executable (plain ELF64 or fSELF) for the
    // game being edited. Retail EBOOT.BIN is encrypted; Any_Ps3 ships no
    // keys and never decrypts it — the user supplies this file from their own
    // legal dump, produced with their own tools.
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) return;
    dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    COMDLG_FILTERSPEC filters[] = {
        {L"Decrypted PS3 executables", L"*.elf;*.bin;*.self"},
        {L"All files", L"*.*"},
    };
    dlg->SetFileTypes(2, filters);
    dlg->SetTitle(L"Select your decrypted executable (plain ELF / fSELF) for this game");
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                s->pending_eboot_path = WideToUtf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

void WinOpenFileDialog(ShellState* s) {
    // COM file dialog: folders first (PS3 dumps), then ISO filter.
    // Plain Win32 GetOpenFileNameA cannot pick folders, so we use the Vista+
    // IFileOpenDialog via shell COM. Called on the UI thread from DrawLibrary.
    static bool pick_folder = false;
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) return;
    dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                    (pick_folder ? FOS_PICKFOLDERS : 0u));
    if (!pick_folder) {
        COMDLG_FILTERSPEC filters[] = {
            {L"PS3 game sources (ISO, EBOOT)", L"*.iso;*.bin;*.self;*.elf"},
            {L"All files", L"*.*"},
        };
        dlg->SetFileTypes(2, filters);
    }
    dlg->SetTitle(pick_folder ? L"Select a PS3 game folder (folder dump)"
                              : L"Select a PS3 game (ISO image or EBOOT)");
    if (SUCCEEDED(dlg->Show(nullptr))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                s->pending_import_path = WideToUtf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    pick_folder = !pick_folder;  // next click picks the other source kind
}
} }  // namespace anyps3::win

// ----------------------------------------------------------------------------
// Headless self-test: exercises the real demo pipeline without a window.
// Output goes to inherited stdout (when redirected / CI) AND to selftest.log
// next to the executable, because a GUI-subsystem process has no console by
// default (this makes the test verifiable under Wine and on real Windows).
// ----------------------------------------------------------------------------
static int RunSelfTest(const std::string& exe_dir) {
    std::string cfg = exe_dir + "/data";
    anyps3::platform::MakeDirs(cfg);
    FILE* logf = std::fopen((exe_dir + "/selftest.log").c_str(), "wb");

    std::vector<std::string> lines;
    auto emit = [&](const char* tag, const std::string& msg) {
        std::printf("[%s] %s\n", tag, msg.c_str());
        if (logf) std::fprintf(logf, "[%s] %s\n", tag, msg.c_str());
    };

    anyps3::SetLogSink([&](anyps3::LogLevel, const std::string& msg) {
        lines.push_back(msg);
        emit("core", msg);
    });

    anyps3::AppConfig app{cfg};
    app.global.LoadFromFile(cfg + "/anyps3.ini");
    app.AcceptFirstRun();
    app.global.SaveToFile(cfg + "/anyps3.ini");

    std::string err;
    std::string sample = exe_dir + "/samples/hello.elf";
    if (!anyps3::demo::InstallHelloDemo(cfg, sample, nullptr, &err)) {
        emit("selftest", "FAIL: demo install: " + err);
        if (logf) std::fclose(logf);
        return 2;
    }

    anyps3::Emulator emu(app);
    anyps3::EmulatorCallbacks cb;
    cb.log = [&](const std::string& l) {
        lines.push_back(l);
        emit("boot", l);
    };
    anyps3::GameSettings gs;  // Emulator forces interpreter on this host

    // Diagnostic: dump what the loader parsed (helps catch host-specific
    // file handling under Wine; also proves the demo EBOOT bytes).
    {
        anyps3::GameLibrary lib(cfg);
        std::vector<anyps3::LibraryEntry> entries;
        lib.List(&entries);
        for (const auto& e : entries) {
            if (e.info.title_id != anyps3::demo::kDemoTitleId) continue;
            std::vector<unsigned char> eboot;
            std::string rerr;
            if (anyps3::ReadGameFile(e.info.source_path, "EBOOT.BIN", &eboot, &rerr)) {
                anyps3::ElfImage img;
                anyps3::SelfInfo si;
                std::string lerr;
                if (anyps3::LoadExecutable(eboot, e.info.title_id, &img, &si, &lerr)) {
                    for (const auto& seg : img.segments) {
                        emit("elf", "seg vaddr=0x" + std::to_string(seg.vaddr) +
                                        " filesz=" + std::to_string(seg.file_size) +
                                        " memsz=" + std::to_string(seg.mem_size));
                    }
                    emit("elf", "entry=0x" + std::to_string(img.entry) +
                                    " segments=" + std::to_string(img.segments.size()));
                } else {
                    emit("elf", "LoadExecutable failed: " + lerr);
                }
            } else {
                emit("elf", "EBOOT read failed: " + rerr);
            }
        }
    }

    anyps3::BootResult r = emu.Boot(anyps3::demo::kDemoTitleId, gs, cb);
    if (!r.ok) {
        for (const auto& n : r.notes) emit("boot", n);  // failed boot: show notes too
        emit("selftest", "FAIL: boot: " + r.error);
        if (logf) std::fclose(logf);
        return 3;
    }
    int code = emu.Run();

    std::string all;
    for (const auto& l : lines) all += l + "\n";
    if (code != 42 || all.find("Hello from Any_Ps3!") == std::string::npos) {
        emit("selftest", "FAIL: exit code " + std::to_string(code));
        if (logf) std::fclose(logf);
        return 4;
    }
    emit("selftest", "OK: guest exited 42, TTY reached, boot log complete");
    if (logf) std::fclose(logf);
    return 0;
}

// ----------------------------------------------------------------------------
// Headless diagnostics test: exercises exactly what the "Export log" button
// does (system info + games + last-500-lines report written next to the exe).
// Verifiable under Wine / on any machine without touching the GUI.
// ----------------------------------------------------------------------------
static int RunDiagTest(const std::string& exe_dir) {
    std::string cfg = PickConfigDir(exe_dir);
    anyps3::platform::MakeDirs(cfg);
    anyps3::win::DiagContext ctx{exe_dir, cfg, cfg + "/cache"};
    std::string log_text;
    for (int i = 1; i <= 520; i++) {
        log_text += "diag-test log line " + std::to_string(i) + "\n";
    }
    std::string path = anyps3::win::ExportDiagnostics(ctx, log_text);
    if (path.empty()) {
        std::printf("DIAG-TEST FAILED: cannot write the report next to the exe\n");
        return 5;
    }
    // verify the tail trimming (only the last 500 lines belong in the report)
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        std::printf("DIAG-TEST FAILED: report unreadable\n");
        return 6;
    }
    std::string content;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
    std::fclose(f);
    bool has_last = content.find("diag-test log line 520") != std::string::npos;
    bool dropped_first = content.find("diag-test log line 1\n") == std::string::npos;
    bool has_sections = content.find("[System]") != std::string::npos &&
                        content.find("[Games]") != std::string::npos &&
                        content.find("[Log \u2014 last 500 lines]") != std::string::npos &&
                        content.find("Any_Ps3 ") != std::string::npos;
    if (has_last && dropped_first && has_sections) {
        std::printf("DIAG-TEST OK: %s (last-500 trimming + sections verified)\n", path.c_str());
        return 0;
    }
    std::printf("DIAG-TEST FAILED: report content check "
                "(last=%d dropped=%d sections=%d)\n",
                (int)has_last, (int)dropped_first, (int)has_sections);
    return 7;
}

// ----------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdline, int) {
    std::string exe_dir = ExeDir();

    if (std::strstr(cmdline, "--selftest") != nullptr) return RunSelfTest(exe_dir);
    if (std::strstr(cmdline, "--diag-test") != nullptr) return RunDiagTest(exe_dir);

    // Crash handler goes in BEFORE anything else — the whole point is to catch
    // startup/worker crashes too. (--crash-test triggers a deliberate crash so
    // the handler can be verified under Wine / on any machine.)
    InstallCrashHandler();
    if (std::strstr(cmdline, "--crash-test") != nullptr) {
        OutputDebugStringA("Any_Ps3 --crash-test: deliberate null dereference\n");
        volatile int* p = nullptr;
        *p = 42;  // SEGV -> CrashFilter writes <config>/crash_last.txt
        return 7;  // never reached
    }

    int gui_frames = 0;
    const char* gf = getenv("ANYPS3_GUI_FRAMES");
    if (gf) gui_frames = std::atoi(gf);

    // window class — icon from the embedded resource (the user's app logo).
    HICON app_icon = LoadIconA(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(1));
    if (!app_icon) app_icon = LoadIconA(nullptr, IDI_APPLICATION);
    WNDCLASSEXA wc = {sizeof(WNDCLASSEXA), CS_CLASSDC, WndProc, 0L, 0L,
                      GetModuleHandleA(nullptr), app_icon,
                      LoadCursorA(nullptr, IDC_ARROW), nullptr, nullptr,
                      "AnyPs3Window", nullptr};
    RegisterClassExA(&wc);
    g_hwnd = CreateWindowA("AnyPs3Window", "Any_Ps3",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900,
                           nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        std::fprintf(stderr, "Any_Ps3: CreateWindow failed (GetLastError=%lu)\n", GetLastError());
        return 1;
    }
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);
    if (!InitD3D(g_hwnd)) {
        std::fprintf(stderr, "Any_Ps3: D3D11 device/swap-chain init failed "
                             "(GPU driver or software rasterizer unavailable)\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;  // portable: no imgui.ini litter

    // Font: try Segoe UI from the system, fall back to the embedded default.
    char win_dir[MAX_PATH];
    if (GetWindowsDirectoryA(win_dir, MAX_PATH)) {
        std::string font = std::string(win_dir) + "/Fonts/segoeui.ttf";
        if (anyps3::platform::IsFile(font)) {
            ImFontConfig fc;
            fc.OversampleH = 2;
            io.Fonts->AddFontFromFileTTF(font.c_str(), 17.0f, &fc,
                                         io.Fonts->GetGlyphRangesDefault());
        }
    }

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_ctx);

    anyps3::win::ShellState shell;
    shell.exe_dir = exe_dir;
    shell.config_dir = PickConfigDir(exe_dir);
    shell.samples_dir = exe_dir + "/samples";
    anyps3::win::ShellSetDevice(g_device);
    anyps3::win::ShellInit(&shell);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    bool done = false;
    int frames = 0;
    while (!done) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (shell.quit) done = true;
        if (gui_frames > 0 && ++frames >= gui_frames) done = true;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        anyps3::win::ShellFrame(&shell);
        ImGui::Render();

        const float clear[4] = {0.063f, 0.063f, 0.078f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);  // vsync
    }

    anyps3::win::ShellShutdown(&shell);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupD3D();
    CoUninitialize();
    DestroyWindow(g_hwnd);
    UnregisterClassA("AnyPs3Window", wc.hInstance);
    return 0;
}
