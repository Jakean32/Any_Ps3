#include "any_ps3/source.h"
#include "any_ps3/sha256.h"

#include <algorithm>
#include <cctype>

namespace any_ps3 {

std::string game_rel_path(const char* rel) {
    return std::string("PS3_GAME/") + rel;
}

GameMeta GameSource::meta(std::string* err) {
    if (meta_loaded_) {
        if (err && !meta_.valid) *err = "PARAM.SFO unreadable on last attempt";
        return meta_;
    }
    meta_loaded_ = true;
    std::vector<uint8_t> sfo;
    const size_t cap = 1024 * 1024;
    if (read_file("PARAM.SFO", sfo, cap)) {
        try {
            SfoData d = parse_sfo(sfo.data(), sfo.size());
            meta_.title = d.title();
            meta_.title_id = d.title_id();
            meta_.category = d.category();
            meta_.app_ver = d.app_ver();
            meta_.system_ver = d.system_ver();
            if (meta_.title_id.empty()) {
                if (err) *err = "PARAM.SFO has no TITLE_ID";
                meta_.valid = false;
            } else {
                meta_.valid = true;
            }
        } catch (const Error& e) {
            if (err) *err = e.what();
            meta_.valid = false;
        }
    } else {
        if (err) *err = "PARAM.SFO not found in source";
        meta_.valid = false;
    }
    return meta_;
}

// ---- ISO source ----

std::unique_ptr<IsoFileSource> IsoFileSource::open(std::unique_ptr<FdSource> fd, std::string& err) {
    auto src = std::unique_ptr<IsoFileSource>(new IsoFileSource(std::move(fd)));
    if (!src->iso_.open()) {
        err = src->iso_.error();
        return nullptr;
    }
    return src;
}

bool IsoFileSource::read_file(const char* rel, std::vector<uint8_t>& out, size_t cap) {
    return iso_.read_file(game_rel_path(rel), out, cap);
}

bool IsoFileSource::copy_entry_to_fd(const char* rel, int out_fd) {
    IsoDirEntry e;
    if (!iso_.stat(game_rel_path(rel), e)) {
        err_ = iso_.error();
        return false;
    }
    if (e.is_dir) { err_ = "entry is a directory"; return false; }
    FdWriter w(out_fd);
    std::vector<uint8_t> buf(256 * 1024);
    uint64_t off = 0;
    while (off < e.size) {
        uint64_t chunk = std::min<uint64_t>(buf.size(), e.size - off);
        if (!iso_.read_entry_range(e, off, chunk, buf.data())) {
            err_ = iso_.error();
            return false;
        }
        if (!w.write_all(buf.data(), (size_t)chunk)) { err_ = "write failed"; return false; }
        off += chunk;
    }
    return true;
}

std::string IsoFileSource::source_hash(std::function<void(float)> progress) {
    // Stream the whole image through SHA-256 in 1 MiB chunks. The image is
    // never fully resident in RAM.
    Sha256 h;
    uint64_t sz = iso_.image_size();
    std::vector<uint8_t> buf(1024 * 1024);
    uint64_t off = 0;
    while (off < sz) {
        uint64_t want = std::min<uint64_t>(buf.size(), sz - off);
        size_t got = iso_.raw_read(off, buf.data(), (size_t)want);
        if (got == 0) break;
        h.update(buf.data(), got);
        off += got;
        if (progress) progress((float)((double)off / (double)sz));
    }
    return h.final_hex();
}

// ---- Folder source ----

std::unique_ptr<FolderSource> FolderSource::open(std::string root, std::string& err) {
    namespace fs = std::filesystem;
    fs::path p(root);
    std::error_code ec;
    if (!fs::exists(p, ec) || !fs::is_directory(p, ec)) {
        err = "not a directory: " + root;
        return nullptr;
    }
    auto src = std::unique_ptr<FolderSource>(new FolderSource(std::move(root)));
    fs::path game = p / "PS3_GAME";
    src->game_dir_ = fs::exists(game, ec) ? game.string() : p.string();
    return src;
}

bool FolderSource::read_file(const char* rel, std::vector<uint8_t>& out, size_t cap) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p = fs::path(game_dir_) / rel;
    if (!fs::is_regular_file(p, ec)) return false;
    return read_file_path(p.string(), out, cap);
}

uint64_t FolderSource::source_size() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(fs::path(root_), ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) total += (uint64_t)it->file_size(ec);
    }
    return total;
}

std::string FolderSource::source_hash(std::function<void(float)> progress) {
    // Deterministic signature (mtimes deliberately excluded):
    //   sha256( sorted "relpath\0size\n" lines || PARAM.SFO bytes || first 4 MiB of EBOOT.BIN )
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<std::pair<std::string, uint64_t>> files;
    for (auto it = fs::recursive_directory_iterator(fs::path(root_), ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            std::string rel = fs::relative(it->path(), fs::path(root_), ec).string();
            std::string low;
            for (char c : rel) low.push_back((char)tolower((unsigned char)c));
            files.emplace_back(low, (uint64_t)it->file_size(ec));
        }
    }
    std::sort(files.begin(), files.end());
    Sha256 h;
    for (auto& kv : files) {
        std::string line = kv.first + '\0' + std::to_string(kv.second) + "\n";
        h.update(line.data(), line.size());
    }
    std::vector<uint8_t> key;
    if (read_file("PARAM.SFO", key, 1024 * 1024)) h.update(key.data(), key.size());
    if (read_file("EBOOT.BIN", key, 4 * 1024 * 1024)) h.update(key.data(), key.size());
    if (progress) progress(1.0f);
    return h.final_hex();
}

} // namespace any_ps3
