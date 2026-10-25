// Any_Ps3 — core types
// AOT-recompilation-based PS3 launcher core (PPU/SPU -> native arm64/x64).
// Honest scaffold: implemented stages are real; unimplemented stages report themselves.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

// Engine version — single source of truth is the VERSION file at the repo
// root; injected by the build systems (see core/include/anyps3/types.h).
#ifndef ANYPS3_VERSION
#define ANYPS3_VERSION "0.0.0-dev"
#endif

namespace any_ps3 {

// Any_Ps3 engine version (also written into cache manifests).
constexpr const char* kEngineVersion = ANYPS3_VERSION;

// Thrown on parse/validation errors. JNI layer catches and reports honestly.
struct Error : std::runtime_error {
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

inline std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

} // namespace any_ps3
