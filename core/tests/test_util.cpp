// Tests: util (bytes/fd/sha256/crc32), json.
#include "harness.h"
#include "testdata.h"
#include "any_ps3/sha256.h"
#include "any_ps3/json.h"

#include <unistd.h>
#include <fcntl.h>
#include <cstdio>

namespace any_ps3 {

static void bytes_endian(TestState& st) {
    uint8_t b4[] = {0x12, 0x34, 0x56, 0x78};
    uint8_t b8[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    AP_CHECK_EQ(rd32be(b4), 0x12345678u);
    AP_CHECK_EQ(rd32le(b4), 0x78563412u);
    AP_CHECK_EQ(rd16be(b4), 0x1234u);
    AP_CHECK_EQ(rd16le(b4), 0x3412u);
    AP_CHECK_EQ(rd64be(b8), 0x0123456789ABCDEFull);
    AP_CHECK_EQ(rd64le(b8), 0xEFCDAB8967452301ull);
}

static void fdsource_random_access(TestState& st) {
    // 8 MiB of patterned data in a temp file, read via fd at random offsets.
    std::vector<uint8_t> data(8u * 1024 * 1024);
    for (size_t i = 0; i < data.size(); i++) data[i] = (uint8_t)(i * 7 + 3);
    std::string path = "/tmp/anyps3_fdtest.bin";
    AP_CHECK(write_file_path(path, data.data(), data.size()));

    int fd = ::open(path.c_str(), O_RDONLY);
    AP_CHECK(fd >= 0);
    {
        FdSource src(fd);
        AP_CHECK_EQ(src.size(), (uint64_t)data.size());
        uint8_t buf[64];
        AP_CHECK(src.read_exact(0, buf, 64));
        AP_CHECK_EQ(buf[7], data[7]);
        AP_CHECK(src.read_exact(data.size() - 64, buf, 64));
        AP_CHECK_EQ(buf[63], data.back());
        // odd offset, unaligned len
        AP_CHECK(src.read_exact(1234567, buf, 37));
        for (int k = 0; k < 37; k++) AP_CHECK_EQ(buf[k], data[1234567 + k]);
        // EOF behaviour
        AP_CHECK_EQ(src.read_at(data.size() - 10, buf, 64), (size_t)10);
        AP_CHECK_EQ(src.read_at(data.size(), buf, 64), (size_t)0);
    }
    ::close(fd);
    ::unlink(path.c_str());
}

static void fdwriter_roundtrip(TestState& st) {
    std::string path = "/tmp/anyps3_wtest.bin";
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    AP_CHECK(fd >= 0);
    {
        FdWriter w(fd);
        AP_CHECK(w.write_str("hello "));
        AP_CHECK(w.write_all("fd world", 8));
    }
    ::lseek(fd, 0, SEEK_SET);
    char buf[32] = {0};
    AP_CHECK_EQ((size_t)::read(fd, buf, 14), (size_t)14);
    AP_CHECK(std::string(buf, 14) == "hello fd world");
    ::close(fd);
    ::unlink(path.c_str());
}

static void sha256_nist_vectors(TestState& st) {
    AP_CHECK_EQ(Sha256::hex_of("abc", 3),
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    AP_CHECK_EQ(Sha256::hex_of("", 0),
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    AP_CHECK_EQ(Sha256::hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56),
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // streaming in chunks equals one-shot
    Sha256 s;
    std::string long_data(10000, 'x');
    s.update(long_data.data(), 3333);
    s.update(long_data.data() + 3333, 6667);
    AP_CHECK_EQ(s.final_hex(), Sha256::hex_of(long_data.data(), long_data.size()));
}

static void crc32_check(TestState& st) {
    AP_CHECK_EQ(crc32_of("123456789", 9), 0xCBF43926u);
    AP_CHECK_EQ(crc32_of("", 0), 0u);
}

static void json_roundtrip(TestState& st) {
    JsonValue j = JsonValue::make_obj();
    j.set("name", JsonValue::make_str("Any_Ps3 \"core\""));
    j.set("count", JsonValue::make_int(-42));
    j.set("flag", JsonValue::make_bool(true));
    JsonValue arr = JsonValue::make_arr();
    arr.arr.push_back(JsonValue::make_int(1));
    arr.arr.push_back(JsonValue::make_str("two"));
    JsonValue inner = JsonValue::make_obj();
    inner.set("k", JsonValue::make_int(9));
    arr.arr.push_back(std::move(inner));
    j.set("list", std::move(arr));

    std::string text = j.dump();
    JsonValue back;
    std::string err;
    AP_CHECK(JsonValue::parse(text, back, err));
    AP_CHECK(err.empty());
    AP_CHECK_EQ(back.get_str("name"), std::string("Any_Ps3 \"core\""));
    AP_CHECK_EQ(back.get_int("count"), (long long)-42);
    AP_CHECK(back.get_bool("flag"));
    AP_CHECK_EQ(back.find("list")->arr.size(), (size_t)3);
    AP_CHECK_EQ(back.find("list")->arr[1].s, std::string("two"));
    AP_CHECK_EQ(back.find("list")->arr[2].get_int("k"), (long long)9);
}

static void json_escapes(TestState& st) {
    JsonValue j = JsonValue::make_obj();
    j.set("s", JsonValue::make_str("line1\nline2\ttab \x01\x02"));
    std::string text = j.dump();
    JsonValue back;
    std::string err;
    AP_CHECK(JsonValue::parse(text, back, err));
    AP_CHECK(back.get_str("s") == std::string("line1\nline2\ttab \x01\x02"));
    // unicode escape decodes to UTF-8
    AP_CHECK(JsonValue::parse("{\"u\":\"\\u00e4\\u00f6\\u00fc\"}", back, err));
    AP_CHECK(back.get_str("u") == "\xc3\xa4\xc3\xb6\xc3\xbc"); // äöü
}

static void json_rejects_bad(TestState& st) {
    JsonValue v;
    std::string err;
    AP_CHECK(!JsonValue::parse("{\"a\": 1.5}", v, err)); // floats rejected by design
    AP_CHECK(!JsonValue::parse("{\"a\": 1} trailing", v, err));
    AP_CHECK(!JsonValue::parse("{\"a\": ", v, err));
    AP_CHECK(!JsonValue::parse("[1, 2", v, err));
    AP_CHECK(!JsonValue::parse("{\"a\": 1,}", v, err));
}

static const TestCase kTests[] = {
    AP_TEST(bytes_endian),
    AP_TEST(fdsource_random_access),
    AP_TEST(fdwriter_roundtrip),
    AP_TEST(sha256_nist_vectors),
    AP_TEST(crc32_check),
    AP_TEST(json_roundtrip),
    AP_TEST(json_escapes),
    AP_TEST(json_rejects_bad),
};

const TestCase* util_tests() { return kTests; }
size_t util_tests_count() { return sizeof kTests / sizeof kTests[0]; }

} // namespace any_ps3
