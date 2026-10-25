// Tests: ISO9660 reading VIA FILE DESCRIPTOR (REQUIRED new test area #2).
// A real mini-ISO image is built, written to a temp file, and read back
// through a POSIX fd — exactly the path Android uses with SAF fds.
#include "harness.h"
#include "testdata.h"
#include "any_ps3/iso.h"
#include "any_ps3/sfo.h"

#include <unistd.h>
#include <fcntl.h>

namespace any_ps3 {

// Builds the standard test image (PS3_GAME with PARAM.SFO, ICON0.PNG,
// PIC1.PNG, EBOOT.BIN) and returns the path of a temp file containing it.
static std::string write_test_iso() {
    testdata::IsoBuilder b;
    auto sfo = testdata::make_sfo({{"TITLE", "Any Ps3 Demo"},
                                   {"TITLE_ID", "NPUP00001"},
                                   {"CATEGORY", "GD"}});
    b.add_file("PARAM.SFO", sfo);
    std::vector<uint8_t> icon(64, 0);
    icon[0] = 0x89; icon[1] = 'P'; icon[2] = 'N'; icon[3] = 'G';
    icon[4] = 0x0D; icon[5] = 0x0A; icon[6] = 0x1A; icon[7] = 0x0A;
    b.add_file("ICON0.PNG", icon);
    std::vector<uint8_t> pic1(100, 0xAB);
    b.add_file("PIC1.PNG", pic1);
    b.add_file("EBOOT.BIN", testdata::make_hello_elf());
    auto img = b.build();

    std::string path = "/tmp/anyps3_test.iso";
    write_file_path(path, img.data(), img.size());
    return path;
}

static void iso_opens_and_lists_root(TestState& st) {
    std::string path = write_test_iso();
    int fd = ::open(path.c_str(), O_RDONLY);
    AP_CHECK(fd >= 0);
    {
        IsoReader iso(std::make_unique<FdSource>(fd));
        AP_CHECK(iso.open());
        AP_CHECK_EQ(iso.error(), std::string());
        std::vector<IsoDirEntry> entries;
        AP_CHECK(iso.list("", entries));
        bool found = false;
        for (auto& e : entries)
            if (e.name == "PS3_GAME" && e.is_dir) found = true;
        AP_CHECK(found);

        // PS3_GAME listing contains our four files
        AP_CHECK(iso.list("PS3_GAME", entries));
        size_t files = 0;
        for (auto& e : entries) if (!e.is_dir) files++;
        AP_CHECK_EQ(files, (size_t)4);
    }
    ::close(fd);
    ::unlink(path.c_str());
}

static void iso_read_param_sfo_from_iso(TestState& st) {
    std::string path = write_test_iso();
    int fd = ::open(path.c_str(), O_RDONLY);
    AP_CHECK(fd >= 0);
    {
        IsoReader iso(std::make_unique<FdSource>(fd));
        AP_CHECK(iso.open());
        std::vector<uint8_t> sfo;
        AP_CHECK(iso.read_file("PS3_GAME/PARAM.SFO", sfo, 1024 * 1024));
        SfoData d;
        try { d = parse_sfo(sfo.data(), sfo.size()); }
        catch (const Error& e) { st.fail(e.what(), __FILE__, __LINE__); ::close(fd); return; }
        AP_CHECK_EQ(d.title(), std::string("Any Ps3 Demo"));
        AP_CHECK_EQ(d.title_id(), std::string("NPUP00001"));
    }
    ::close(fd);
    ::unlink(path.c_str());
}

static void iso_read_icon_and_pic(TestState& st) {
    std::string path = write_test_iso();
    int fd = ::open(path.c_str(), O_RDONLY);
    AP_CHECK(fd >= 0);
    {
        IsoReader iso(std::make_unique<FdSource>(fd));
        AP_CHECK(iso.open());
        std::vector<uint8_t> icon;
        AP_CHECK(iso.read_file("PS3_GAME/ICON0.PNG", icon, 1024 * 1024));
        AP_CHECK_EQ(icon.size(), (size_t)64);
        AP_CHECK_EQ(icon[0], (uint8_t)0x89);
        AP_CHECK_EQ(icon[1], (uint8_t)'P');
        std::vector<uint8_t> pic1;
        AP_CHECK(iso.read_file("PS3_GAME/PIC1.PNG", pic1, 1024 * 1024));
        AP_CHECK_EQ(pic1.size(), (size_t)100);
        AP_CHECK_EQ(pic1[42], (uint8_t)0xAB);
    }
    ::close(fd);
    ::unlink(path.c_str());
}

static void iso_random_access_range(TestState& st) {
    std::string path = write_test_iso();
    int fd = ::open(path.c_str(), O_RDONLY);
    AP_CHECK(fd >= 0);
    {
        IsoReader iso(std::make_unique<FdSource>(fd));
        AP_CHECK(iso.open());
        // odd offset + unaligned length inside PIC1.PNG (100 bytes of 0xAB)
        std::vector<uint8_t> out;
        AP_CHECK(iso.read_file_range("PS3_GAME/PIC1.PNG", 13, 21, out));
        AP_CHECK_EQ(out.size(), (size_t)21);
        for (auto b : out) AP_CHECK_EQ(b, (uint8_t)0xAB);
        // offset+len beyond EOF is clamped
        AP_CHECK(iso.read_file_range("PS3_GAME/PIC1.PNG", 90, 50, out));
        AP_CHECK_EQ(out.size(), (size_t)10);
        // offset beyond EOF fails
        AP_CHECK(!iso.read_file_range("PS3_GAME/PIC1.PNG", 500, 10, out));
        // stat a file through mixed case + lowercase path
        IsoDirEntry e;
        AP_CHECK(iso.stat("ps3_game/param.sfo", e));
        AP_CHECK(!e.is_dir);
        AP_CHECK(e.size > 20); // a real PARAM.SFO is bigger than its header
    }
    ::close(fd);
    ::unlink(path.c_str());
}

static void iso_rejects_garbage(TestState& st) {
    std::vector<uint8_t> junk(64 * 2048, 0x5A);
    std::string path = "/tmp/anyps3_junk.iso";
    write_file_path(path, junk.data(), junk.size());
    int fd = ::open(path.c_str(), O_RDONLY);
    AP_CHECK(fd >= 0);
    {
        IsoReader iso(std::make_unique<FdSource>(fd));
        AP_CHECK(!iso.open());
        AP_CHECK(!iso.error().empty());
    }
    ::close(fd);
    ::unlink(path.c_str());
}

static void iso_udf_detected_honest(TestState& st) {
    // BEA01 + NSR03 descriptors, no CD001 => reader must report UDF honestly.
    std::vector<uint8_t> img(40 * 2048, 0);
    img[16 * 2048 + 1] = 'B'; img[16 * 2048 + 2] = 'E';
    img[16 * 2048 + 3] = 'A'; img[16 * 2048 + 4] = '0'; img[16 * 2048 + 5] = '1';
    img[17 * 2048 + 1] = 'N'; img[17 * 2048 + 2] = 'S';
    img[17 * 2048 + 3] = 'R'; img[17 * 2048 + 4] = '0'; img[17 * 2048 + 5] = '3';
    std::string path = "/tmp/anyps3_udf.iso";
    write_file_path(path, img.data(), img.size());
    int fd = ::open(path.c_str(), O_RDONLY);
    AP_CHECK(fd >= 0);
    {
        IsoReader iso(std::make_unique<FdSource>(fd));
        AP_CHECK(!iso.open());
        std::string msg = iso.error();
        AP_CHECK(msg.find("UDF") != std::string::npos);
        AP_CHECK(msg.find("not supported yet") != std::string::npos);
    }
    ::close(fd);
    ::unlink(path.c_str());
}

static const TestCase kTests[] = {
    AP_TEST(iso_opens_and_lists_root),
    AP_TEST(iso_read_param_sfo_from_iso),
    AP_TEST(iso_read_icon_and_pic),
    AP_TEST(iso_random_access_range),
    AP_TEST(iso_rejects_garbage),
    AP_TEST(iso_udf_detected_honest),
};

const TestCase* iso_tests() { return kTests; }
size_t iso_tests_count() { return sizeof kTests / sizeof kTests[0]; }

} // namespace any_ps3
