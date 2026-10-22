#include "framework.h"

#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include "anyps3/fs/iso9660.h"
#include "anyps3/loader/import.h"
#include "anyps3/sfo/sfo.h"

using namespace anyps3;

static std::vector<u8> MakeParamSfo(const std::string& title, const std::string& id) {
    ParamSfoBuilder b;
    b.SetString("TITLE", title);
    b.SetString("TITLE_ID", id);
    b.SetString("CATEGORY", "GD");
    b.SetString("APP_VER", "01.00");
    b.SetInt("BOOTABLE", 1);
    return b.Build();
}

static std::vector<u8> MakeFakePng() {
    // 8-byte PNG header + junk: good enough for import (we do not decode images).
    std::vector<u8> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3};
    return png;
}

TEST(iso_roundtrip) {
    IsoWriter w;
    w.AddFile("PS3_GAME/PARAM.SFO", MakeParamSfo("Iso Game", "ISO000001"));
    w.AddFile("PS3_GAME/ICON0.PNG", MakeFakePng());
    w.AddFile("PS3_GAME/USRDIR/A.BIN", {1, 2, 3, 4, 5, 6, 7, 8});
    std::vector<u8> img = w.Build();

    IsoReader r;
    CHECK_TRUE(r.Open(img));

    auto sfo = r.Find("PS3_GAME/PARAM.SFO");
    CHECK_TRUE(sfo.has_value());
    CHECK_TRUE(!sfo->is_dir);

    std::vector<u8> data;
    CHECK_TRUE(r.ReadFile("/ps3_game/param.sfo", &data));  // case-insensitive + slashes
    ParamSfo parsed;
    CHECK_TRUE(parsed.Parse(data));
    CHECK_STR_EQ(parsed.GetString("TITLE_ID").value_or(""), "ISO000001");

    CHECK_TRUE(r.ReadFile("PS3_GAME/USRDIR/A.BIN", &data));
    CHECK_EQ(data.size(), 8);
    CHECK_EQ(data[7], 8);

    auto missing = r.Find("PS3_GAME/NOPE.BIN");
    CHECK_TRUE(!missing.has_value());

    std::vector<IsoFileEntry> entries;
    CHECK_TRUE(r.ListDir("PS3_GAME", &entries));
    // PARAM.SFO, ICON0.PNG, USRDIR
    CHECK_EQ(entries.size(), 3);
}

TEST(iso_rejects_non_iso) {
    IsoReader r;
    CHECK_TRUE(!r.Open(std::vector<u8>(2048 * 20, 0xFF)));
}

TEST(import_iso) {
    IsoWriter w;
    w.AddFile("PS3_GAME/PARAM.SFO", MakeParamSfo("Imported Game", "IMP000001"));
    w.AddFile("PS3_GAME/ICON0.PNG", MakeFakePng());
    w.AddFile("PS3_GAME/USRDIR/EBOOT.BIN", {0xEF, 0xBE, 0xBE, 0xEF});
    std::vector<u8> img = w.Build();

    char tmpl[] = "/tmp/anyps3_iso_XXXXXX";
    int fd = mkstemp(tmpl);
    CHECK_TRUE(fd >= 0);
    FILE* f = fdopen(fd, "wb");
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    std::string iso_path = tmpl;

    GameInfo info;
    std::string err;
    CHECK_TRUE(InspectGame(iso_path, &info, &err));
    CHECK_STR_EQ(info.title, "Imported Game");
    CHECK_STR_EQ(info.title_id, "IMP000001");
    CHECK_STR_EQ(info.category, "GD");
    CHECK_STR_EQ(info.content_root, "PS3_GAME");
    CHECK_EQ(info.icon_png.size(), 11);

    // library import round-trip
    GameLibrary lib("/tmp/anyps3_lib_test");
    GameInfo imported;
    CHECK_TRUE(lib.Import(iso_path, &imported, &err));
    std::vector<LibraryEntry> entries;
    CHECK_TRUE(lib.List(&entries));
    CHECK_EQ(entries.size(), 1);
    CHECK_STR_EQ(entries[0].info.title_id, "IMP000001");
    CHECK_TRUE(lib.Remove("IMP000001", &err));
    CHECK_TRUE(lib.List(&entries));
    CHECK_EQ(entries.size(), 0);

    std::remove(iso_path.c_str());
}

TEST(import_folder) {
    std::string dir = "/tmp/anyps3_game_folder";
    std::string gd = dir + "/PS3_GAME";
    std::remove((gd + "/PARAM.SFO").c_str());
    std::remove((gd + "/ICON0.PNG").c_str());
    std::remove((gd + "/settings.ini").c_str());
    ::rmdir(gd.c_str());
    ::rmdir(dir.c_str());
    CHECK_TRUE(::mkdir(dir.c_str(), 0755) == 0);
    CHECK_TRUE(::mkdir(gd.c_str(), 0755) == 0);

    std::vector<u8> sfo = MakeParamSfo("Folder Game", "DIR000001");
    FILE* f = std::fopen((gd + "/PARAM.SFO").c_str(), "wb");
    std::fwrite(sfo.data(), 1, sfo.size(), f);
    std::fclose(f);

    GameInfo info;
    std::string err;
    CHECK_TRUE(InspectGame(dir, &info, &err));
    CHECK_STR_EQ(info.title, "Folder Game");
    CHECK_STR_EQ(info.title_id, "DIR000001");

    std::remove((gd + "/PARAM.SFO").c_str());
    ::rmdir(gd.c_str());
    ::rmdir(dir.c_str());
}

TEST(import_rejects_bad) {
    GameInfo info;
    std::string err;
    // empty dir
    std::string dir = "/tmp/anyps3_empty_dir";
    ::mkdir(dir.c_str(), 0755);
    CHECK_TRUE(!InspectGame(dir, &info, &err));
    CHECK_TRUE(!err.empty());
}
