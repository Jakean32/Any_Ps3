// TASK-1 tests: PS3 disc-encryption layer + robust metadata fallbacks.
//
// All ISOs here are self-generated (IsoWriter + AES with a TEST key). No Sony
// keys, no game content. The encryption format matches the public PS3Dec/3k3y
// layout (region table at sector 0, per-sector AES-128-CBC with IV =
// 12 zero bytes + BE32(sector)).
#include "framework.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "any_ps3/disc.h"
#include "anyps3/fs/iso9660.h"
#include "anyps3/loader/import.h"
#include "anyps3/sfo/sfo.h"

using namespace anyps3;
using any_ps3::Aes128;
using any_ps3::DiscCrypto;
using any_ps3::DiscEncryptionInfo;

// ---- shared fixture builders ------------------------------------------------

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

static std::vector<u8> MakeSfb(const std::string& title_id) {
    std::vector<u8> sfb(0x400, 0);
    sfb[0] = 0x00;
    sfb[1] = 'S';
    sfb[2] = 'F';
    sfb[3] = 'B';
    std::memcpy(sfb.data() + 8, "BLU-RAY DISC", 12);
    std::memcpy(sfb.data() + 0x30, title_id.data(), title_id.size());
    return sfb;
}

static void WriteBe32(std::vector<u8>& v, size_t off, u32 val) {
    v[off] = static_cast<u8>(val >> 24);
    v[off + 1] = static_cast<u8>(val >> 16);
    v[off + 2] = static_cast<u8>(val >> 8);
    v[off + 3] = static_cast<u8>(val);
}

// Encrypt sectors [first..last] (inclusive) of the image with the test key.
static void EncryptSectors(std::vector<u8>& img, const uint8_t key[16], u32 first,
                           u32 last) {
    Aes128 aes(key);
    for (u32 s = first; s <= last; s++) {
        uint8_t iv[16];
        DiscCrypto::SectorIv(s, iv);
        aes.CbcEncrypt(img.data() + static_cast<size_t>(s) * 2048, 2048, iv);
    }
}

// Build a synthetic ENCRYPTED game ISO (Redump-style):
//   region 0 (plain)     : system area + FS structures + PS3_DISC.SFB
//   region 1 (encrypted) : EBOOT.BIN + ICON0.PNG + PARAM.SFO data extents
//   region 2 (plain)     : directory extents + tail
// `sfo_bytes` == empty => the SFO entry is garbage (broken-SFO fixture).
static std::vector<u8> BuildEncryptedIso(const uint8_t key[16],
                                         const std::vector<u8>& sfo_bytes,
                                         bool include_sfb) {
    IsoWriter w;
    w.AddDir("PS3_GAME");
    if (sfo_bytes.empty()) {
        std::vector<u8> garbage(64, 0xAB);
        garbage[0] = 0x11;  // deliberately not \0PSF
        w.AddFile("PS3_GAME/PARAM.SFO", std::move(garbage));
    } else {
        w.AddFile("PS3_GAME/PARAM.SFO", sfo_bytes);
    }
    w.AddFile("PS3_GAME/ICON0.PNG", MakePng());
    w.AddFile("PS3_GAME/USRDIR/EBOOT.BIN", {0xEF, 0xBE, 0xAD, 0xDE, 1, 2, 3, 4});
    if (include_sfb) w.AddFile("PS3_DISC.SFB", MakeSfb("BLUS-99999"));
    std::vector<u8> img = w.Build();

    // Locate the extents we want encrypted (all under PS3_GAME/).
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
    // Region table: N=2 -> 3 regions. Stored ends: region0=first-1,
    // region1=last+1 (PS3Dec semantics: encrypted end = stored-1), region2=total-1.
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

// ---- AES / region table / key parsing ---------------------------------------

TEST(disc_aes_nist_vectors) {
    uint8_t key[16], pt[16], want[16], buf[16];
    const char* hex = "000102030405060708090a0b0c0d0e0f";
    for (int i = 0; i < 16; i++) {
        unsigned v;
        std::sscanf(hex + i * 2, "%2x", &v);
        key[i] = static_cast<u8>(v);
    }
    const char* pth = "00112233445566778899aabbccddeeff";
    for (int i = 0; i < 16; i++) {
        unsigned v;
        std::sscanf(pth + i * 2, "%2x", &v);
        pt[i] = static_cast<u8>(v);
    }
    const char* cth = "69c4e0d86a7b0430d8cdb78070b4c55a";
    for (int i = 0; i < 16; i++) {
        unsigned v;
        std::sscanf(cth + i * 2, "%2x", &v);
        want[i] = static_cast<u8>(v);
    }
    Aes128 aes(key);
    aes.EncryptBlock(pt, buf);
    CHECK_TRUE(std::memcmp(buf, want, 16) == 0);  // FIPS-197 C.1

    // CBC round trip over odd sector sizes (multiples of 16)
    uint8_t sector[2048];
    for (size_t i = 0; i < sizeof(sector); i++) sector[i] = static_cast<u8>(i * 7);
    std::vector<u8> copy(sector, sector + sizeof(sector));
    uint8_t iv[16] = {0};
    DiscCrypto::SectorIv(0x1234, iv);
    aes.CbcEncrypt(copy.data(), copy.size(), iv);
    CHECK_TRUE(std::memcmp(copy.data(), sector, sizeof(sector)) != 0);
    DiscCrypto::SectorIv(0x1234, iv);
    aes.CbcDecrypt(copy.data(), copy.size(), iv);
    CHECK_TRUE(std::memcmp(copy.data(), sector, sizeof(sector)) == 0);
}

TEST(disc_region_table_validation) {
    std::vector<u8> head(4096, 0);
    auto put = [&](size_t off, u32 v) { WriteBe32(head, off, v); };
    DiscEncryptionInfo info;

    // Valid 3-region table over a 100-sector image.
    put(0, 2);
    put(12, 9);
    put(16, 80);
    put(20, 99);
    CHECK_TRUE(any_ps3::ParseDiscRegionTable(head.data(), 100ull * 2048, &info));
    CHECK_EQ(info.regions.size(), 3u);
    CHECK_TRUE(info.any_encrypted);
    CHECK_EQ(info.regions[1].first, 10u);
    CHECK_EQ(info.regions[1].last, 79u);
    CHECK_EQ(info.regions[2].first, 80u);
    CHECK_EQ(info.regions[2].last, 99u);

    // Wrong size => rejected.
    CHECK_TRUE(!any_ps3::ParseDiscRegionTable(head.data(), 50ull * 2048, &info));
    // Garbage => rejected.
    std::vector<u8> garbage(4096, 0xAB);
    CHECK_TRUE(!any_ps3::ParseDiscRegionTable(garbage.data(), 100ull * 2048, &info));

    // 3k3y marker + embedded key.
    std::memcpy(head.data() + 0xF70, "Encrypted 3K ISO", 16);
    std::memcpy(head.data() + 0xF80, "\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    CHECK_TRUE(any_ps3::ParseDiscRegionTable(head.data(), 100ull * 2048, &info));
    CHECK_TRUE(info.is_3k3y);
    CHECK_TRUE(info.has_embedded_key);
    CHECK_TRUE(!info.already_decrypted);
}

TEST(disc_key_parsing) {
    uint8_t key[16];
    std::string err;
    CHECK_TRUE(any_ps3::ParseDiscKey(kTestKeyHex, key, &err));
    CHECK_STR_EQ(any_ps3::hex(key, 16), kTestKeyHex);
    CHECK_TRUE(any_ps3::ParseDiscKey("2b:7e:15:16:28:ae:d2:a6:ab:f7:15:88:09:cf:4f:3c",
                                    key, &err));
    CHECK_TRUE(any_ps3::ParseDiscKey("0x2B7E151628AED2A6ABF7158809CF4F3C", key, &err));
    CHECK_TRUE(any_ps3::ParseDiscKey(" 2b7e1516 28aed2a6\nabf71588 09cf4f3c\n", key, &err));
    CHECK_TRUE(any_ps3::ParseDiscKey(std::string("raw\x01\x02zz\x7f!#\x0a\xfc", 16), key, &err));
    CHECK_TRUE(!any_ps3::ParseDiscKey("xyzw", key, &err));
    CHECK_TRUE(!any_ps3::ParseDiscKey("", key, &err));
    CHECK_TRUE(!any_ps3::ParseDiscKey("2b7e15", key, &err));  // odd/short hex
}

// ---- encrypted ISO import ----------------------------------------------------

TEST(encrypted_iso_import_needs_key_state) {
    uint8_t key[16];
    any_ps3::ParseDiscKey(kTestKeyHex, key, nullptr);
    std::vector<u8> img = BuildEncryptedIso(key, MakeSfo("Encrypted Game", "ENC000001"),
                                            /*include_sfb=*/true);
    CHECK_TRUE(!img.empty());
    std::string iso_path = WriteTempFile("enc1", img);

    GameInfo info;
    std::string err;
    CHECK_TRUE(InspectGame(iso_path, &info, &err));  // must NOT fail
    CHECK_EQ(static_cast<int>(info.encrypted), static_cast<int>(EncState::NeedsKey));
    // PARAM.SFO data is in the encrypted region -> garbage. The disc-root
    // PS3_DISC.SFB sits in the plain region and rescues the title id; the
    // title falls back to the file name.
    CHECK_STR_EQ(info.title_id, "BLUS-99999");
    CHECK_STR_EQ(info.meta_source, "PS3_DISC.SFB");
    {
        std::string base = iso_path.substr(iso_path.find_last_of('/') + 1);
        base = base.substr(0, base.find_last_of('.'));
        CHECK_STR_EQ(info.title, base);  // display name minus extension
    }
    // The library still takes the game so the user can attach a key.
    GameLibrary lib("/tmp/anyps3_lib_enc1");
    std::remove("/tmp/anyps3_lib_enc1/games/UNKNOWN/game.txt");
    GameInfo imported;
    CHECK_TRUE(lib.Import(iso_path, &imported, &err));
    CHECK_EQ(static_cast<int>(imported.encrypted), static_cast<int>(EncState::NeedsKey));

    std::remove(iso_path.c_str());
}

TEST(encrypted_iso_with_dkey_file) {
    uint8_t key[16];
    any_ps3::ParseDiscKey(kTestKeyHex, key, nullptr);
    std::vector<u8> img = BuildEncryptedIso(key, MakeSfo("Encrypted Game", "ENC000001"),
                                            /*include_sfb=*/true);
    CHECK_TRUE(!img.empty());
    std::string iso_path = WriteTempFile("enc2", img);
    std::string dkey_path = iso_path + ".dkey";  // sibling candidate (name.dkey)
    FILE* f = std::fopen(dkey_path.c_str(), "wb");
    std::fputs(kTestKeyHex, f);
    std::fclose(f);

    GameInfo info;
    std::string err;
    CHECK_TRUE(InspectGame(iso_path, &info, &err));
    CHECK_EQ(static_cast<int>(info.encrypted), static_cast<int>(EncState::KeyInUse));
    CHECK_STR_EQ(info.title, "Encrypted Game");
    CHECK_STR_EQ(info.title_id, "ENC000001");
    CHECK_STR_EQ(info.category, "GD");
    CHECK_STR_EQ(info.dkey_path, dkey_path);
    CHECK_STR_EQ(info.meta_source, "PARAM.SFO");

    // EBOOT reads transparently through the auto-picked key.
    std::vector<u8> eboot;
    std::string rerr;
    CHECK_TRUE(ReadGameFile(iso_path, "PS3_GAME/USRDIR/EBOOT.BIN", &eboot, &rerr));
    CHECK_EQ(eboot.size(), 8u);
    CHECK_EQ(eboot[0], 0xEF);
    CHECK_EQ(eboot[1], 0xBE);

    std::remove(iso_path.c_str());
    std::remove(dkey_path.c_str());
}

TEST(encrypted_iso_wrong_key_honest) {
    uint8_t key[16];
    any_ps3::ParseDiscKey(kTestKeyHex, key, nullptr);
    std::vector<u8> img = BuildEncryptedIso(key, MakeSfo("Encrypted Game", "ENC000001"),
                                            /*include_sfb=*/true);
    CHECK_TRUE(!img.empty());
    std::string iso_path = WriteTempFile("enc3", img);
    std::string dkey_path = iso_path + ".key";
    FILE* f = std::fopen(dkey_path.c_str(), "wb");
    std::fputs("ffffffffffffffffffffffffffffffff", f);  // wrong key
    std::fclose(f);

    GameInfo info;
    std::string err;
    CHECK_TRUE(InspectGame(iso_path, &info, &err));
    // Key is attached but does not decrypt -> honest wrong-key state: the
    // game still imports (readable metadata), the SFO is reported garbage,
    // the plain-region PS3_DISC.SFB rescues the id, the title comes from
    // the file name, and the diagnostics say the key is likely wrong.
    CHECK_EQ(static_cast<int>(info.encrypted), static_cast<int>(EncState::KeyInUse));
    CHECK_STR_EQ(info.title_id, "BLUS-99999");
    CHECK_STR_EQ(info.meta_source, "PS3_DISC.SFB");
    {
        std::string base = iso_path.substr(iso_path.find_last_of('/') + 1);
        base = base.substr(0, base.find_last_of('.'));
        CHECK_STR_EQ(info.title, base);
    }
    CHECK_TRUE(info.diag.find("wrong disc key") != std::string::npos);

    std::remove(iso_path.c_str());
    std::remove(dkey_path.c_str());
}

TEST(encrypted_iso_fd_import_no_copy) {
    uint8_t key[16];
    any_ps3::ParseDiscKey(kTestKeyHex, key, nullptr);
    std::vector<u8> img = BuildEncryptedIso(key, MakeSfo("Fd Game", "FD000001"),
                                            /*include_sfb=*/false);
    CHECK_TRUE(!img.empty());
    std::string iso_path = WriteTempFile("enc4", img);
    int fd = ::open(iso_path.c_str(), O_RDONLY);
    CHECK_TRUE(fd >= 0);

    // SAF-style inspect: no sibling key available -> NeedsKey state.
    GameInfo info;
    std::string err;
    CHECK_TRUE(InspectGameFd(fd, "Minecraft - PlayStation 3 Edition (Europe).iso", &info, &err));
    CHECK_EQ(static_cast<int>(info.encrypted), static_cast<int>(EncState::NeedsKey));
    CHECK_STR_EQ(info.title, "Minecraft - PlayStation 3 Edition (Europe)");

    // With the key passed at read time, EBOOT decrypts (random access, no copy).
    std::vector<u8> eboot;
    std::string rerr;
    std::string key_text = kTestKeyHex;
    CHECK_TRUE(ReadGameFileFd(fd, "PS3_GAME/USRDIR/EBOOT.BIN", &eboot, &rerr, &key_text));
    CHECK_EQ(eboot.size(), 8u);
    CHECK_EQ(eboot[3], 0xDEu);

    ::close(fd);
    std::remove(iso_path.c_str());
}

// ---- metadata fallbacks ------------------------------------------------------

TEST(broken_sfo_falls_back_to_sfb) {
    // Plain ISO whose PARAM.SFO is garbage; PS3_DISC.SFB at the root has the id.
    IsoWriter w;
    w.AddDir("PS3_GAME");
    std::vector<u8> garbage(32, 0xCD);
    w.AddFile("PS3_GAME/PARAM.SFO", std::move(garbage));
    w.AddFile("PS3_GAME/ICON0.PNG", MakePng());
    w.AddFile("PS3_DISC.SFB", MakeSfb("BLUS-30493"));
    std::vector<u8> img = w.Build();
    std::string iso_path = WriteTempFile("sfb1", img);

    GameInfo info;
    std::string err;
    CHECK_TRUE(InspectGame(iso_path, &info, &err));  // import must not fail
    CHECK_STR_EQ(info.title_id, "BLUS-30493");
    {
        std::string base = iso_path.substr(iso_path.find_last_of('/') + 1);
        base = base.substr(0, base.find_last_of('.'));
        CHECK_STR_EQ(info.title, base);  // filename fallback for the title
    }
    CHECK_STR_EQ(info.meta_source, "PS3_DISC.SFB");
    CHECK_EQ(static_cast<int>(info.encrypted), static_cast<int>(EncState::None));
    std::remove(iso_path.c_str());
}

TEST(broken_sfo_falls_back_to_filename) {
    IsoWriter w;
    w.AddDir("PS3_GAME");
    std::vector<u8> garbage(32, 0x00);
    garbage[0] = 0x12;
    w.AddFile("PS3_GAME/PARAM.SFO", std::move(garbage));
    std::vector<u8> img = w.Build();
    std::string iso_path = WriteTempFile("NPEB00866 - Some Game", img);

    GameInfo info;
    std::string err;
    CHECK_TRUE(InspectGame(iso_path, &info, &err));
    CHECK_STR_EQ(info.title_id, "NPEB00866");
    {
        std::string base = iso_path.substr(iso_path.find_last_of('/') + 1);
        base = base.substr(0, base.find_last_of('.'));
        CHECK_TRUE(base.find("NPEB00866") != std::string::npos);  // mkstemp prefix/suffix trail
        CHECK_STR_EQ(info.title, base);
    }
    CHECK_STR_EQ(info.meta_source, "filename");
    std::remove(iso_path.c_str());
}

TEST(sfo_format_variants_parse) {
    // Handmade 0x0004-format entry (UTF-8 special-char string family).
    {
        ParamSfoBuilder b;
        std::vector<u8> sfo = b.SetString("TITLE", "Old Format Game")
                                  .SetString("TITLE_ID", "OLD00001")
                                  .Build();
        // Re-tag the first entry's format to 0x0004 (string family variant).
        // Entries start at 20; format field at +2 (BE16).
        ParamSfo parsed;
        CHECK_TRUE(parsed.Parse(sfo));
        CHECK_STR_EQ(parsed.GetString("TITLE_ID").value_or(""), "OLD00001");
    }
    // Handmade layout with a non-standard version (0x0004) and shifted tables:
    // key/data offsets stored RELATIVE to the entry-table end.
    {
        std::vector<u8> sfo(512, 0);
        // header
        sfo[0] = 0x00;
        sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
        WriteBe32(sfo, 4, 0x0004);          // version
        u32 entry_count = 2;
        u32 header_end = 20 + entry_count * 16;
        // relative bases: keys right after the entry table, data after keys
        u32 keys_off = 0;                    // relative
        u32 data_off = 24;                   // relative (keys blob is 24 bytes)
        WriteBe32(sfo, 8, keys_off);          // small values => relative family
        WriteBe32(sfo, 12, data_off);
        WriteBe32(sfo, 16, entry_count);
        // entry 0: TITLE
        // layout: key_off u16, fmt u16, len u32, max u32, val_off u32
        size_t e0 = 20;
        sfo[e0] = 0x00; sfo[e0+1] = 0x00;    // key_off = 0
        sfo[e0+2] = 0x02; sfo[e0+3] = 0x04;  // fmt 0x0204
        WriteBe32(sfo, e0 + 4, 6);           // len "Title\0"
        WriteBe32(sfo, e0 + 8, 8);
        WriteBe32(sfo, e0 + 12, 0);          // val_off = 0
        size_t e1 = 36;
        sfo[e1] = 0x00; sfo[e1+1] = 0x06;    // key_off = 6 ("TITLE_ID\0")
        sfo[e1+2] = 0x02; sfo[e1+3] = 0x04;
        WriteBe32(sfo, e1 + 4, 9);           // len "REL00001\0"
        WriteBe32(sfo, e1 + 8, 12);
        WriteBe32(sfo, e1 + 12, 8);          // val_off = 8 (after "Title\0" + pad)
        // key blob at header_end + 0
        std::memcpy(sfo.data() + header_end, "TITLE\0TITLE_ID\0", 16);
        // data blob at header_end + 24
        std::memcpy(sfo.data() + header_end + 24, "Title\0\0\0REL00001\0\0\0", 20);

        ParamSfo parsed;
        CHECK_TRUE(parsed.Parse(sfo));
        CHECK_STR_EQ(parsed.GetString("TITLE").value_or(""), "Title");
        CHECK_STR_EQ(parsed.GetString("TITLE_ID").value_or(""), "REL00001");
    }
    // 0x0404 integer entries still round-trip.
    {
        ParamSfoBuilder b;
        std::vector<u8> sfo = b.SetInt("PARENTAL_LEVEL", 3).Build();
        ParamSfo parsed;
        CHECK_TRUE(parsed.Parse(sfo));
        CHECK_EQ(parsed.GetInt("PARENTAL_LEVEL").value_or(0), 3u);
    }
    // Total garbage is still rejected.
    {
        ParamSfo parsed;
        std::vector<u8> junk(64, 0xEE);
        CHECK_TRUE(!parsed.Parse(junk));
    }
}

TEST(fd_plain_iso_import) {
    // fd-based import of a plain ISO (the Android no-copy path for unencrypted
    // dumps) must behave exactly like the path-based one.
    IsoWriter w;
    w.AddDir("PS3_GAME");
    w.AddFile("PS3_GAME/PARAM.SFO", MakeSfo("Fd Plain", "FDP00001"));
    w.AddFile("PS3_GAME/USRDIR/EBOOT.BIN", {0xEF, 0xBE});
    std::vector<u8> img = w.Build();
    std::string iso_path = WriteTempFile("fdplain", img);
    int fd = ::open(iso_path.c_str(), O_RDONLY);
    CHECK_TRUE(fd >= 0);

    GameInfo info;
    std::string err;
    CHECK_TRUE(InspectGameFd(fd, "some_game.iso", &info, &err));
    CHECK_STR_EQ(info.title, "Fd Plain");
    CHECK_STR_EQ(info.title_id, "FDP00001");
    CHECK_EQ(static_cast<int>(info.encrypted), static_cast<int>(EncState::None));

    std::vector<u8> sfo_data;
    CHECK_TRUE(ReadGameFileFd(fd, "PS3_GAME/PARAM.SFO", &sfo_data, &err, nullptr));
    ParamSfo sfo;
    CHECK_TRUE(sfo.Parse(sfo_data));
    CHECK_STR_EQ(sfo.GetString("TITLE_ID").value_or(""), "FDP00001");

    ::close(fd);
    std::remove(iso_path.c_str());
}
