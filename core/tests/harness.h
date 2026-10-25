// Any_Ps3 test harness — tiny, dependency-free.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>

namespace any_ps3 {

struct TestState {
    const char* name = "";
    int failures = 0;
    std::string first_failure;

    void fail(const char* what, const char* file, int line) {
        if (failures == 0) {
            char buf[512];
            std::snprintf(buf, sizeof buf, "%s:%d: %s", file, line, what);
            first_failure = buf;
        }
        failures++;
    }
    void check(bool cond, const char* what, const char* file, int line) {
        if (!cond) fail(what, file, line);
    }
};

struct TestCase {
    const char* name;
    void (*fn)(TestState&);
};

inline int run_tests(const TestCase* cases, size_t n) {
    int pass = 0, fail = 0;
    for (size_t i = 0; i < n; i++) {
        TestState st;
        st.name = cases[i].name;
        cases[i].fn(st);
        if (st.failures == 0) {
            std::printf("  [PASS] %s\n", cases[i].name);
            pass++;
        } else {
            std::printf("  [FAIL] %s\n            %s\n", cases[i].name, st.first_failure.c_str());
            fail++;
        }
    }
    std::printf("\n%d passed, %d failed, %d total\n", pass, fail, (int)n);
    return fail == 0 ? 0 : 1;
}

#define AP_CHECK(cond) st.check((cond), #cond, __FILE__, __LINE__)
#define AP_CHECK_EQ(a, b) st.check((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define AP_TEST(fn) { #fn, fn }

} // namespace any_ps3
