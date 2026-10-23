#include "util.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/statvfs.h>
#include <unistd.h>
#else
#include <direct.h>
#include <io.h>
#include <windows.h>
#define R_OK 4
#endif

namespace anyps3 {

// ================= SHA-256 =================
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

Sha256::Sha256() : total_(0), buflen_(0) {
    h_[0] = 0x6a09e667; h_[1] = 0xbb67ae85; h_[2] = 0x3c6ef372; h_[3] = 0xa54ff53a;
    h_[4] = 0x510e527f; h_[5] = 0x9b05688c; h_[6] = 0x1f83d9ab; h_[7] = 0x5be0cd19;
}

void Sha256::process_block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
               (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
}

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    total_ += len;
    while (len > 0) {
        size_t take = std::min(len, sizeof(buf_) - buflen_);
        memcpy(buf_ + buflen_, p, take);
        buflen_ += take; p += take; len -= take;
        if (buflen_ == sizeof(buf_)) { process_block(buf_); buflen_ = 0; }
    }
}

std::string Sha256::hex() {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (buflen_ != 56) update(&zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = uint8_t(bits >> (56 - i * 8));
    // bypass total_ increment for length bytes: write directly
    memcpy(buf_ + buflen_, lenb, 8);
    process_block(buf_);
    static const char* hx = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; i++)
        for (int j = 28; j >= 0; j -= 4) out += hx[(h_[i] >> j) & 0xF];
    return out;
}

std::string Sha256::hex_of(const void* data, size_t len) {
    Sha256 s;
    s.update(data, len);
    return s.hex();
}

// ================= time =================
int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// ================= strings =================
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)tolower(c); });
    return s;
}
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (true) {
        size_t nxt = s.find(sep, pos);
        if (nxt == std::string::npos) { out.push_back(s.substr(pos)); break; }
        out.push_back(s.substr(pos, nxt - pos));
        pos = nxt + 1;
    }
    return out;
}

// ================= paths =================
static std::string normalize_path(std::string p) {
#ifdef _WIN32
    std::replace(p.begin(), p.end(), '\\', '/');
#endif
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}
std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return normalize_path(b);
    if (b.empty()) return normalize_path(a);
    if (!b.empty() && (b[0] == '/' || (b.size() > 1 && b[1] == ':'))) return normalize_path(b);
    return normalize_path(a + "/" + b);
}
std::string path_filename(const std::string& p) {
    std::string n = normalize_path(p);
    size_t s = n.rfind('/');
    return s == std::string::npos ? n : n.substr(s + 1);
}
std::string path_dirname(const std::string& p) {
    std::string n = normalize_path(p);
    size_t s = n.rfind('/');
    if (s == std::string::npos) return "";
    return s == 0 ? "/" : n.substr(0, s);
}
std::string path_extension(const std::string& p) {
    std::string f = path_filename(p);
    size_t d = f.rfind('.');
    return d == std::string::npos ? "" : to_lower(f.substr(d));
}

// ================= fs =================
bool file_exists(const std::string& p) {
    if (p.empty()) return false;
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool dir_exists(const std::string& p) {
    if (p.empty()) return false;
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
uint64_t file_size(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return (uint64_t)st.st_size;
}

bool make_dirs(const std::string& p) {
    if (p.empty() || dir_exists(p)) return p.empty() ? false : true;
    std::string n = normalize_path(p);
    std::string cur;
    std::vector<std::string> parts = split(n, '/');
    for (size_t i = 0; i < parts.size(); i++) {
        if (parts[i].empty()) { if (n[0] == '/') cur = "/"; continue; }
        cur = cur.empty() ? parts[i] : (cur == "/" ? "/" + parts[i] : cur + "/" + parts[i]);
#ifdef _WIN32
        if (i == 0 && n.size() > 1 && n[1] == ':') { cur += "/"; continue; }  // drive letter
#endif
        if (!dir_exists(cur)) {
#ifdef _WIN32
            if (_mkdir(cur.c_str()) != 0 && errno != EEXIST) return false;
#else
            if (mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) return false;
#endif
        }
    }
    return dir_exists(n) || file_exists(n);
}

bool read_file(const std::string& p, std::vector<uint8_t>* out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return false; }
    out->resize((size_t)n);
    size_t got = n ? fread(out->data(), 1, (size_t)n, f) : 0;
    fclose(f);
    return got == (size_t)n;
}
bool write_file(const std::string& p, const void* data, size_t n) {
    std::string d = path_dirname(p);
    if (!d.empty() && !make_dirs(d)) return false;
    FILE* f = fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = n ? fwrite(data, 1, n, f) == n : true;
    fclose(f);
    return ok;
}

bool list_dir(const std::string& dir, std::vector<std::string>* out) {
    out->clear();
#ifndef _WIN32
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        out->push_back(n);
    }
    closedir(d);
#else
    std::string pat = dir + "/*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        std::string n = fd.cFileName;
        if (n != "." && n != "..") out->push_back(n);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
    return true;
}

bool remove_tree(const std::string& p) {
    if (p.empty() || p == "/" || p.size() < 5) return false;  // safety guard
    if (dir_exists(p)) {
        std::vector<std::string> names;
        if (!list_dir(p, &names)) return false;
        for (const std::string& n : names) remove_tree(path_join(p, n));
#ifdef _WIN32
        return _rmdir(p.c_str()) == 0;
#else
        return rmdir(p.c_str()) == 0;
#endif
    }
    return remove(p.c_str()) == 0;
}

uint64_t free_space_bytes(const std::string& path) {
    std::string probe = dir_exists(path) ? path : path_dirname(path);
    // walk up until an existing directory is found (handles deep paths)
    while (!probe.empty() && !dir_exists(probe)) {
        std::string parent = path_dirname(probe);
        if (parent == probe) break;
        probe = parent;
    }
    if (probe.empty()) probe = ".";
#ifndef _WIN32
    struct statvfs st;
    if (statvfs(probe.c_str(), &st) != 0) return 0;
    return (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
#else
    ULARGE_INTEGER avail;
    if (!GetDiskFreeSpaceExA(probe.c_str(), &avail, nullptr, nullptr)) return 0;
    return (uint64_t)avail.QuadPart;
#endif
}

}  // namespace anyps3
