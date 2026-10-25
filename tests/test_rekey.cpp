// TASK-3 tests: the "attach a disc key and re-read" flow end to end.
//
// Everything here uses SELF-GENERATED images (IsoWriter + AES with a test
// key) — no Sony content, no real keys. The tests cover:
//   * importing an encrypted ISO without a key -> "Encrypted - disc key
//     needed" placeholder entry (id UNKNOWN)
//   * attaching the key through the per-game settings (key file path or a
//     typed 32-hex string) and re-reading -> real title/id/category, the
//     entry moves to the real TITLE_ID ("Ready to compile")
//   * the wrong-key path: explicit keys that do not unlock fail honestly
//     and leave the library untouched
//   * disc-key file parsing variants (hex text, raw 16 bytes, colon form,
//     whitespace-padded text, garbage)
//   * game removal also removes the stored key + settings (no key leak)
#include "framework.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "any_ps3/disc.h"
#include "anyps3/fs/iso9660.h"
#include "anyps3/loader/import.h"
#include "anyps3/platform.h"
#include "anyps3/sfo/sfo.h"

using namespace anyps3;

// ---- fixture builders (same approach as test_disc.cpp) ----------------------

static std::vector<u8> MakeSfo(const std::string& title, const std::string& id) {
    ParamSfoBuilder b;
    b.SetString("TITLE", title)
        .SetString("TITLE_ID", id)
        .SetString("CATEGORY", "GD")
        .SetString("APP_VER", "01.00")
        .SetString("PS3_SYSTEM_VER", "04.910")
        .SetInt("PARENTAL_LEVEL", 1);
    return b.Build();
}

static std::vector<u8> MakePng() {
    return {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3};
}

static void WriteBe32(std::vector<u8>& v, size_t off, u32 val) {
    v[off] = static_cast<u8>(val >> 24);
    v[off + 1] = static_cast<u8>(val >> 16);
    v[off + 2] = static_cast<u8>(val >> 8);
    v[off + 3] = static_cast<u8>(val);
}

static void EncryptSectors(std::vector<u8>& img, const uint8_t key[16], u32 first,
                           u32 last) {
    any_ps3::Aes128 aes(key);
    for (u32 s = first; s <= last; s++) {
        uint8_t iv[16];
        any_ps3::DiscCrypto::SectorIv(s, iv);
        aes.CbcEncrypt(img.data() + static_cast<size_t>(s) * 2048, 2048, iv);
    }
}

// Encrypted image WITHOUT PS3_DISC.SFB: without a key there is no id source
// at all (SFO garbage, no SFB, no id in the file name) -> UNKNOWN placeholder.
static std::vector<u8> BuildLockedIso(const uint8_t key[16], const std::string& title,
                                      const std::string& id) {
    IsoWriter w;
    w.AddDir("PS3_GAME");
    w.AddFile("PS3_GAME/PARAM.SFO", MakeSfo(title, id));
    w.AddFile("PS3_GAME/ICON0.PNG", MakePng());
    w.AddFile("PS3_GAME/USRDIR/EBOOT.BIN", {0xEF, 0xBE, 0xAD, 0xDE, 1, 2, 3, 4});
    std::vector<u8> img = w.Build();
    u32 first = 0xFFFFFFFF, last = 0;
    {
        IsoReader probe;
        if (!probe.Open(img)) return {};
        for (const char* rel : {"PS3_GAME/PARAM.SFO", "PS3_GAME/ICON0.PNG",
                                "PS3_GAME/USRDIR/EBOOT.BIN"}) {
            auto e = probe.Find(rel);
            if (!e) continue;
            first = std::min(first, e->lba);
            last = std::max(last, static_cast<u32>(e->lba + (e->size + 2047) / 2048 - 1));
        }
    }
    if (first == 0xFFFFFFFF) return {};
    EncryptSectors(img, key, first, last);
    u32 total = static_cast<u32>(img.size() / 2048);
    WriteBe32(img, 0, 2);
    WriteBe32(img, 12, first - 1);
    WriteBe32(img, 16, last + 1);
    WriteBe32(img, 20, total - 1);
    return img;
}

static std::string WriteTempFile(const char* pattern, const std::vector<u8>& data) {
    char tmpl[128];
    std::snprintf(tmpl, sizeof(tmpl), "/tmp/anyps3_%s_XXXXXX", pattern);
    int fd = mkstemp(tmpl);
    CHECK_TRUE(fd >= 0);
    FILE* f = fdopen(fd, "wb");
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return tmpl;
}

static const char* kTestKeyHex = "2b7e151628aed2a6abf7158809cf4f3c";

static void RemoveTree(const char* path) {
    std::string cmd = std::string("rm -rf '") + path + "'";
    if (std::system(cmd.c_str()) != 0) {
    }
}

// ---- key file parsing --------------------------------------------------------

TEST(rekey_key_file_text_variants) {
    uint8_t key[16];
    CHECK_TRUE(any_ps3::ParseDiscKey(kTestKeyHex, key, nullptr));

    // (a) plain hex text with a trailing newline
    {
        std::string p = WriteTempFile("ktxt", std::vector<u8>(kTestKeyHex, kTestKeyHex + 32));
        FILE* f = std::fopen(p.c_str(), "ab");
        std::fputc('\n', f);
        std::fclose(f);
        std::string err;
        CHECK_STR_EQ(ReadDiscKeyFileAsText(p, &err), kTestKeyHex);
        std::remove(p.c_str());
    }
    // (b) raw 16 binary bytes
    {
        std::vector<u8> raw(key, key + 16);
        // make byte 15 non-trivial for whitespace edge cases:
        uint8_t raw_key[16];
        std::memcpy(raw_key, key, 16);
        raw_key[15] = 0x0a;  // a byte the TEXT parser would trim away
        std::string p = WriteTempFile("kraw", std::vector<u8>(raw_key, raw_key + 16));
        std::string err;
        std::string want;
        static const char* hex = "0123456789abcdef";
        for (int i = 0; i < 16; i++) {
            want += hex[raw_key[i] >> 4];
            want += hex[raw_key[i] & 0xF];
        }
        CHECK_STR_EQ(ReadDiscKeyFileAsText(p, &err), want);
        std::remove(p.c_str());
    }
    // (c) colon-separated bytes
    {
        std::string colon = "2b:7e:15:16:28:ae:d2:a6:ab:f7:15:88:09:cf:4f:3c";
        std::string p = WriteTempFile("kcol",
                                      std::vector<u8>(colon.begin(), colon.end()));
        std::string err;
        CHECK_STR_EQ(ReadDiscKeyFileAsText(p, &err), kTestKeyHex);
        std::remove(p.c_str());
    }
    // (d) garbage -> honest empty + error
    {
        std::string garbage = "this is not a key at all, sorry";
        std::string p = WriteTempFile("kgarbage",
                                      std::vector<u8>(garbage.begin(), garbage.end()));
        std::string err;
        CHECK_STR_EQ(ReadDiscKeyFileAsText(p, &err), "");
        CHECK_TRUE(!err.empty());
        std::remove(p.c_str());
    }
    // (e) missing file
    {
        std::string err;
        CHECK_STR_EQ(ReadDiscKeyFileAsText("/tmp/anyps3_no_such_key_file_xyz", &err), "");
        CHECK_TRUE(!err.empty());
    }
}

TEST(rekey_effective_key_prefers_typed_hex) {
    // key file holds key A, typed hex holds key B -> typed wins
    std::string keyA = "0123456789abcdef0123456789abcdef";
    std::string keyB = "fedcba9876543210fedcba9876543210";
    std::string kf = WriteTempFile("keff", std::vector<u8>(keyB.begin(), keyB.end()));
    GameSettings s;
    s.dkey_path = kf;
    s.dkey_hex = keyA;
    std::string err;
    CHECK_STR_EQ(EffectiveDiscKeyText(s, &err), keyA);
    // file-only fallback
    s.dkey_hex.clear();
    CHECK_STR_EQ(EffectiveDiscKeyText(s, &err), keyB);
    // nothing attached
    s.dkey_path.clear();
    CHECK_STR_EQ(EffectiveDiscKeyText(s, &err), "");
    CHECK_TRUE(!err.empty());
    std::remove(kf.c_str());
}

// ---- the full re-key flow (path-based, as the Windows host runs it) --------

TEST(rekey_path_import_then_unlock) {
    RemoveTree("/tmp/anyps3_lib_rekey1");
    uint8_t key[16];
    CHECK_TRUE(any_ps3::ParseDiscKey(kTestKeyHex, key, nullptr));
    std::vector<u8> img = BuildLockedIso(key, "Rekey Game", "RKY00001");
    CHECK_TRUE(!img.empty());
    // Display name has no title-id pattern and no sibling key exists.
    std::string iso_path = WriteTempFile("mylockedgame", img);

    // 1) import without key -> "Encrypted - disc key needed", placeholder id
    GameLibrary lib("/tmp/anyps3_lib_rekey1");
    GameInfo imported;
    std::string err;
    CHECK_TRUE(lib.Import(iso_path, &imported, &err));
    CHECK_EQ(static_cast<int>(imported.encrypted), static_cast<int>(EncState::NeedsKey));
    CHECK_STR_EQ(imported.title_id, "UNKNOWN");

    // 2) the user attaches a key through the per-game settings (key FILE,
    //    deliberately NOT next to the ISO so no sibling pickup happens)
    AppConfig app{"/tmp/anyps3_lib_rekey1"};
    app.global.LoadFromFile("/tmp/anyps3_lib_rekey1/anyps3.ini");
    std::string keyfile = WriteTempFile("users_own_key",
                                        std::vector<u8>(kTestKeyHex, kTestKeyHex + 32));
    GameSettings gs = GameSettings::Load(app, "UNKNOWN");
    gs.dkey_path = keyfile;
    gs.Save(app, "UNKNOWN");
    std::string kerr;
    std::string key_text = EffectiveDiscKeyText(gs, &kerr);
    CHECK_STR_EQ(key_text, kTestKeyHex);

    // 3) re-read with the key -> real metadata through the decrypted view
    GameInfo info;
    CHECK_TRUE(InspectGame(iso_path, &info, &err, key_text));
    CHECK_EQ(static_cast<int>(info.encrypted), static_cast<int>(EncState::KeyInUse));
    CHECK_STR_EQ(info.title, "Rekey Game");
    CHECK_STR_EQ(info.title_id, "RKY00001");
    CHECK_STR_EQ(info.category, "GD");
    CHECK_STR_EQ(info.meta_source, "PARAM.SFO");

    // 4) the library entry is replaced: UNKNOWN -> RKY00001, settings follow
    CHECK_TRUE(ReplaceLibraryEntry(app, &lib, info, "UNKNOWN", &err));
    std::vector<LibraryEntry> entries;
    CHECK_TRUE(lib.List(&entries));
    CHECK_EQ(entries.size(), 1u);
    CHECK_STR_EQ(entries[0].info.title_id, "RKY00001");
    CHECK_STR_EQ(entries[0].info.title, "Rekey Game");
    CHECK_EQ(static_cast<int>(entries[0].info.encrypted),
             static_cast<int>(EncState::KeyInUse));
    // settings moved with the entry
    GameSettings moved = GameSettings::Load(app, "RKY00001");
    CHECK_STR_EQ(moved.dkey_path, keyfile);
    // old slot really gone
    CHECK_TRUE(!platform::IsFile("/tmp/anyps3_lib_rekey1/games/UNKNOWN/game.txt"));

    std::remove(iso_path.c_str());
    std::remove(keyfile.c_str());
    RemoveTree("/tmp/anyps3_lib_rekey1");
}

// ---- fd-based re-read (the Android host flow) --------------------------------

TEST(rekey_fd_locked_then_unlocked_with_key) {
    uint8_t key[16];
    CHECK_TRUE(any_ps3::ParseDiscKey(kTestKeyHex, key, nullptr));
    std::vector<u8> img = BuildLockedIso(key, "Fd Rekey Game", "FDR00001");
    CHECK_TRUE(!img.empty());
    std::string iso_path = WriteTempFile("fdlocked", img);
    int fd = ::open(iso_path.c_str(), O_RDONLY);
    CHECK_TRUE(fd >= 0);

    // No key: placeholder import (no sibling possible on a SAF fd)
    GameInfo info;
    std::string err;
    CHECK_TRUE(InspectGameFd(fd, "random_locked_image.iso", &info, &err));
    CHECK_EQ(static_cast<int>(info.encrypted), static_cast<int>(EncState::NeedsKey));
    CHECK_STR_EQ(info.title_id, "UNKNOWN");

    // With the explicit key: the SFO decrypts, metadata is real.
    CHECK_TRUE(InspectGameFd(fd, "random_locked_image.iso", &info, &err, kTestKeyHex));
    CHECK_EQ(static_cast<int>(info.encrypted), static_cast<int>(EncState::KeyInUse));
    CHECK_STR_EQ(info.title, "Fd Rekey Game");
    CHECK_STR_EQ(info.title_id, "FDR00001");
    CHECK_STR_EQ(info.category, "GD");

    ::close(fd);
    std::remove(iso_path.c_str());
}

// ---- wrong-key honesty --------------------------------------------------------

TEST(rekey_wrong_explicit_key_refuses_and_keeps_entry) {
    RemoveTree("/tmp/anyps3_lib_rekey2");
    uint8_t key[16];
    CHECK_TRUE(any_ps3::ParseDiscKey(kTestKeyHex, key, nullptr));
    std::vector<u8> img = BuildLockedIso(key, "Wrong Key Game", "WRK00001");
    CHECK_TRUE(!img.empty());
    std::string iso_path = WriteTempFile("wrongkeygame", img);

    GameLibrary lib("/tmp/anyps3_lib_rekey2");
    GameInfo imported;
    std::string err;
    CHECK_TRUE(lib.Import(iso_path, &imported, &err));
    CHECK_EQ(static_cast<int>(imported.encrypted), static_cast<int>(EncState::NeedsKey));

    // A wrong EXPLICIT key must fail the re-read honestly — the library
    // entry stays exactly as it was.
    GameInfo info;
    CHECK_TRUE(!InspectGame(iso_path, &info, &err, "ffffffffffffffffffffffffffffffff"));
    CHECK_TRUE(err.find("did not unlock") != std::string::npos);
    std::vector<LibraryEntry> entries;
    CHECK_TRUE(lib.List(&entries));
    CHECK_EQ(entries.size(), 1u);
    CHECK_EQ(static_cast<int>(entries[0].info.encrypted),
             static_cast<int>(EncState::NeedsKey));
    CHECK_STR_EQ(entries[0].info.title_id, "UNKNOWN");

    std::remove(iso_path.c_str());
    RemoveTree("/tmp/anyps3_lib_rekey2");
}

// ---- game removal must also remove the stored key (no leak) ------------------

TEST(rekey_remove_game_deletes_settings_and_key) {
    RemoveTree("/tmp/anyps3_lib_rekey3");
    AppConfig app{"/tmp/anyps3_lib_rekey3"};
    GameLibrary lib("/tmp/anyps3_lib_rekey3");
    GameInfo info;
    std::string err;
    info.title = "Leak Test";
    info.title_id = "LKY00001";
    CHECK_TRUE(lib.ImportInfo(info, &err));
    // store settings + a key file in the game's slot
    GameSettings gs;
    gs.dkey_hex = "0102030405060708090a0b0c0d0e0f10";
    gs.Save(app, "LKY00001");
    std::string dkey = "/tmp/anyps3_lib_rekey3/games/LKY00001/dkey.bin";
    FILE* f = std::fopen(dkey.c_str(), "wb");
    CHECK_TRUE(f != nullptr);
    std::fputs("0102030405060708090a0b0c0d0e0f10", f);
    std::fclose(f);

    CHECK_TRUE(lib.Remove("LKY00001", &err));
    CHECK_TRUE(!platform::IsFile("/tmp/anyps3_lib_rekey3/games/LKY00001/settings.ini"));
    CHECK_TRUE(!platform::IsFile(dkey));
    CHECK_TRUE(!platform::IsFile("/tmp/anyps3_lib_rekey3/games/LKY00001/game.txt"));
    RemoveTree("/tmp/anyps3_lib_rekey3");
}

// ---- settings persistence of the typed hex key -------------------------------

TEST(rekey_dkey_hex_persists_in_settings_ini) {
    RemoveTree("/tmp/anyps3_lib_rekey4");
    AppConfig app{"/tmp/anyps3_lib_rekey4"};
    GameSettings gs;
    gs.dkey_hex = "0123456789abcdef0123456789abcdef";
    gs.controller["cross"] = 96;
    gs.Save(app, "HEX00001");
    GameSettings back = GameSettings::Load(app, "HEX00001");
    CHECK_STR_EQ(back.dkey_hex, "0123456789abcdef0123456789abcdef");
    CHECK_EQ(back.controller.count("cross") > 0 ? back.controller["cross"] : -1, 96);
    RemoveTree("/tmp/anyps3_lib_rekey4");
}
