#pragma once
// Portable utilities: SHA-256, paths, fs helpers, free space.
#include <cstdint>
#include <string>
#include <vector>

namespace anyps3 {

// ---------- SHA-256 (self-contained, FIPS 180-4) ----------
class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::string hex();  // 64 hex chars (finalizes)
    static std::string hex_of(const void* data, size_t len);
    static std::string hex_of_str(const std::string& s) { return hex_of(s.data(), s.size()); }

private:
    void process_block(const uint8_t* p);
    uint32_t h_[8];
    uint64_t total_;
    uint8_t buf_[64];
    size_t buflen_;
};

// ---------- time ----------
int64_t now_ms();

// ---------- strings ----------
std::string to_lower(std::string s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);

// ---------- paths (always '/' internally; Windows accepts both) ----------
std::string path_join(const std::string& a, const std::string& b);
std::string path_filename(const std::string& p);
std::string path_dirname(const std::string& p);
std::string path_extension(const std::string& p);   // includes '.', lowercased

// ---------- fs ----------
bool file_exists(const std::string& p);
bool dir_exists(const std::string& p);
uint64_t file_size(const std::string& p);           // 0 if missing
bool make_dirs(const std::string& p);               // mkdir -p
bool read_file(const std::string& p, std::vector<uint8_t>* out);
bool write_file(const std::string& p, const void* data, size_t n);
bool list_dir(const std::string& dir, std::vector<std::string>* out);  // names only
bool remove_tree(const std::string& p);
// Available space (bytes) at the filesystem containing `path`. 0 on error.
uint64_t free_space_bytes(const std::string& path);

}  // namespace anyps3
