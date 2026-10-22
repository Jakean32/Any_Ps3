#include "anyps3/fs/iso9660.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "anyps3/log.h"

namespace anyps3 {

static constexpr u32 kSectorSize = 2048;
static constexpr u32 kPvdLba = 16;

static std::string NormalizeKey(std::string path) {
    // "/PS3_GAME/PARAM.SFO" or "PS3_GAME\\PARAM.SFO" -> "ps3_game/param.sfo"
    std::string out;
    for (char c : path) {
        if (c == '\\') c = '/';
        if (c == '/' && (out.empty() || out.back() == '/')) continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

static std::string StripVersionSuffix(const std::string& name) {
    size_t pos = name.find(';');
    return pos == std::string::npos ? name : name.substr(0, pos);
}

bool IsoReader::Open(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 17 * static_cast<long>(kSectorSize)) {
        std::fclose(f);
        return false;
    }
    data_.resize(static_cast<size_t>(size));
    size_t got = std::fread(data_.data(), 1, data_.size(), f);
    std::fclose(f);
    if (got != data_.size()) return false;
    return Open(std::move(data_));
}

bool IsoReader::Open(std::vector<u8> image_data) {
    data_ = std::move(image_data);
    if (data_.size() < 17 * kSectorSize) return false;
    return ParsePvd();
}

bool IsoReader::ParsePvd() {
    const u8* pvd = data_.data() + kPvdLba * kSectorSize;
    if (std::memcmp(pvd, "\x00" "CD001", 5) != 0) {
        AP_ERROR("IsoReader: no ISO9660 volume descriptor at LBA 16 (not an ISO, or UDF-only image)");
        return false;
    }
    const u8* rec = pvd + 156;
    u8 rec_len = rec[0];
    if (rec_len < 34) return false;
    u32 lba_le = ReadLE32(rec + 2);
    u32 lba_be = ReadBE32(rec + 6);
    u64 sz_le = ReadLE32(rec + 10);
    u64 sz_be = ReadBE32(rec + 14);
    if (lba_le != lba_be || sz_le != sz_be) {
        AP_ERROR("IsoReader: PVD root record endian mismatch");
        return false;
    }
    root_lba_ = lba_le;
    root_size_ = sz_le;
    entries_.clear();
    return WalkDir(root_lba_, root_size_, "");
}

bool IsoReader::WalkDir(u32 dir_lba, u64 dir_size, const std::string& prefix) {
    if (dir_size == 0) return true;
    u32 sectors = static_cast<u32>((dir_size + kSectorSize - 1) / kSectorSize);
    std::vector<u8> dir(static_cast<size_t>(sectors) * kSectorSize, 0);
    if (!ReadSectors(dir_lba, sectors, dir.data())) return false;

    size_t off = 0;
    size_t limit = static_cast<size_t>(dir_size);
    while (off < limit) {
        u8 len = dir[off];
        if (len == 0) {  // records never span sector boundaries
            off = (off / kSectorSize + 1) * kSectorSize;
            continue;
        }
        const u8* r = dir.data() + off;
        u32 lba = ReadLE32(r + 2);
        u64 size = ReadLE32(r + 10);
        u8 flags = r[25];
        u8 name_len = r[32];
        std::string name(reinterpret_cast<const char*>(r + 33), name_len);
        if (!name.empty() && name[0] != '\x00' && name[0] != '\x01') {
            std::string clean = StripVersionSuffix(name);
            std::string key = prefix.empty() ? NormalizeKey(clean)
                                            : NormalizeKey(prefix + "/" + clean);
            IsoFileEntry entry;
            entry.name = clean;
            entry.lba = lba;
            entry.size = size;
            entry.is_dir = (flags & 0x02) != 0;
            entries_[key] = entry;
            if (entry.is_dir) {
                if (!WalkDir(lba, size, key)) return false;
            }
        }
        off += len;
    }
    return true;
}

std::optional<IsoFileEntry> IsoReader::Find(const std::string& iso_path) const {
    auto it = entries_.find(NormalizeKey(iso_path));
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

bool IsoReader::ReadSectors(u32 lba, u32 count, u8* out) const {
    u64 off = static_cast<u64>(lba) * kSectorSize;
    u64 need = static_cast<u64>(count) * kSectorSize;
    if (off + need > data_.size()) return false;
    std::memcpy(out, data_.data() + off, static_cast<size_t>(need));
    return true;
}

bool IsoReader::ReadFile(const std::string& iso_path, std::vector<u8>* out) const {
    auto entry = Find(iso_path);
    if (!entry || entry->is_dir) return false;
    u32 sectors = static_cast<u32>((entry->size + kSectorSize - 1) / kSectorSize);
    // Read whole sectors into a scratch buffer first — the last sector is padded.
    std::vector<u8> tmp(static_cast<size_t>(sectors) * kSectorSize, 0);
    if (!ReadSectors(entry->lba, sectors, tmp.data())) return false;
    out->assign(tmp.begin(), tmp.begin() + static_cast<long>(entry->size));
    return true;
}

bool IsoReader::ListDir(const std::string& iso_path, std::vector<IsoFileEntry>* out) const {
    std::string key = NormalizeKey(iso_path);
    for (const auto& [k, v] : entries_) {
        std::string parent;
        size_t slash = k.rfind('/');
        parent = slash == std::string::npos ? "" : k.substr(0, slash);
        if (parent == key) out->push_back(v);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Minimal writer (tests only). Two-pass layout:
//   pass 1: assign LBAs to file data
//   pass 2: compute directory extent sizes (record sizes depend only on names),
//           assign directory LBAs, then fill records with final LBA values.
// ---------------------------------------------------------------------------

namespace iso_write {

static u8 RecordLength(const std::string& name) {
    u8 len = 33 + static_cast<u8>(name.size());
    if ((name.size() & 1) == 0) len++;  // even-length names get a pad byte
    return len;
}

static void PutRecord(std::vector<u8>* out, const std::string& name, bool is_dir, u32 lba,
                      u32 size) {
    size_t start = out->size();
    out->push_back(0);  // length (patched below)
    out->push_back(0);  // ext attr
    u8 t[4];
    WriteLE32(t, lba);  out->insert(out->end(), t, t + 4);
    WriteBE32(t, lba);  out->insert(out->end(), t, t + 4);
    WriteLE32(t, size); out->insert(out->end(), t, t + 4);
    WriteBE32(t, size); out->insert(out->end(), t, t + 4);
    for (int i = 0; i < 7; i++) out->push_back(0);  // recording time
    out->push_back(is_dir ? 0x02 : 0x00);           // flags
    out->push_back(0); out->push_back(0);           // interleave
    out->push_back(1); out->push_back(0);           // volume seq LE (1)
    out->push_back(0); out->push_back(1);           // volume seq BE (1)
    out->push_back(static_cast<u8>(name.size()));
    out->insert(out->end(), name.begin(), name.end());
    if ((name.size() & 1) == 0) out->push_back(0);  // pad
    (*out)[start] = static_cast<u8>(out->size() - start);
}

}  // namespace iso_write

void IsoWriter::AddFile(const std::string& iso_path, std::vector<u8> bytes) {
    std::string key = NormalizeKey(iso_path);
    size_t slash = key.rfind('/');
    if (slash != std::string::npos) AddDir(key.substr(0, slash));
    Node n;
    n.name = key;
    n.is_dir = false;
    n.bytes = std::move(bytes);
    nodes_.push_back(std::move(n));
}

void IsoWriter::AddDir(const std::string& iso_path) {
    std::string p = NormalizeKey(iso_path);
    while (!p.empty()) {
        bool have = false;
        for (const auto& e : nodes_) {
            if (e.name == p && e.is_dir) have = true;
        }
        if (!have) {
            Node n;
            n.name = p;
            n.is_dir = true;
            nodes_.push_back(std::move(n));
        }
        size_t slash = p.rfind('/');
        if (slash == std::string::npos) break;
        p = p.substr(0, slash);
    }
}

std::vector<u8> IsoWriter::Build() const {
    using iso_write::PutRecord;
    using iso_write::RecordLength;

    // Sorted node list (parents first by construction of key ordering).
    std::vector<const Node*> ordered;
    for (const auto& n : nodes_) ordered.push_back(&n);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Node* a, const Node* b) { return a->name < b->name; });

    // All directories, including implicit root "".
    std::vector<std::string> dirs{std::string()};
    for (const Node* n : ordered) {
        if (n->is_dir) dirs.push_back(n->name);
    }
    // Deepest first: children are laid out before parents.
    std::sort(dirs.begin(), dirs.end());
    std::sort(dirs.begin(), dirs.end(),
              [](const std::string& a, const std::string& b) {
                  return std::count(a.begin(), a.end(), '/') >
                         std::count(b.begin(), b.end(), '/');
              });

    auto parent_of = [](const std::string& d) {
        size_t s = d.rfind('/');
        return s == std::string::npos ? std::string() : d.substr(0, s);
    };

    // Pass 1: file LBAs (file data region starts at LBA 18).
    std::map<std::string, u32> file_lba;
    u32 next_lba = 18;
    for (const Node* n : ordered) {
        if (n->is_dir) continue;
        file_lba[n->name] = next_lba;
        next_lba += static_cast<u32>((n->bytes.size() + kSectorSize - 1) / kSectorSize);
    }

    // Pass 2a: directory sizes (records depend only on names/flags).
    std::map<std::string, u32> dir_size;
    for (const std::string& d : dirs) {
        u32 sz = RecordLength("\x00") + RecordLength("\x01");
        for (const Node* n : ordered) {
            if (parent_of(n->name) != d) continue;
            sz += RecordLength(n->name.substr(n->name.rfind('/') == std::string::npos
                                                  ? 0
                                                  : n->name.rfind('/') + 1));
        }
        dir_size[d] = sz;
    }
    // Pass 2b: directory LBAs.
    std::map<std::string, u32> dir_lba;
    for (const std::string& d : dirs) {
        dir_lba[d] = next_lba;
        next_lba += (dir_size[d] + kSectorSize - 1) / kSectorSize;
    }
    // Pass 2c: materialize directory extents.
    std::map<std::string, std::vector<u8>> dir_data;
    for (const std::string& d : dirs) {
        std::vector<u8> body;
        PutRecord(&body, "\x00", true, dir_lba[d], dir_size[d]);
        PutRecord(&body, "\x01", true, dir_lba[parent_of(d) == d ? "" : parent_of(d)],
                  dir_size[parent_of(d) == d ? "" : parent_of(d)]);
        for (const Node* n : ordered) {
            if (parent_of(n->name) != d) continue;
            std::string leaf =
                n->name.substr(n->name.rfind('/') == std::string::npos ? 0
                                                                        : n->name.rfind('/') + 1);
            if (n->is_dir) {
                PutRecord(&body, leaf, true, dir_lba[n->name], dir_size[n->name]);
            } else {
                PutRecord(&body, leaf, false, file_lba[n->name],
                          static_cast<u32>(n->bytes.size()));
            }
        }
        dir_data[d] = std::move(body);
    }

    // Assemble image.
    std::vector<u8> img(static_cast<size_t>(next_lba) * kSectorSize, 0);
    // PVD at LBA 16.
    {
        std::vector<u8> pvd(kSectorSize, 0);
        std::memcpy(pvd.data(), "\x00" "CD001", 5);
        pvd[5] = 0x01;  // VDT version
        std::string sys_id = "ANY_PS3_TEST_VOLUME";
        std::memcpy(pvd.data() + 8, sys_id.c_str(), sys_id.size());
        std::string vol_id = "ANY_PS3";
        std::memcpy(pvd.data() + 40, vol_id.c_str(), vol_id.size());
        // Both LBA fields in PVD (offset 80/84 LE, 84/88 BE).
        WriteLE32(pvd.data() + 80, 18);
        WriteBE32(pvd.data() + 84, 18);
        WriteLE32(pvd.data() + 80 + 8, static_cast<u32>(img.size() / kSectorSize));
        WriteBE32(pvd.data() + 84 + 8, static_cast<u32>(img.size() / kSectorSize));
        std::vector<u8> rootrec;
        PutRecord(&rootrec, "\x00", true, dir_lba[""], dir_size[""]);
        std::memcpy(pvd.data() + 156, rootrec.data(), rootrec.size());
        std::memcpy(img.data() + 16 * kSectorSize, pvd.data(), kSectorSize);
    }
    // Volume descriptor set terminator at LBA 17.
    {
        std::vector<u8> term(kSectorSize, 0);
        std::memcpy(term.data(), "\x01" "CD001", 5);
        term[5] = 0x01;
        std::memcpy(img.data() + 17 * kSectorSize, term.data(), kSectorSize);
    }
    for (const Node* n : ordered) {
        if (n->is_dir) continue;
        u32 lba = file_lba[n->name];
        std::memcpy(img.data() + static_cast<size_t>(lba) * kSectorSize, n->bytes.data(),
                    n->bytes.size());
    }
    for (const auto& [d, body] : dir_data) {
        u32 lba = dir_lba[d];
        std::memcpy(img.data() + static_cast<size_t>(lba) * kSectorSize, body.data(),
                    body.size());
    }
    return img;
}

}  // namespace anyps3
