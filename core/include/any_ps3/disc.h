// Any_Ps3 — PS3 disc-image encryption layer (clean-room, from the publicly
// documented PS3Dec/3k3y format).
//
// Format (validated against the reference PS3Dec r5 tool source, WTFPL):
//   * Sector 0 (first 2048 bytes) holds the region table:
//       BE32 @ 0x00 : N — the image has regions = 2*N - 1 (always odd)
//       BE32 @ 0x0C : region end-sector array, `regions` entries
//                     (2048-byte sector indices)
//   * Regions alternate PLAIN / ENCRYPTED, starting with PLAIN at sector 0.
//   * Stored end values: even (plain) regions end at their stored value
//     (inclusive); odd (encrypted) regions end at stored value MINUS ONE —
//     the stored boundary sector itself stays plain.
//   * Each encrypted 2048-byte sector s is an independent AES-128-CBC
//     ciphertext: IV = 12 zero bytes || big-endian u32(s), key = the 16-byte
//     disc key. CBC runs only inside the sector ("no padding" mode).
//   * 3k3y-produced images additionally carry a marker at 0xF70 ('e' when
//     encrypted, 'd' when decrypted) and the 16-byte key at 0xF80.
//
// LEGAL: Any_Ps3 ships NO keys and NO key databases. All keys arrive from
// the user (a .dkey/.key file next to the ISO, or the per-game settings).
#pragma once

#include "types.h"

#include <string>
#include <vector>

namespace any_ps3 {

// ---- clean-room AES-128 (FIPS-197) ----
class Aes128 {
public:
    Aes128() = default;
    explicit Aes128(const uint8_t key[16]) { SetKey(key); }

    void SetKey(const uint8_t key[16]);
    bool valid() const { return has_key_; }

    // CBC mode (no padding; len must be a multiple of 16), in place.
    void CbcDecrypt(uint8_t* data, size_t len, const uint8_t iv[16]) const;
    void CbcEncrypt(uint8_t* data, size_t len, const uint8_t iv[16]) const;

    // Single-block primitives (used by tests and by the fixture writer).
    void EncryptBlock(const uint8_t in[16], uint8_t out[16]) const;
    void DecryptBlock(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint32_t enc_[44] = {};   // encryption schedule
    uint32_t dec_[44] = {};   // decryption schedule (reversed + InvMixColumns)
    bool has_key_ = false;
};

// ---- region table ----
struct DiscRegion {
    uint32_t first = 0;   // first 2048-byte sector (inclusive)
    uint32_t last = 0;    // last 2048-byte sector (inclusive)
    bool encrypted = false;
};

struct DiscEncryptionInfo {
    bool table_present = false;    // plausible region table found at sector 0
    bool is_3k3y = false;          // 3k3y marker at 0xF70 seen
    bool already_decrypted = false; // 3k3y marker says 'd' (decrypted 3K image)
    bool has_embedded_key = false;  // 3k3y key present at 0xF80
    bool any_encrypted = false;    // at least one non-empty encrypted region
    uint32_t region_pairs = 0;     // N as stored at sector 0
    uint32_t total_sectors = 0;    // from the last end entry (+1)
    std::vector<DiscRegion> regions;

    // Human-readable one-liner for logs.
    std::string Summary() const;
};

// Parses and validates the region table out of the first bytes of the image
// (the table itself lives in the first 2048-byte sector; the 3k3y marker at
// 0xF70 / embedded key at 0xF80 live in the second sector, so `first_sectors`
// must cover at least 4096 bytes). Returns false when the bytes are not a
// table.
bool ParseDiscRegionTable(const uint8_t* first_sectors, uint64_t image_bytes,
                          DiscEncryptionInfo* out);

// ---- disc key loading ----
// Accepts (case-insensitive, whitespace/colon/0x tolerant):
//   * 32 hex characters  -> 16 bytes (the Redump .dkey format)
//   * 64 hex characters  -> 32 bytes, first 16 used (some ODE dumps)
//   * exactly 16 raw bytes (binary .dkey files)
// Returns false + err on anything else.
bool ParseDiscKey(const std::string& text, uint8_t key[16], std::string* err);

// Sibling key-file candidates for an ISO path, best guess first:
//   game.iso -> game.dkey, game.key, game.DKEY, game.KEY,
//               game.iso.dkey, game.iso.key
std::vector<std::string> DiscKeyFileCandidates(const std::string& iso_path);

// ---- per-image decryption ----
class DiscCrypto {
public:
    DiscCrypto() = default;

    void SetInfo(const DiscEncryptionInfo& info) { info_ = info; }
    const DiscEncryptionInfo& info() const { return info_; }

    void SetKey(const uint8_t key[16]) { aes_.SetKey(key); }
    bool has_key() const { return aes_.valid(); }
    bool NeedsKey() const { return info_.any_encrypted && !aes_.valid(); }

    // True when any part of [byte_offset, +len) lies in an encrypted region.
    bool IntersectsEncrypted(uint64_t byte_offset, uint64_t len) const;

    // Decrypts `count` consecutive 2048-byte sectors starting at absolute
    // sector index `lba`, in place. Plain-region sectors are left untouched.
    // Without a key this is a no-op (caller sees ciphertext, by design).
    void DecryptSectors(uint32_t lba, uint8_t* sectors, uint32_t count) const;

    // IV used for sector s: 12 zero bytes + BE32(s). Exposed for tests.
    static void SectorIv(uint32_t sector, uint8_t iv[16]);

private:
    DiscEncryptionInfo info_;
    Aes128 aes_;
};

}  // namespace any_ps3
