// Any_Ps3 — hello-world demo installer: builds a minimal legal "game folder"
// from the packaged sample ELF and imports it through the standard pipeline.
#include "anyps3/demo/hello.h"

#include <cstdio>
#include <vector>

#include "anyps3/log.h"
#include "anyps3/platform.h"
#include "anyps3/sfo/sfo.h"

namespace anyps3 {
namespace demo {

static bool WriteAllBytes(const std::string& path, const void* data, size_t len) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = len == 0 || std::fwrite(data, 1, len, f) == len;
    std::fclose(f);
    return ok;
}

static bool CopyFile(const std::string& from, const std::string& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(to.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return false;
    }
    std::vector<unsigned char> buf(1 << 16);
    size_t n;
    bool ok = true;
    while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
        if (std::fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    std::fclose(in);
    std::fclose(out);
    return ok;
}

bool InstallHelloDemo(const std::string& config_dir, const std::string& hello_elf_path,
                      GameInfo* out, std::string* err) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        AP_ERROR("demo install failed: ", msg.c_str());
        return false;
    };

    std::string src = config_dir + "/demo-src";
    if (!platform::MakeDirs(src)) return fail("cannot create " + src);

    // EBOOT.BIN: the sample ELF, byte-for-byte. The loader accepts plain
    // big-endian PPC64 ELF executables (SelfStatus::NotSelf).
    if (!CopyFile(hello_elf_path, src + "/EBOOT.BIN"))
        return fail("cannot copy sample ELF from " + hello_elf_path);

    // PARAM.SFO: built with the in-repo SFO writer so the demo flows through
    // exactly the same metadata path as a real game.
    ParamSfoBuilder sfo;
    sfo.SetString("TITLE", "Hello World (Any_Ps3 demo)")
        .SetString("TITLE_ID", "DEMO00001")
        .SetString("CATEGORY", "GD")
        .SetString("APP_VER", "01.00")
        .SetString("PS3_SYSTEM_VER", "04.910")
        .SetInt("PARENTAL_LEVEL", 1);
    std::vector<u8> sfo_bytes = sfo.Build();
    if (sfo_bytes.empty()) return fail("SFO build produced no bytes");
    if (!WriteAllBytes(src + "/PARAM.SFO", sfo_bytes.data(), sfo_bytes.size()))
        return fail("cannot write PARAM.SFO");

    // Import through the standard pipeline — the demo is a first-class library
    // entry, not a special case.
    GameLibrary lib(config_dir);
    GameInfo info;
    std::string import_err;
    if (!lib.Import(src, &info, &import_err))
        return fail(import_err.empty() ? "import pipeline rejected the demo" : import_err);
    if (info.title_id != kDemoTitleId) return fail("unexpected TITLE_ID: " + info.title_id);

    AP_INFO("demo installed: ", info.title.c_str());
    if (out) *out = std::move(info);
    return true;
}

}  // namespace demo
}  // namespace anyps3
