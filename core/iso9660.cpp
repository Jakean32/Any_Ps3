#include "iso9660.h"

#include <cstring>

#include "util.h"

namespace anyps3 {

static const size_t kSector = 2048;

static uint32_t rd32le(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static uint32_t rd32be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// strips ";1"-style version suffix and lowercases
static std::string canon_name(const uint8_t* name, uint8_t len) {
    std::string s((const char*)name, len);
    size_t semi = s.find(';');
    if (semi != std::string::npos) s.resize(semi);
    // strip trailing dot from "NAME." forms
    while (!s.empty() && s.back() == '.') s.pop_back();
    return to_lower(s);
}

std::shared_ptr<Iso9660> Iso9660::open(std::shared_ptr<ByteSource> src, std::string* err) {
    if (!src) {
        if (err) *err = "null source";
        return nullptr;
    }
    auto out = std::shared_ptr<Iso9660>(new Iso9660());
    out->src_ = std::move(src);

    // Primary Volume Descriptor at LBA 16
    uint8_t pvd[kSector];
    if (!out->src_->read(16 * kSector, kSector, pvd)) {
        if (err) *err = "cannot read volume descriptor at LBA 16 (missing CD001 PVD; too small or unreadable)";
        return nullptr;
    }
    if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) {
        if (err) *err = "not an ISO9660 image (missing CD001 PVD)";
        return nullptr;
    }
    char vid[33];
    memcpy(vid, pvd + 40, 32);
    vid[32] = 0;
    out->volume_id_ = trim(std::string(vid));

    // Root directory record at offset 156 inside the PVD sector
    const uint8_t* root = pvd + 156;
    if (root[0] < 34) {
        if (err) *err = "malformed root directory record";
        return nullptr;
    }
    out->root_.name_l = "";
    out->root_.lba = rd32le(root + 2);
    out->root_.size = rd32le(root + 10);
    out->root_.is_dir = true;
    // sanity: also check BE copies agree
    uint32_t lba_be = rd32be(root + 6);
    if (lba_be != 0 && lba_be != out->root_.lba) {
        if (err) *err = "endian-mismatched root record (corrupt image?)";
        return nullptr;
    }
    if (!out->index_dir(out->root_, err, 0)) return nullptr;
    return out;
}

bool Iso9660::index_dir(const Entry& dir, std::string* err, int depth) {
    if (depth > 8) {
        if (err) *err = "directory nesting too deep";
        return false;
    }
    if (dir.size == 0 || dir.size > 64 * 1024 * 1024) {
        if (err) *err = "unreasonable directory size";
        return false;
    }
    // directory data spans ceil(size/2048) sectors; records never cross
    // sector boundaries
    const size_t sectors = (size_t)((dir.size + kSector - 1) / kSector);
    std::vector<uint8_t> buf(sectors * kSector);
    for (size_t i = 0; i < sectors; i++) {
        if (!src_->read(dir.lba * kSector + i * kSector, kSector, buf.data() + i * kSector)) {
            if (err) *err = "directory read error";
            return false;
        }
    }
    size_t pos = 0;
    while (pos < dir.size) {
        uint8_t reclen = buf[pos];
        if (reclen == 0) {
            // advance to next sector boundary
            pos += kSector - (pos % kSector);
            continue;
        }
        if (pos + reclen > buf.size()) break;
        const uint8_t* r = buf.data() + pos;
        uint8_t flags = r[25];
        uint8_t name_len = r[32];
        const uint8_t* name = r + 33;
        bool is_dir = (flags & 0x02) != 0;
        // ISO9660 encodes "." as name byte 0x00 and ".." as 0x01
        if (name_len == 1 && (name[0] == 0 || name[0] == 1)) {
            pos += reclen;
            continue;
        }
        std::string cname = canon_name(name, name_len);
        // skip empty names
        if (cname.empty()) {
            pos += reclen;
            continue;
        }
        // multi-extent files (bit 0x80): not needed for PS3 metadata files
        Entry child;
        child.name_l = cname;
        child.lba = rd32le(r + 2);
        child.size = rd32le(r + 10);
        child.is_dir = is_dir;
        if (is_dir) {
            if (!index_dir(child, err, depth + 1)) return false;
        }
        const_cast<Entry&>(dir).children[cname] = std::move(child);
        pos += reclen;
    }
    return true;
}

static const Iso9660::Entry* find_entry(const Iso9660::Entry* root,
                                        const std::vector<std::string>& parts,
                                        size_t i) {
    if (i == parts.size()) return root;
    auto it = root->children.find(parts[i]);
    if (it == root->children.end()) return nullptr;
    return find_entry(&it->second, parts, i + 1);
}

IsoExtent Iso9660::lookup(const std::string& path) const {
    std::string p = to_lower(trim(path));
    if (!p.empty() && p[0] == '/') p = p.substr(1);
    if (p.empty()) {
        IsoExtent e;
        e.valid = true;
        e.lba = root_.lba;
        e.size = root_.size;
        return e;
    }
    auto parts = split(p, '/');
    const Entry* e = find_entry(&root_, parts, 0);
    IsoExtent out;
    if (e && !e->is_dir) {
        out.valid = true;
        out.lba = e->lba;
        out.size = e->size;
    }
    return out;
}

bool Iso9660::file_exists(const std::string& path) const { return lookup(path).valid; }

uint64_t Iso9660::file_size(const std::string& path) const {
    IsoExtent e = lookup(path);
    return e.valid ? e.size : 0;
}

bool Iso9660::read_extent(const IsoExtent& e, uint64_t off, uint32_t len, void* dst) const {
    if (!e.valid || off + len > e.size) return false;
    return src_->read(e.lba * kSector + off, len, dst);
}

bool Iso9660::read_file(const std::string& path, std::vector<uint8_t>* out,
                        size_t max_bytes, std::string* err) const {
    IsoExtent e = lookup(path);
    if (!e.valid) {
        if (err) *err = "file not found in ISO: " + path;
        return false;
    }
    if (e.size > max_bytes) {
        if (err) *err = "file too large to load fully (" + std::to_string(e.size) + " bytes)";
        return false;
    }
    out->resize((size_t)e.size);
    if (e.size && !read_extent(e, 0, (uint32_t)e.size, out->data())) {
        if (err) *err = "read error: " + path;
        return false;
    }
    return true;
}

std::vector<std::string> Iso9660::list_dir(const std::string& path) const {
    std::vector<std::string> out;
    std::string p = to_lower(trim(path));
    if (!p.empty() && p[0] == '/') p = p.substr(1);
    const Entry* dir = &root_;
    if (!p.empty()) {
        auto parts = split(p, '/');
        dir = find_entry(&root_, parts, 0);
    }
    if (!dir || !dir->is_dir) return out;
    for (const auto& kv : dir->children) out.push_back(kv.second.name_l);
    return out;
}

}  // namespace anyps3
