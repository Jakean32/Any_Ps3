// Any_Ps3 Windows shell — diagnostics implementation. See diagnostics.h.
//
// System facts are read from the OS only: RtlGetVersion (OS build), __cpuid
// (CPU brand), GetSystemInfo/GlobalMemoryStatusEx (cores/RAM), DXGI (GPU
// adapter names), and — when a Vulkan loader is installed — the Vulkan API
// (instance extensions + physical device names) through manually typed
// function pointers (no Vulkan SDK required to build).
#include "diagnostics.h"

#include <cstdio>
#include <ctime>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>

#include <intrin.h>  // __cpuid

#include "anyps3/loader/import.h"
#include "anyps3/log.h"
#include "anyps3/runtime/emulator.h"
#include "anyps3/types.h"

namespace anyps3 {
namespace win {

// ---------------------------------------------------------------------------
// System info fragments
// ---------------------------------------------------------------------------

// Timestamp helper: localtime_s on Windows (no localtime_r in MinGW headers).
static void FormatLocalTime(char* out, size_t cap, const char* fmt) {
    time_t now = std::time(nullptr);
    tm local = {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::snprintf(out, cap, fmt, local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
}

static std::string OsVersionLine() {
    // RtlGetVersion never lies (GetVersionEx is manifest-blank on Win8.1+).
    typedef LONG (WINAPI *RtlGetVersionFn)(void*);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return "Windows (version unknown — ntdll not loaded)";
    auto fn = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
    if (!fn) return "Windows (version unknown — RtlGetVersion missing)";
    struct {
        ULONG dwOSVersionInfoSize;
        ULONG dwMajorVersion;
        ULONG dwMinorVersion;
        ULONG dwBuildNumber;
        ULONG dwPlatformId;
        wchar_t szCSDVersion[128];
    } vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(&vi) != 0) return "Windows (version unknown — RtlGetVersion failed)";
    char sz[64] = {0};
    WideCharToMultiByte(CP_UTF8, 0, vi.szCSDVersion, -1, sz, sizeof(sz), nullptr, nullptr);
    std::string out = "Windows " + std::to_string(vi.dwMajorVersion) + "." +
                      std::to_string(vi.dwMinorVersion) + " (build " +
                      std::to_string(vi.dwBuildNumber) + ")";
    if (sz[0]) out += std::string(" ") + sz;
    return out;
}

static std::string CpuLine() {
    int brand[12] = {0};
    __cpuid(brand, 0x80000002);
    __cpuid(brand + 4, 0x80000003);
    __cpuid(brand + 8, 0x80000004);
    char text[49] = {0};
    std::memcpy(text, brand, sizeof(brand));
    std::string cpu(text);
    // trim leading spaces of the brand string
    size_t a = cpu.find_first_not_of(' ');
    if (a != std::string::npos) cpu = cpu.substr(a);
    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    std::string ram = "unknown";
    if (GlobalMemoryStatusEx(&ms)) {
        ram = std::to_string(ms.ullTotalPhys / (1024ULL * 1024ULL * 1024ULL)) + " GiB";
    }
    return "CPU: " + (cpu.empty() ? "unknown" : cpu) + " | logical cores: " +
           std::to_string(si.dwNumberOfProcessors) + " | RAM: " + ram;
}

static std::string WideToUtf8(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::string GpuLine() {
    // Real adapter names through DXGI (what the D3D11 device actually uses).
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return "GPU: DXGI unavailable";
    std::string out = "GPU (DXGI adapters):";
    UINT i = 0;
    for (IDXGIAdapter1* adapter = nullptr;
         factory->EnumAdapters1(i++, &adapter) != DXGI_ERROR_NOT_FOUND;) {
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            out += "\n  - " + WideToUtf8(desc.Description) + " | dedicated video memory: " +
                   std::to_string(desc.DedicatedVideoMemory / (1024ULL * 1024ULL)) + " MiB";
        }
        adapter->Release();
        if (i > 8) break;  // cap the list
    }
    factory->Release();
    return out;
}

// Minimal Vulkan introspection through the official loader, if installed.
static std::string VulkanLine() {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) return "Vulkan: loader (vulkan-1.dll) not installed on this system";

    // Root-level export; everything else comes through it.
    typedef void* (*PFN_vkGetInstanceProcAddr)(void*, const char*);
    auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(vk, "vkGetInstanceProcAddr")));
    if (!getInstanceProcAddr) {
        FreeLibrary(vk);
        return "Vulkan: loader present but vkGetInstanceProcAddr missing";
    }

    typedef int32_t (*PFN_vkEnumerateInstanceExtensionProperties)(const char*, uint32_t*, void*);
    typedef int32_t (*PFN_vkCreateInstance)(const void*, const void*, void**);
    typedef void (*PFN_vkDestroyInstance)(void*, const void*);
    typedef int32_t (*PFN_vkEnumeratePhysicalDevices)(void*, uint32_t*, void*);
    typedef void (*PFN_vkGetPhysicalDeviceProperties)(void*, void*);

    auto enumerateExt = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
        getInstanceProcAddr(nullptr, "vkEnumerateInstanceExtensionProperties"));
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(
        getInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!enumerateExt || !createInstance) {
        FreeLibrary(vk);
        return "Vulkan: loader present but core entry points missing";
    }

    // Instance extensions (no instance needed yet).
    struct VkExtensionProperties { char name[256]; uint32_t specVersion; };
    uint32_t extCount = 0;
    std::string out = "Vulkan: loader present";
    if (enumerateExt(nullptr, &extCount, nullptr) == 0 && extCount > 0) {
        out += " | " + std::to_string(extCount) + " instance extensions";
    }

    // Physical devices (needs a throwaway instance).
    // VkInstanceCreateInfo minimal layout (VKStructureType 1).
    struct CInfo {
        int32_t sType;                 // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
        const void* pNext;
        uint32_t flags;
        const void* pApplicationInfo;  // null is legal
        uint32_t enabledLayerCount;
        const char* const* ppEnabledLayerNames;
        uint32_t enabledExtensionCount;
        const char* const* ppEnabledExtensionNames;
    };
    CInfo ci = {};
    ci.sType = 1;
    void* inst = nullptr;
    if (createInstance(&ci, nullptr, &inst) == 0 && inst) {
        auto enumPhys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            getInstanceProcAddr(inst, "vkEnumeratePhysicalDevices"));
        auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            getInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties"));
        auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(
            getInstanceProcAddr(inst, "vkDestroyInstance"));
        if (enumPhys && getProps) {
            uint32_t n = 0;
            if (enumPhys(inst, &n, nullptr) == 0 && n > 0) {
                std::vector<void*> devices(n);
                if (enumPhys(inst, &n, devices.data()) == 0) {
                    // VkPhysicalDeviceProperties: apiVersion, driverVersion,
                    // vendorID, deviceID, (enum), deviceName[256], ...
                    struct PDevProps {
                        uint32_t apiVersion, driverVersion, vendorID, deviceID;
                        int32_t deviceType;
                        char deviceName[256];
                        uint8_t pipelineCacheUUID[16];
                        uint32_t limits[106];
                    } props = {};
                    out += " | " + std::to_string(n) + " physical device(s):";
                    for (uint32_t i = 0; i < n && i < 8; i++) {
                        std::memset(&props, 0, sizeof(props));
                        getProps(devices[i], &props);
                        out += std::string("\n  - ") + props.deviceName +
                               " (API " + std::to_string(props.apiVersion >> 22) + "." +
                               std::to_string((props.apiVersion >> 12) & 0x3FF) + ")";
                    }
                }
            }
        }
        if (destroy) destroy(inst, nullptr);
    }
    FreeLibrary(vk);
    return out;
}

// ---------------------------------------------------------------------------
// Per-game status block
// ---------------------------------------------------------------------------

static std::string EncStateName(EncState s) {
    switch (s) {
        case EncState::None: return "plain";
        case EncState::NeedsKey: return "Encrypted - disc key needed";
        case EncState::KeyInUse: return "Encrypted (decryption active)";
    }
    return "?";
}

static std::string GamesBlock(const DiagContext& ctx) {
    GameLibrary lib(ctx.config_dir);
    std::vector<LibraryEntry> games;
    lib.List(&games);
    if (games.empty()) return "Games: library is empty";
    AotCache cache(ctx.cache_dir);
    std::string out = "Games (" + std::to_string(games.size()) + "):";
    for (const auto& e : games) {
        out += "\n  - " + e.info.title + " [" + e.info.title_id + "]";
        out += "\n      state: " + EncStateName(e.info.encrypted);
        if (!e.info.meta_source.empty())
            out += " | id from " + e.info.meta_source;
        out += "\n      source: " + e.info.source_path;
        AotCache::Manifest m;
        if (cache.Load(e.info.title_id, &m)) {
            out += "\n      AOT cache: present (" + std::to_string(m.block_count) +
                   " blocks, backend " + std::to_string(m.backend_id) + ")";
        } else {
            out += "\n      AOT cache: none (not compiled yet)";
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Report assembly + export
// ---------------------------------------------------------------------------

static void AppendTail(std::string* out, const std::string& log_text, size_t max_lines) {
    // Keep the LAST `max_lines` lines of the ring.
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= log_text.size()) {
        size_t nl = log_text.find('\n', start);
        if (nl == std::string::npos) {
            if (start < log_text.size()) lines.emplace_back(log_text.substr(start));
            break;
        }
        lines.emplace_back(log_text.substr(start, nl - start));
        start = nl + 1;
    }
    if (lines.size() > max_lines)
        lines.erase(lines.begin(), lines.end() - max_lines);
    for (const auto& l : lines) {
        *out += l;
        *out += "\n";
    }
}

std::string BuildDiagnosticsReport(const DiagContext& ctx, const std::string& log_text,
                                   const std::string& extra) {
    char ts[32] = {0};
    FormatLocalTime(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d");

    std::string out;
    out += "Any_Ps3 diagnostics report\n";
    out += "==========================\n";
    out += std::string("Generated: ") + ts + "\n";
    out += std::string("App version: ") + kEngineVersion + "\n";
    out += "Host: Windows x64 (Win32 + D3D11 shell; PPU interpreter)\n";
    out += "\n[System]\n";
    out += OsVersionLine() + "\n";
    out += CpuLine() + "\n";
    out += GpuLine() + "\n";
    out += VulkanLine() + "\n";
    out += "\n[Paths]\n";
    out += "Config folder: " + ctx.config_dir + "\n";
    out += "Cache folder (AOT): " + ctx.cache_dir + "\n";
    out += "\n[Games]\n";
    out += GamesBlock(ctx) + "\n";
    if (!extra.empty()) {
        out += "\n[Extra]\n";
        out += extra;
        if (extra.back() != '\n') out += "\n";
    }
    out += "\n[Log — last 500 lines]\n";
    AppendTail(&out, log_text, 500);
    return out;
}

std::string ExportDiagnostics(const DiagContext& ctx, const std::string& log_text,
                              const std::string& extra) {
    std::string report = BuildDiagnosticsReport(ctx, log_text, extra);
    char stamp[32] = {0};
    FormatLocalTime(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d");
    std::string path = ctx.exe_dir + "/anyps3-diagnostics-" + stamp + ".txt";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return "";
    std::fwrite(report.data(), 1, report.size(), f);
    std::fclose(f);
    AP_INFO("diagnostics written: ", path.c_str());
    return path;
}

}  // namespace win
}  // namespace anyps3
