// Any_Ps3 — SHA-256 (FIPS 180-4). Used for source hashes in cache manifests.
#pragma once

#include "types.h"

namespace any_ps3 {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    void final(uint8_t out[32]);
    std::string final_hex();

    // Convenience: hash a buffer.
    static std::string hex_of(const void* data, size_t len);

private:
    void compress(const uint8_t block[64]);
    uint32_t h_[8];
    uint64_t total_;
    uint8_t buf_[64];
    size_t buf_len_;
};

// CRC-32 (IEEE 802.3, reflected 0xEDB88320). Light integrity helper.
uint32_t crc32_of(const void* data, size_t len);

} // namespace any_ps3
