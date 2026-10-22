#include "anyps3/loader/import.h"

#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "anyps3/fs/iso9660.h"
#include "anyps3/log.h"
#include "anyps3/sfo/sfo.h"

namespace anyps3 {

static bool IsDir(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static bool IsFile(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static bool ReadHostFile(const std::string& path, std::vector<u8>* out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        std::fclose(f);
        return false;
    }
    out->resize(static_cast<size_t>(sz));
    size_t got = out->empty() ? 0 : std::fread(out->data(), 1, out->size(), f);
    std::fclose(f);
    return got == out->size();
}

bool ReadGameFile(const std::string& source_path, const std::string& rel_path,
                  std::vector<u8>* out, std::string* err) {
    std::string path = rel_path;
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (IsDir(source_path)) {
        if (ReadHostFile(source_path + "/" + path, out)) return true;
        *err = "file not found in game folder: " + path;
        return false;
    }
    if (IsFile(source_path)) {
        IsoReader iso;
        if (!iso.Open(source_path)) {
            *err = "cannot open ISO image: " + source_path;
            return false;
        }
        if (iso.ReadFile(path, out)) return true;
        *err = "file not found in ISO: " + path;
        return false;
    }
    *err = "source is neither file nor folder: " + source_path;
    return false;
}

bool InspectGame(const std::string& source_path, GameInfo* out, std::string* err) {
    // 1) Decide the content root ("PS3_GAME" for disc dumps, "" for PSN-style).
    std::string content_root;
    if (IsDir(source_path)) {
        if (IsFile(source_path + "/PS3_GAME/PARAM.SFO")) content_root = "PS3_GAME";
        else if (IsFile(source_path + "/PARAM.SFO")) content_root = "";
        else {
            *err = "folder does not contain PS3_GAME/PARAM.SFO or PARAM.SFO — not a PS3 game dump";
            return false;
        }
    } else if (IsFile(source_path)) {
        IsoReader probe;
        if (!probe.Open(source_path)) {
            *err = "not a readable PS3 ISO (ISO9660 not found — UDF-only images are not "
                   "supported yet; use a folder dump or hybrid ISO)";
            return false;
        }
        if (probe.Find("PS3_GAME/PARAM.SFO")) content_root = "PS3_GAME";
        else if (probe.Find("PARAM.SFO")) content_root = "";
        else {
            *err = "ISO does not contain PS3_GAME/PARAM.SFO or PARAM.SFO";
            return false;
        }
    } else {
        *err = "path does not exist: " + source_path;
        return false;
    }

    // 2) PARAM.SFO
    std::vector<u8> sfo_data;
    std::string sfo_rel = content_root.empty() ? "PARAM.SFO" : content_root + "/PARAM.SFO";
    if (!ReadGameFile(source_path, sfo_rel, &sfo_data, err)) return false;
    ParamSfo sfo;
    if (!sfo.Parse(sfo_data)) {
        *err = "corrupt PARAM.SFO";
        return false;
    }
    out->source_path = source_path;
    out->content_root = content_root;
    out->title = sfo.GetString("TITLE").value_or("(untitled)");
    out->title_id = sfo.GetString("TITLE_ID").value_or("");
    out->category = sfo.GetString("CATEGORY").value_or("");
    out->app_ver = sfo.GetString("APP_VER").value_or("");
    out->system_ver = sfo.GetString("PS3_SYSTEM_VER").value_or("");
    if (out->title_id.empty()) {
        *err = "PARAM.SFO has no TITLE_ID — refusing import";
        return false;
    }
    // 3) ICON0.PNG (optional)
    std::string icon_rel = content_root.empty() ? "ICON0.PNG" : content_root + "/ICON0.PNG";
    std::vector<u8> icon;
    if (ReadGameFile(source_path, icon_rel, &icon, err)) {
        out->icon_png = std::move(icon);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Library index: <base>/games/<TITLE_ID>/{game.txt, icon.png}
// game.txt is a tiny key=value manifest (no external deps, easy to edit by hand).
// ---------------------------------------------------------------------------

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static void WriteManifest(const std::string& path, const GameInfo& g) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "title=%s\n", g.title.c_str());
    std::fprintf(f, "title_id=%s\n", g.title_id.c_str());
    std::fprintf(f, "category=%s\n", g.category.c_str());
    std::fprintf(f, "app_ver=%s\n", g.app_ver.c_str());
    std::fprintf(f, "system_ver=%s\n", g.system_ver.c_str());
    std::fprintf(f, "source_path=%s\n", g.source_path.c_str());
    std::fprintf(f, "content_root=%s\n", g.content_root.c_str());
    std::fclose(f);
}

static bool ReadManifest(const std::string& path, GameInfo* g) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::map<std::string, std::string> kv;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        std::string l = Trim(line);
        if (l.empty()) continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        kv[Trim(l.substr(0, eq))] = Trim(l.substr(eq + 1));
    }
    std::fclose(f);
    g->title = kv["title"];
    g->title_id = kv["title_id"];
    g->category = kv["category"];
    g->app_ver = kv["app_ver"];
    g->system_ver = kv["system_ver"];
    g->source_path = kv["source_path"];
    g->content_root = kv["content_root"];
    return !g->title_id.empty();
}

static void MkDirP(const std::string& path) {
    std::string cur;
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        cur = path.substr(0, pos);
        ::mkdir(cur.c_str(), 0755);
    }
    ::mkdir(path.c_str(), 0755);
}

GameLibrary::GameLibrary(std::string base_dir) : dir_(std::move(base_dir) + "/games") {
    MkDirP(dir_);
}

bool GameLibrary::Import(const std::string& source_path, GameInfo* out, std::string* err) {
    if (!InspectGame(source_path, out, err)) return false;
    std::string gdir = dir_ + "/" + out->title_id;
    MkDirP(gdir);
    WriteManifest(gdir + "/game.txt", *out);
    if (!out->icon_png.empty()) {
        FILE* f = std::fopen((gdir + "/icon.png").c_str(), "wb");
        if (f) {
            std::fwrite(out->icon_png.data(), 1, out->icon_png.size(), f);
            std::fclose(f);
        }
    }
    return true;
}

bool GameLibrary::List(std::vector<LibraryEntry>* out) const {
    out->clear();
    DIR* d = ::opendir(dir_.c_str());
    if (!d) return false;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        std::string manifest = dir_ + "/" + name + "/game.txt";
        GameInfo g;
        if (!ReadManifest(manifest, &g)) continue;
        LibraryEntry entry;
        entry.info = g;
        entry.dir = dir_ + "/" + name;
        std::vector<u8> icon;
        if (ReadHostFile(entry.dir + "/icon.png", &icon)) entry.info.icon_png = icon;
        out->push_back(std::move(entry));
    }
    ::closedir(d);
    return true;
}

bool GameLibrary::Remove(const std::string& title_id, std::string* err) {
    std::string gdir = dir_ + "/" + title_id;
    std::string manifest = gdir + "/game.txt";
    if (!IsFile(manifest)) {
        *err = "game not in library: " + title_id;
        return false;
    }
    std::remove((gdir + "/game.txt").c_str());
    std::remove((gdir + "/icon.png").c_str());
    std::remove((gdir + "/aot").c_str());
    ::rmdir(gdir.c_str());
    return true;
}

}  // namespace anyps3
