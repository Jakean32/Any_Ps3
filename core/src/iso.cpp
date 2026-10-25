#include "any_ps3/iso.h"
#include <cstring>
#include <cctype>
#include <algorithm>

namespace any_ps3 {

bool IsoReader::read_sector(uint32_t lba, uint8_t* buf) {
    return src_ && src_->read_exact((uint64_t)lba * kSector, buf, kSector);
}

static void ucs2be_to_utf8(const uint8_t* p, size_t nbytes, std::string& out) {
    out.clear();
    out.reserve(nbytes / 2 + 1);
    for (size_t i = 0; i + 1 < nbytes; i += 2) {
        uint32_t cp = ((uint32_t)p[i] << 8) | p[i + 1];
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
}

static void strip_version_suffix(std::string& name) {
    // "PARAM.SFO;1" -> "PARAM.SFO"
    size_t pos = name.find(';');
    if (pos != std::string::npos) name.resize(pos);
}

bool IsoReader::parse_dir_extent(uint32_t lba, uint64_t size, bool ucs2,
                                 std::vector<IsoDirEntry>& out) {
    if (size == 0) size = kSector;
    if (size > 16ull * 1024 * 1024) { error_ = "directory too large"; return false; }
    std::vector<uint8_t> data((size_t)size);
    uint64_t got = 0;
    while (got < size) {
        size_t chunk = (size_t)std::min<uint64_t>(kSector, size - got);
        if (!read_sector(lba + (uint32_t)(got / kSector), data.data() + got)) {
            error_ = "failed reading directory extent";
            return false;
        }
        got += chunk;
    }

    size_t off = 0;
    while (off < data.size()) {
        // Records never cross sector boundaries; a zero length means "skip to
        // the next sector".
        size_t sec_off = off % kSector;
        uint8_t rec_len = data[off];
        if (rec_len == 0) {
            if (sec_off == 0 && off + kSector > data.size()) break; // trailing padding
            off += kSector - sec_off;
            continue;
        }
        if (off + rec_len > data.size()) break; // malformed tail; stop
        const uint8_t* r = data.data() + off;
        uint32_t elba = rd32le(r + 2);
        uint64_t esize = rd32le(r + 10);
        uint8_t flags = r[25];
        uint8_t name_len = r[32];
        const uint8_t* namep = r + 33;
        if (33 + name_len > rec_len) { off += rec_len; continue; }

        if (name_len == 1 && (namep[0] == 0 || namep[0] == 1)) {
            // "." or ".." — skip
        } else {
            IsoDirEntry e;
            if (ucs2) ucs2be_to_utf8(namep, name_len, e.name);
            else e.name.assign((const char*)namep, name_len);
            strip_version_suffix(e.name);
            // Basic sanity on d-character names.
            if (!e.name.empty()) {
                e.lba = elba;
                e.size = esize;
                e.is_dir = (flags & 0x02) != 0;
                out.push_back(std::move(e));
            }
        }
        off += rec_len;
    }
    return true;
}

bool IsoReader::open() {
    if (!src_) { error_ = "no source"; return false; }

    // Scan the volume descriptor chain starting at LBA 16.
    bool have_pvd = false;
    bool have_svd = false;
    bool have_bea_or_nsr = false;
    for (int i = 0; i < 64; i++) {
        uint8_t sec[kSector];
        if (!read_sector(16 + i, sec)) break; // truncated image: use what we have
        if (sec[1] == 'B' && sec[2] == 'E' && sec[3] == 'A' && sec[4] == '0' && sec[5] == '1')
            have_bea_or_nsr = true; // Beginning Extended Area (UDF present)
        if (sec[1] == 'N' && sec[2] == 'S' && sec[3] == 'R' && (sec[4] == '0' || sec[4] == '2' || sec[4] == '3'))
            have_bea_or_nsr = true; // NSR02/NSR03 => UDF/ISO-13346 structure
        if (std::memcmp(sec + 1, "CD001", 5) != 0) continue;
        uint8_t type = sec[0];
        if (type == 1 && !have_pvd) {
            // Primary Volume Descriptor; root dir record at offset 156.
            const uint8_t* rec = sec + 156;
            if (rec[0] >= 34) {
                root_[0].lba = rd32le(rec + 2);
                root_[0].size = rd32le(rec + 10);
                root_[0].is_dir = true;
                root_[0].name = "/";
                have_pvd = true;
            }
        } else if (type == 2 && !have_svd) {
            // Supplementary Volume Descriptor (Joliet, UCS-2BE names)?
            const uint8_t* esc = sec + 88; // escape sequences, 32 bytes
            bool joliet = (esc[0] == 0x25 && esc[1] == 0x2F && (esc[2] == 0x40 || esc[2] == 0x43 || esc[2] == 0x45));
            const uint8_t* rec = sec + 156;
            if (joliet && rec[0] >= 34) {
                root_[1].lba = rd32le(rec + 2);
                root_[1].size = rd32le(rec + 10);
                root_[1].is_dir = true;
                root_[1].name = "/";
                have_svd = true;
                joliet_ = true;
            }
        } else if (type == 255) {
            break; // terminator
        }
    }

    if (!have_pvd) {
        if (have_bea_or_nsr) {
            error_ = "UDF filesystem detected — UDF is not supported yet. "
                     "Re-dump the disc as ISO9660/Joliet or use a folder dump (PS3_GAME/...).";
        } else {
            error_ = "no ISO9660 volume descriptor found (not an ISO image?)";
        }
        return false;
    }
    return true;
}

bool IsoReader::list(const std::string& path, std::vector<IsoDirEntry>& out) {
    out.clear();
    IsoDirEntry dir;
    if (!walk(path, dir)) return false;
    if (!dir.is_dir) { error_ = "not a directory"; return false; }
    return parse_dir_extent(dir.lba, dir.size, joliet_, out);
}

bool IsoReader::walk(const std::string& path, IsoDirEntry& out) {
    // Start at the root (prefer Joliet names when present).
    out = joliet_ ? root_[1] : root_[0];
    if (out.lba == 0) { error_ = "volume not open"; return false; }

    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') i++;
        size_t j = i;
        while (j < path.size() && path[j] != '/') j++;
        if (j == i) break;
        std::string seg = path.substr(i, j - i);
        i = j;

        std::vector<IsoDirEntry> entries;
        if (!parse_dir_extent(out.lba, out.size, joliet_, entries)) return false;
        bool found = false;
        std::string needle;
        needle.reserve(seg.size());
        for (char c : seg) needle.push_back((char)::tolower((unsigned char)c));
        for (auto& e : entries) {
            std::string low;
            low.reserve(e.name.size());
            for (char c : e.name) low.push_back((char)::tolower((unsigned char)c));
            if (low == needle) { out = e; found = true; break; }
        }
        if (!found) { error_ = "entry not found: " + seg; return false; }
    }
    return true;
}

bool IsoReader::stat(const std::string& path, IsoDirEntry& out) {
    return walk(path, out);
}

bool IsoReader::read_file_range(const std::string& path, uint64_t offset, uint64_t len,
                                std::vector<uint8_t>& out) {
    IsoDirEntry e;
    if (!walk(path, e)) return false;
    if (e.is_dir) { error_ = "is a directory"; return false; }
    if (offset > e.size) { error_ = "range beyond file"; return false; }
    if (offset + len > e.size) len = e.size - offset;
    out.resize((size_t)len);
    size_t got = src_->read_at((uint64_t)e.lba * kSector + offset, out.data(), out.size());
    if (got != out.size()) { out.resize(got); error_ = "short read"; return false; }
    return true;
}

bool IsoReader::read_file(const std::string& path, std::vector<uint8_t>& out, size_t cap) {
    IsoDirEntry e;
    if (!walk(path, e)) return false;
    if (e.is_dir) { error_ = "is a directory"; return false; }
    if (e.size > cap) { error_ = "file too large for requested cap"; return false; }
    out.resize((size_t)e.size);
    size_t got = src_->read_at((uint64_t)e.lba * kSector, out.data(), out.size());
    if (got != out.size()) { out.resize(got); error_ = "short read"; return false; }
    return true;
}

bool IsoReader::read_entry_range(const IsoDirEntry& e, uint64_t offset, uint64_t len, void* out) {
    if (offset > e.size) { error_ = "range beyond file"; return false; }
    if (offset + len > e.size) len = e.size - offset;
    if (!src_->read_exact((uint64_t)e.lba * kSector + offset, out, (size_t)len)) {
        error_ = "short read";
        return false;
    }
    return true;
}

} // namespace any_ps3
