// Any_Ps3 — RSX: null backend + command processor + shader cache.
#include "anyps3/gfx/rsx.h"

#include <cstdio>
#include <sys/stat.h>
#include <dirent.h>

#include "anyps3/log.h"

namespace anyps3 {

// ---------------------------------------------------------------------------
class NullRsxBackend : public RsxBackend {
public:
    std::string name() const override { return "null"; }
    bool Initialize(void*) override {
        initialized_ = true;
        return true;
    }
    void Present() override { presents_++; }
    void Shutdown() override { initialized_ = false; }
    u64 presents() const { return presents_; }

private:
    bool initialized_ = false;
    u64 presents_ = 0;
};

RsxBackend* CreateNullBackend() { return new NullRsxBackend(); }

#if defined(ANYPS3_WITH_VULKAN)
// Vulkan skeleton: device/swapchain enumeration lives here; actual GCM draw
// translation is future work (honest status in PHASE_REPORTS.md).
class VulkanRsxBackend : public RsxBackend {
public:
    std::string name() const override { return "vulkan"; }
    bool Initialize(void*) override {
        // Device selection, instance creation and surface setup are TODO —
        // returning false keeps the runtime from pretending a display exists.
        return false;
    }
    void Present() override {}
    void Shutdown() override {}
};
RsxBackend* CreateVulkanBackend() { return new VulkanRsxBackend(); }
#endif

// ---------------------------------------------------------------------------
void RsxCommandProcessor::Execute(const u32* cmds, size_t count) {
    for (size_t i = 0; i < count; i++) {
        u32 cmd = ReadBE32(reinterpret_cast<const u8*>(cmds + i));
        u32 op = cmd & 0xFF000000u;
        switch (op) {
            case 0xFF000000u:
                stats_.flips++;
                if (backend_) backend_->Present();
                break;
            case 0x01000000u:
                stats_.draws += cmd & 0xFFFFFF;
                break;
            case 0xF0000000u:
                last_label_ = cmd & 0xFFFFFF;
                break;
            default:
                stats_.unknown_cmds++;
                break;
        }
    }
}

// ---------------------------------------------------------------------------
static void MkDirP(const std::string& path) {
    std::string cur;
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        cur = path.substr(0, pos);
        ::mkdir(cur.c_str(), 0755);
    }
    ::mkdir(path.c_str(), 0755);
}

std::string ShaderCache::PathFor(u64 key) const {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/shaders/%016llx.blob", root_.c_str(),
                  static_cast<unsigned long long>(key));
    return path;
}

bool ShaderCache::Lookup(u64 key, std::vector<u8>* blob) const {
    auto it = index_.find(key);
    if (it != index_.end() && !it->second) return false;  // known-negative
    std::string path = PathFor(key);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        index_[key] = false;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    blob->resize(static_cast<size_t>(sz));
    size_t got = sz ? std::fread(blob->data(), 1, blob->size(), f) : 0;
    std::fclose(f);
    index_[key] = true;
    return got == blob->size();
}

void ShaderCache::Store(u64 key, const void* data, size_t len) {
    std::string path = PathFor(key);
    std::string dir = path.substr(0, path.rfind('/'));
    MkDirP(dir);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    if (len) std::fwrite(data, 1, len, f);
    std::fclose(f);
    index_[key] = true;
}

bool ShaderCache::Invalidate(u64 key) {
    index_.erase(key);
    return std::remove(PathFor(key).c_str()) == 0;
}

size_t ShaderCache::EntryCount() const {
    // Count entries on disk (index may be stale across runs).
    std::string dir = root_ + "/shaders";
    DIR* d = ::opendir(dir.c_str());
    if (!d) return 0;
    size_t n = 0;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string n2 = e->d_name;
        if (n2.size() > 5 && n2.compare(n2.size() - 5, 5, ".blob") == 0) n++;
    }
    ::closedir(d);
    return n;
}

}  // namespace anyps3
