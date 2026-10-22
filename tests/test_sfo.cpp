// PARAM.SFO parse + build round-trip tests.
#include "framework.h"

#include "anyps3/sfo/sfo.h"

using namespace anyps3;

TEST(sfo_roundtrip) {
    ParamSfoBuilder b;
    b.SetString("TITLE", "Any Test Game");
    b.SetString("TITLE_ID", "TEST00001");
    b.SetString("CATEGORY", "HG");
    b.SetString("APP_VER", "01.00");
    b.SetString("PS3_SYSTEM_VER", "04.910");
    b.SetInt("ATTRIBUTE", 0x100);
    b.SetInt("BOOTABLE", 1);
    std::vector<u8> data = b.Build();

    ParamSfo sfo;
    CHECK_TRUE(sfo.Parse(data));
    CHECK_STR_EQ(sfo.GetString("TITLE").value_or(""), "Any Test Game");
    CHECK_STR_EQ(sfo.GetString("TITLE_ID").value_or(""), "TEST00001");
    CHECK_STR_EQ(sfo.GetString("CATEGORY").value_or(""), "HG");
    CHECK_EQ(sfo.GetInt("ATTRIBUTE").value_or(0), 0x100);
    CHECK_EQ(sfo.GetInt("BOOTABLE").value_or(0), 1);
    CHECK_TRUE(sfo.GetInt("TITLE").has_value() == false);

    // Rebuild through ParamSfo and verify equivalence.
    std::vector<u8> again = sfo.Build();
    ParamSfo sfo2;
    CHECK_TRUE(sfo2.Parse(again));
    CHECK_STR_EQ(sfo2.GetString("TITLE").value_or(""), "Any Test Game");
    CHECK_EQ(sfo2.GetInt("BOOTABLE").value_or(0), 1);
}

TEST(sfo_rejects_garbage) {
    ParamSfo sfo;
    std::vector<u8> junk(64, 0xAB);
    CHECK_TRUE(!sfo.Parse(junk));
    CHECK_TRUE(!sfo.Parse({}));
}

// A hand-built SFO blob matching the documented layout, decoded field by field.
TEST(sfo_handmade_layout) {
    // entries: TITLE (utf8) + ATTRIBUTE (int)
    std::string key1 = "TITLE";
    std::string key2 = "ATTRIBUTE";
    const char* val1 = "Hello World";
    u32 val2 = 0x12345678;
    struct Entry {
        u16 key_off;
        u16 fmt;
        u32 len, max, off;
    };
    Entry e1{0, 0x0204, 12, 16, 0};
    Entry e2{6, 0x0404, 4, 4, 16};
    u32 key_table = 20 + 2 * 16;
    u32 data_table = key_table + 6 + 10;
    std::vector<u8> blob(data_table + 20, 0);
    auto w16 = [](u8* p, u16 v) { WriteBE16(p, v); };
    auto w32 = [](u8* p, u32 v) { WriteBE32(p, v); };
    w32(blob.data(), 0x00505346);
    w32(blob.data() + 4, 0x101);
    w32(blob.data() + 8, key_table);
    w32(blob.data() + 12, data_table);
    w32(blob.data() + 16, 2);
    u8* e = blob.data() + 20;
    w16(e, e1.key_off); w16(e + 2, e1.fmt); w32(e + 4, e1.len); w32(e + 8, e1.max); w32(e + 12, e1.off);
    e += 16;
    w16(e, e2.key_off); w16(e + 2, e2.fmt); w32(e + 4, e2.len); w32(e + 8, e2.max); w32(e + 12, e2.off);
    std::memcpy(blob.data() + key_table, key1.c_str(), 6);
    std::memcpy(blob.data() + key_table + 6, key2.c_str(), 10);
    std::memcpy(blob.data() + data_table, val1, 12);
    w32(blob.data() + data_table + 16, val2);

    ParamSfo sfo;
    CHECK_TRUE(sfo.Parse(blob));
    CHECK_STR_EQ(sfo.GetString("TITLE").value_or(""), "Hello World");
    CHECK_EQ(sfo.GetInt("ATTRIBUTE").value_or(0), 0x12345678);
}
