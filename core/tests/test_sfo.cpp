// Tests: PARAM.SFO parsing (REQUIRED new test area #1).
#include "harness.h"
#include "testdata.h"
#include "any_ps3/sfo.h"

namespace any_ps3 {

using testdata::put32le;

static void sfo_parse_minimal(TestState& st) {
    auto sfo = testdata::make_sfo({{"TITLE", "Minimal Game"}});
    SfoData d;
    try {
        d = parse_sfo(sfo.data(), sfo.size());
    } catch (const Error& e) { st.fail(e.what(), __FILE__, __LINE__); return; }
    AP_CHECK_EQ(d.entries.size(), (size_t)1);
    AP_CHECK_EQ(d.title(), std::string("Minimal Game"));
    AP_CHECK(d.has("TITLE"));
    AP_CHECK(!d.has("TITLE_ID"));
}

static void sfo_parse_full_game_meta(TestState& st) {
    auto sfo = testdata::make_sfo(
        {{"TITLE", "Any Ps3 Demo"},
         {"TITLE_ID", "NPUP00001"},
         {"CATEGORY", "GD"},
         {"APP_VER", "01.00"},
         {"PS3_SYSTEM_VER", "04.2500"}},
        {{"PARENTAL_LEVEL", 4}});
    SfoData d;
    try {
        d = parse_sfo(sfo.data(), sfo.size());
    } catch (const Error& e) { st.fail(e.what(), __FILE__, __LINE__); return; }
    AP_CHECK_EQ(d.title(), std::string("Any Ps3 Demo"));
    AP_CHECK_EQ(d.title_id(), std::string("NPUP00001"));
    AP_CHECK_EQ(d.category(), std::string("GD"));
    AP_CHECK_EQ(d.app_ver(), std::string("01.00"));
    AP_CHECK_EQ(d.system_ver(), std::string("04.2500"));
    AP_CHECK_EQ(d.integer("PARENTAL_LEVEL"), 4);
}

static void sfo_utf8_special_chars(TestState& st) {
    // Special-char (0x0204) entries carry UTF-8 bytes verbatim.
    std::string title = "J\xC3\xA4ger-Spi\xE2\x84\x96""e \xE2\x80\x94 Test"; // UTF-8
    auto sfo = testdata::make_sfo({{"TITLE", title}, {"TITLE_ID", "BLZE99999"}});
    SfoData d;
    try {
        d = parse_sfo(sfo.data(), sfo.size());
    } catch (const Error& e) { st.fail(e.what(), __FILE__, __LINE__); return; }
    const SfoEntry* e = d.find("TITLE");
    AP_CHECK(e != nullptr);
    AP_CHECK_EQ(e->data_fmt, (uint16_t)0x0204);
    AP_CHECK_EQ(d.title(), title);
}

static void sfo_rejects_bad_magic(TestState& st) {
    std::vector<uint8_t> bad = {'P', 'S', 'F', 'X', 1, 0, 0, 0};
    bool threw = false;
    try { parse_sfo(bad.data(), bad.size()); } catch (const Error&) { threw = true; }
    AP_CHECK(threw);
}

static void sfo_truncated_input(TestState& st) {
    auto sfo = testdata::make_sfo({{"TITLE", "Trunc"}, {"TITLE_ID", "BLUS12345"}});
    bool threw = false;
    try { parse_sfo(sfo.data(), 10); } catch (const Error&) { threw = true; }
    AP_CHECK(threw);
    // header claims more entries than exist -> must not crash
    std::vector<uint8_t> evil = sfo;
    put32le(evil, 16, 100000); // entry_count overflow
    threw = false;
    try { parse_sfo(evil.data(), evil.size()); } catch (const Error&) { threw = true; }
    AP_CHECK(threw);
}

static const TestCase kTests[] = {
    AP_TEST(sfo_parse_minimal),
    AP_TEST(sfo_parse_full_game_meta),
    AP_TEST(sfo_utf8_special_chars),
    AP_TEST(sfo_rejects_bad_magic),
    AP_TEST(sfo_truncated_input),
};

const TestCase* sfo_tests() { return kTests; }
size_t sfo_tests_count() { return sizeof kTests / sizeof kTests[0]; }

} // namespace any_ps3
