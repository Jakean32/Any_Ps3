// Any_Ps3 — minimal dependency-free test framework.
// TEST(name) registers a function; test_main runs all of them and prints a summary.
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace anyps3test {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        Registry().push_back({name, std::move(fn)});
    }
};

inline int g_failures = 0;
inline const char* g_current = "";

inline void ReportFail(const char* file, int line, const std::string& msg) {
    std::fprintf(stderr, "  FAIL %s:%d [%s] %s\n", file, line, g_current, msg.c_str());
    g_failures++;
}

template <typename A, typename B>
void CheckEq(const char* file, int line, const A& a, const B& b, const char* what) {
    if (!(a == b)) {
        std::string msg = std::string(what) + " (" + std::to_string(static_cast<long long>(a)) +
                          " != " + std::to_string(static_cast<long long>(b)) + ")";
        ReportFail(file, line, msg);
    }
}

inline void CheckTrue(const char* file, int line, bool v, const char* what) {
    if (!v) ReportFail(file, line, what);
}

inline void CheckStrEq(const char* file, int line, const std::string& a, const std::string& b,
                       const char* what) {
    if (a != b) {
        ReportFail(file, line, std::string(what) + " ('" + a + "' != '" + b + "')");
    }
}

}  // namespace anyps3test

#define TEST(name)                                                          \
    static void anyps3_test_##name();                                       \
    static ::anyps3test::Registrar anyps3_reg_##name(#name, &anyps3_test_##name); \
    static void anyps3_test_##name()

#define CHECK_EQ(a, b) ::anyps3test::CheckEq(__FILE__, __LINE__, (a), (b), #a " == " #b)
#define CHECK_TRUE(v) ::anyps3test::CheckTrue(__FILE__, __LINE__, (v), #v)
#define CHECK_STR_EQ(a, b) ::anyps3test::CheckStrEq(__FILE__, __LINE__, (a), (b), #a " == " #b)
#define CHECK_FLOAT_EQ(a, b) \
    do { double _x = (a), _y = (b); \
         if (std::fabs(_x - _y) > 1e-6) ::anyps3test::ReportFail(__FILE__, __LINE__, #a " == " #b); \
    } while (0)
