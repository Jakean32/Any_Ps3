// Any_Ps3 — PS3 disc-image encryption layer implementation.
// See any_ps3/disc.h for the format description and legal notes.
#include "any_ps3/disc.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

namespace any_ps3 {

// ---------------------------------------------------------------------------
// AES-128 (FIPS-197). Tables are computed at first use (clean-room: derived
// from the GF(2^8) math, not copied from any listing).
// ---------------------------------------------------------------------------

namespace {

struct AesTables {
    uint8_t sbox[256];
    uint8_t inv_sbox[256];
    uint8_t gmul[2][256];  // [0]: x*2 (xtime), [1]: x*3
    bool init = false;

    static uint8_t XTime(uint8_t a) {
        return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0x00));
    }

    AesTables() {
        // GF(2^8) inverse via exponentiation: generator 3.
        // Build log/antilog tables over 0x03, then inv[a] = 3^(255 - log3[a]).
        uint8_t log3[256];
        uint8_t exp3[256 * 2];
        uint8_t v = 1;
        for (int i = 0; i < 255; i++) {
            exp3[i] = v;
            log3[v] = (uint8_t)i;
            // multiply v by 3 (x+1): v*3 = v*2 xor v
            v = (uint8_t)(XTime(v) ^ v);
        }
        for (int i = 255; i < 512; i++) exp3[i] = exp3[i - 255];

        for (int a = 0; a < 256; a++) {
            uint8_t inv = (a == 0) ? 0 : exp3[255 - log3[a]];
            // S-box = affine transform of the inverse (FIPS-197 eq. 7).
            uint8_t s = inv;
            s = (uint8_t)(s ^ ROTL8(inv, 1) ^ ROTL8(inv, 2) ^ ROTL8(inv, 3) ^
                          ROTL8(inv, 4) ^ 0x63);
            sbox[a] = s;
            inv_sbox[s] = (uint8_t)a;
        }
        for (int a = 0; a < 256; a++) {
            gmul[0][a] = XTime((uint8_t)a);
            gmul[1][a] = (uint8_t)(XTime((uint8_t)a) ^ a);
        }
        init = true;
    }

private:
    static uint8_t ROTL8(uint8_t x, int n) {
        return (uint8_t)((x << n) | (x >> (8 - n)));
    }
};

const AesTables& Tables() {
    static AesTables t;
    return t;
}

inline uint8_t Mul9(uint8_t a) {
    const auto& T = Tables();
    // 9a = 8a ^ 1a; 8a = three xtime steps.
    return (uint8_t)(T.gmul[0][T.gmul[0][T.gmul[0][a]]] ^ a);
}
inline uint8_t MulB(uint8_t a) {
    const auto& T = Tables();
    // 11 = 8 ^ 2 ^ 1
    return (uint8_t)(T.gmul[0][T.gmul[0][T.gmul[0][a]]] ^ T.gmul[0][a] ^ a);
}
inline uint8_t MulD(uint8_t a) {
    const auto& T = Tables();
    // 13 = 8 ^ 4 ^ 1
    return (uint8_t)(T.gmul[0][T.gmul[0][T.gmul[0][a]]] ^ T.gmul[0][T.gmul[0][a]] ^ a);
}
inline uint8_t MulE(uint8_t a) {
    const auto& T = Tables();
    // 14 = 8 ^ 4 ^ 2
    return (uint8_t)(T.gmul[0][T.gmul[0][T.gmul[0][a]]] ^ T.gmul[0][T.gmul[0][a]] ^
                     T.gmul[0][a]);
}

inline uint32_t Rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Rcon[i] = 1 in GF(2^8) successively xtimed.
uint8_t Rcon(int i) {
    uint8_t v = 1;
    for (int k = 0; k < i; k++) v = Tables().XTime(v);
    return v;
}

}  // namespace

void Aes128::SetKey(const uint8_t key[16]) {
    // Key expansion (FIPS-197 sec. 5.2) -> encryption schedule in enc_.
    uint32_t w[44];
    for (int i = 0; i < 4; i++) w[i] = Rd32be(key + 4 * i);
    for (int i = 4; i < 44; i++) {
        uint32_t t = w[i - 1];
        if (i % 4 == 0) {
            uint32_t r = (t << 8) | (t >> 24);  // RotWord
            uint32_t s = ((uint32_t)Tables().sbox[(r >> 24) & 0xFF] << 24) |
                         ((uint32_t)Tables().sbox[(r >> 16) & 0xFF] << 16) |
                         ((uint32_t)Tables().sbox[(r >> 8) & 0xFF] << 8) |
                         (uint32_t)Tables().sbox[r & 0xFF];  // SubWord
            t = s ^ ((uint32_t)Rcon(i / 4 - 1) << 24);
        }
        w[i] = w[i - 4] ^ t;
    }
    std::memcpy(enc_, w, sizeof(enc_));
    // Decryption uses the plain schedule directly (FIPS-197 sec. 5.3 cipher);
    // no modified key schedule needed.
    has_key_ = true;
}

namespace {

inline void AddRoundKey(uint8_t state[16], const uint32_t* w, int round) {
    for (int c = 0; c < 4; c++) {
        uint32_t k = w[round * 4 + c];
        uint8_t kb[4] = {(uint8_t)(k >> 24), (uint8_t)(k >> 16), (uint8_t)(k >> 8),
                         (uint8_t)k};
        // state is column-major: state[c*4 + r]
        for (int r = 0; r < 4; r++) state[c * 4 + r] ^= kb[r];
    }
}

inline void SubBytes(uint8_t state[16]) {
    auto& T = Tables();
    for (int i = 0; i < 16; i++) state[i] = T.sbox[state[i]];
}
inline void InvSubBytes(uint8_t state[16]) {
    auto& T = Tables();
    for (int i = 0; i < 16; i++) state[i] = T.inv_sbox[state[i]];
}
inline void ShiftRows(uint8_t state[16]) {
    uint8_t t;
    // row 1 rotate left 1: elements at odd columns? layout: row r, col c -> state[c*4+r]
    for (int r = 1; r < 4; r++) {
        for (int k = 0; k < r; k++) {
            t = state[0 * 4 + r];
            for (int c = 0; c < 3; c++) state[c * 4 + r] = state[(c + 1) * 4 + r];
            state[3 * 4 + r] = t;
        }
    }
}
inline void InvShiftRows(uint8_t state[16]) {
    uint8_t t;
    for (int r = 1; r < 4; r++) {
        // row r rotates RIGHT by r positions
        for (int k = 0; k < r; k++) {
            t = state[3 * 4 + r];
            for (int c = 3; c > 0; c--) state[c * 4 + r] = state[(c - 1) * 4 + r];
            state[0 * 4 + r] = t;
        }
    }
}
inline void MixColumns(uint8_t state[16]) {
    auto& T = Tables();
    for (int c = 0; c < 4; c++) {
        uint8_t* col = state + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = (uint8_t)(T.gmul[0][a0] ^ T.gmul[1][a1] ^ a2 ^ a3);
        col[1] = (uint8_t)(a0 ^ T.gmul[0][a1] ^ T.gmul[1][a2] ^ a3);
        col[2] = (uint8_t)(a0 ^ a1 ^ T.gmul[0][a2] ^ T.gmul[1][a3]);
        col[3] = (uint8_t)(T.gmul[1][a0] ^ a1 ^ a2 ^ T.gmul[0][a3]);
    }
}
inline void InvMixColumns(uint8_t state[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t* col = state + c * 4;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = (uint8_t)(MulE(a0) ^ MulB(a1) ^ MulD(a2) ^ Mul9(a3));
        col[1] = (uint8_t)(Mul9(a0) ^ MulE(a1) ^ MulB(a2) ^ MulD(a3));
        col[2] = (uint8_t)(MulD(a0) ^ Mul9(a1) ^ MulE(a2) ^ MulB(a3));
        col[3] = (uint8_t)(MulB(a0) ^ MulD(a1) ^ Mul9(a2) ^ MulE(a3));
    }
}

}  // namespace

void Aes128::EncryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t state[16];
    std::memcpy(state, in, 16);
    AddRoundKey(state, enc_, 0);
    for (int round = 1; round < 10; round++) {
        SubBytes(state);
        ShiftRows(state);
        MixColumns(state);
        AddRoundKey(state, enc_, round);
    }
    SubBytes(state);
    ShiftRows(state);
    AddRoundKey(state, enc_, 10);
    std::memcpy(out, state, 16);
}

void Aes128::DecryptBlock(const uint8_t in[16], uint8_t out[16]) const {
    // FIPS-197 sec. 5.3 InvCipher, using the plain encryption schedule.
    uint8_t state[16];
    std::memcpy(state, in, 16);
    AddRoundKey(state, enc_, 10);
    for (int round = 9; round >= 1; round--) {
        InvShiftRows(state);
        InvSubBytes(state);
        AddRoundKey(state, enc_, round);
        InvMixColumns(state);
    }
    InvShiftRows(state);
    InvSubBytes(state);
    AddRoundKey(state, enc_, 0);
    std::memcpy(out, state, 16);
}

void Aes128::CbcDecrypt(uint8_t* data, size_t len, const uint8_t iv[16]) const {
    if (!has_key_ || len % 16 != 0) return;
    uint8_t prev[16];
    std::memcpy(prev, iv, 16);
    uint8_t cur[16];
    for (size_t off = 0; off < len; off += 16) {
        std::memcpy(cur, data + off, 16);
        DecryptBlock(data + off, data + off);
        for (int i = 0; i < 16; i++) data[off + i] ^= prev[i];
        std::memcpy(prev, cur, 16);
    }
}

void Aes128::CbcEncrypt(uint8_t* data, size_t len, const uint8_t iv[16]) const {
    if (!has_key_ || len % 16 != 0) return;
    uint8_t prev[16];
    std::memcpy(prev, iv, 16);
    for (size_t off = 0; off < len; off += 16) {
        for (int i = 0; i < 16; i++) data[off + i] ^= prev[i];
        EncryptBlock(data + off, data + off);
        std::memcpy(prev, data + off, 16);
    }
}

// ---------------------------------------------------------------------------
// Region table
// ---------------------------------------------------------------------------

static uint32_t Be32At(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

std::string DiscEncryptionInfo::Summary() const {
    std::string s;
    if (!table_present) return "no PS3 disc-encryption layer (plain ISO9660)";
    s += "PS3 disc-encryption layer: " + std::to_string(regions.size()) + " region(s)";
    if (is_3k3y) s += " [3k3y image]";
    if (already_decrypted) s += " [marker: decrypted]";
    if (has_embedded_key) s += " [embedded key present]";
    size_t enc = 0;
    uint64_t enc_sectors = 0;
    for (const auto& r : regions) {
        if (r.encrypted) {
            enc++;
            enc_sectors += (uint64_t)r.last - r.first + 1;
        }
    }
    s += ", " + std::to_string(enc) + " encrypted span(s) (" +
         std::to_string(enc_sectors * 2048 / (1024 * 1024)) + " MiB)";
    return s;
}

bool ParseDiscRegionTable(const uint8_t* first_sectors, uint64_t image_bytes,
                         DiscEncryptionInfo* out) {
    *out = DiscEncryptionInfo{};

    const uint8_t* sector0 = first_sectors;
    uint32_t pairs = Be32At(sector0);
    if (pairs == 0 || pairs > 4096) return false;
    uint32_t regions = pairs * 2 - 1;
    if (12 + (uint64_t)regions * 4 > 2048) return false;

    uint64_t total_sectors = (image_bytes + 2047) / 2048;
    std::vector<DiscRegion> rs;
    uint32_t prev_end = 0;  // region 0 starts at sector 0
    for (uint32_t i = 0; i < regions; i++) {
        uint32_t stored_end = Be32At(sector0 + 12 + i * 4);
        bool encrypted = (i % 2) == 1;
        uint32_t end = encrypted ? (stored_end == 0 ? 0 : stored_end - 1) : stored_end;
        // A span must be non-empty-or-empty-at-tip: end >= prev_end - 1.
        if (end + 1 < prev_end) return false;
        DiscRegion r;
        r.first = i == 0 ? 0 : prev_end;
        r.last = end;
        r.encrypted = encrypted && r.last + 1 >= r.first;  // non-empty
        rs.push_back(r);
        // After a plain region the next span starts past its stored boundary;
        // after an ENCRYPTED region the stored boundary sector itself is plain
        // and starts the next span (PS3Dec semantics: end was stored-1).
        prev_end = encrypted ? stored_end : stored_end + 1;
        (void)total_sectors;
    }
    // Strong validation: the last stored end must match the image size.
    uint32_t last_stored = Be32At(sector0 + 12 + (regions - 1) * 4);
    if (total_sectors != 0 && last_stored + 1 != total_sectors) return false;

    out->table_present = true;
    out->region_pairs = pairs;
    out->total_sectors = total_sectors ? (uint32_t)total_sectors : last_stored + 1;
    out->regions = std::move(rs);
    for (const auto& r : out->regions) {
        if (r.encrypted) out->any_encrypted = true;
    }

    // 3k3y marker ("Encrypted 3K ISO" / "Decrypted 3K ISO" at 0xF70).
    if ((sector0[0xF70] == 'e' || sector0[0xF70] == 'E' || sector0[0xF70] == 'd' ||
         sector0[0xF70] == 'D') &&
        std::memcmp(sector0 + 0xF71, "ncrypted 3K ISO", 15) == 0) {
        out->is_3k3y = true;
        out->already_decrypted = (sector0[0xF70] == 'd' || sector0[0xF70] == 'D');
        for (int i = 0; i < 16; i++) {
            if (sector0[0xF80 + i] != 0) {
                out->has_embedded_key = true;
                break;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Disc key parsing / lookup
// ---------------------------------------------------------------------------

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseDiscKey(const std::string& text, uint8_t key[16], std::string* err) {
    // Trim whitespace at both ends.
    std::string t;
    size_t a = text.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        if (err) *err = "empty key";
        return false;
    }
    size_t b = text.find_last_not_of(" \t\r\n");
    t = text.substr(a, b - a + 1);

    // Raw 16-byte binary key.
    if (t.size() == 16) {
        // Ambiguity: 16 chars that are ALL hex could be a truncated hex key —
        // require the text to contain at least one non-hex byte to treat it as
        // binary, otherwise report a bad hex length.
        bool all_hex = true;
        for (char c : t)
            if (HexVal(c) < 0) all_hex = false;
        if (!all_hex || t.find('\n') != std::string::npos) {
            std::memcpy(key, t.data(), 16);
            return true;
        }
        if (err) *err = "hex key must be 32 characters (16 bytes)";
        return false;
    }

    // Hex forms: strip leading 0x and separators (:, -, spaces, newlines).
    std::string hex;
    hex.reserve(t.size());
    for (char c : t) {
        if (c == ':' || c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        hex.push_back(c);
    }
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex.erase(0, 2);
    if (hex.size() != 32 && hex.size() != 64) {
        if (err)
            *err = "key must be 32 hex characters (16 bytes); got " +
                   std::to_string(hex.size());
        return false;
    }
    for (int i = 0; i < 16; i++) {
        int hi = HexVal(hex[i * 2]);
        int lo = HexVal(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            if (err) *err = "invalid hex in key";
            return false;
        }
        key[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

std::vector<std::string> DiscKeyFileCandidates(const std::string& iso_path) {
    std::vector<std::string> out;
    if (iso_path.empty()) return out;
    size_t slash = iso_path.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? "" : iso_path.substr(0, slash + 1);
    std::string name = slash == std::string::npos ? iso_path : iso_path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    std::string base = dot == std::string::npos ? name : name.substr(0, dot);

    for (const char* ext : {".dkey", ".key", ".DKEY", ".KEY"}) {
        out.push_back(dir + base + ext);
    }
    for (const char* ext : {".dkey", ".key"}) {
        out.push_back(dir + name + ext);
    }
    return out;
}

// ---------------------------------------------------------------------------
// DiscCrypto
// ---------------------------------------------------------------------------

void DiscCrypto::SectorIv(uint32_t sector, uint8_t iv[16]) {
    std::memset(iv, 0, 12);
    iv[12] = (uint8_t)(sector >> 24);
    iv[13] = (uint8_t)(sector >> 16);
    iv[14] = (uint8_t)(sector >> 8);
    iv[15] = (uint8_t)(sector);
}

bool DiscCrypto::IntersectsEncrypted(uint64_t byte_offset, uint64_t len) const {
    uint32_t first = (uint32_t)(byte_offset / 2048);
    uint32_t last = (uint32_t)((byte_offset + len - 1) / 2048);
    for (const auto& r : info_.regions) {
        if (!r.encrypted) continue;
        if (last >= r.first && first <= r.last) return true;
    }
    return false;
}

void DiscCrypto::DecryptSectors(uint32_t lba, uint8_t* sectors, uint32_t count) const {
    if (!aes_.valid() || !info_.table_present || count == 0) return;
    uint32_t idx = lba;
    for (const auto& r : info_.regions) {
        if (idx >= count + lba) break;
        if (!r.encrypted) continue;
        uint32_t lo = std::max(idx, r.first);
        uint32_t hi = std::min(lba + count - 1, r.last);
        if (lo > hi) continue;
        for (uint32_t s = lo; s <= hi; s++) {
            uint8_t iv[16];
            SectorIv(s, iv);
            aes_.CbcDecrypt(sectors + (size_t)(s - lba) * 2048, 2048, iv);
        }
    }
}

}  // namespace any_ps3
