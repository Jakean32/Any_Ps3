#include "anyps3/fs/iso9660.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "anyps3/log.h"

#ifdef _WIN32
#include <io.h>
#include <sys/types.h>
#include <sys/stat.h>
#define AP_OPEN_RO(p) ::_open((p), 0x8000 /*_O_BINARY*/ | 0x1000 /*_O_RDONLY*/)
#define AP_DUP(fd) ::_dup((fd))
#define AP_CLOSE(fd) ::_close((fd))
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define AP_OPEN_RO(p) ::open((p), O_RDONLY)
#define AP_DUP(fd) ::dup((fd))
#define AP_CLOSE(fd) ::close((fd))
#endif

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

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

struct IsoReader::MemBackend : public IsoReader::Backend {
    explicit MemBackend(const std::vector<u8>* data) : data(data) {}
    bool ReadAt(u64 offset, size_t len, void* out) const override {
        if (offset + len > data->size()) return false;
        std::memcpy(out, data->data() + offset, len);
        return true;
    }
    u64 Size() const override { return data->size(); }
    const std::vector<u8>* data;
};

struct IsoReader::FdBackend : public IsoReader::Backend {
    FdBackend(int fd, u64 size) : fd(fd), size(size) {}
    ~FdBackend() override {
        if (fd >= 0) AP_CLOSE(fd);
    }
    bool ReadAt(u64 offset, size_t len, void* out) const override {
        if (fd < 0 || offset > size) return false;
        u64 avail = size - offset;
        if (len > avail) return false;
        if (lseek64_or_decay(fd, offset) != 0) return false;
        auto* dst = static_cast<u8*>(out);
        size_t total = 0;
        while (total < len) {
#ifdef _WIN32
            int r = ::_read(fd, dst + total, static_cast<unsigned>(len - total));
#else
            ssize_t r = ::read(fd, dst + total, len - total);
#endif
            if (r <= 0) return false;
            total += static_cast<size_t>(r);
        }
        return true;
    }
    u64 Size() const override { return size; }
    int fd;
    u64 size;

    static int lseek64_or_decay(int fd, u64 offset) {
#ifdef _WIN32
        return ::_lseeki64(fd, static_cast<__int64>(offset), SEEK_SET) < 0 ? -1 : 0;
#else
        return ::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0 ? -1 : 0;
#endif
    }
};

IsoReader::~IsoReader() = default;

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

bool IsoReader::Open(const std::string& path) {
    backend_.reset();
    open_error_.clear();
    source_path_ = path;
    int fd = AP_OPEN_RO(path.c_str());
    if (fd < 0) {
        open_error_ = "cannot open file: " + path;
        return false;
    }
#ifdef _WIN32
    struct _stati64 st;
    if (::_fstati64(fd, &st) != 0) {
#else
    struct stat st;
    if (::fstat(fd, &st) != 0) {
#endif
        AP_CLOSE(fd);
        open_error_ = "cannot stat file: " + path;
        return false;
    }
    u64 size = static_cast<u64>(st.st_size);
    if (size < 17ull * kSectorSize) {
        AP_CLOSE(fd);
        open_error_ = "too small to be an ISO image (";
        open_error_ += std::to_string(size) + " bytes)";
        return false;
    }
    return FinishOpen(std::make_unique<FdBackend>(fd, size), /*sibling key pickup=*/true);
}

bool IsoReader::Open(std::vector<u8> image_data) {
    backend_.reset();
    open_error_.clear();
    source_path_.clear();
    mem_data_ = std::move(image_data);
    if (mem_data_.size() < 17ull * kSectorSize) {
        open_error_ = "too small to be an ISO image";
        return false;
    }
    return FinishOpen(std::make_unique<MemBackend>(&mem_data_), false);
}

bool IsoReader::OpenFd(int fd, const std::string& display_name) {
    backend_.reset();
    open_error_.clear();
    source_path_ = display_name;  // informational only (no sibling pickup)
    if (fd < 0) {
        open_error_ = "invalid file descriptor";
        return false;
    }
    int dup_fd = AP_DUP(fd);
    if (dup_fd < 0) {
        open_error_ = "cannot duplicate file descriptor";
        return false;
    }
#ifdef _WIN32
    struct _stati64 st;
    if (::_fstati64(dup_fd, &st) != 0) {
#else
    struct stat st;
    if (::fstat(dup_fd, &st) != 0) {
#endif
        AP_CLOSE(dup_fd);
        open_error_ = "cannot stat file descriptor";
        return false;
    }
    u64 size = static_cast<u64>(st.st_size);
    if (size < 17ull * kSectorSize) {
        AP_CLOSE(dup_fd);
        open_error_ = "too small to be an ISO image";
        return false;
    }
    return FinishOpen(std::make_unique<FdBackend>(dup_fd, size), false);
}

bool IsoReader::FinishOpen(std::unique_ptr<Backend> backend, bool auto_pickup_key) {
    sibling_pickup_ = auto_pickup_key;
    backend_ = std::move(backend);
    entries_.clear();

    // Parse the disc-encryption layer from the first two sectors.
    any_ps3::DiscEncryptionInfo info;
    u64 image_bytes = backend_->Size();
    {
        std::vector<u8> head(4096, 0);
        size_t want = static_cast<size_t>(std::min<u64>(4096, image_bytes));
        head.resize(want);
        if (!backend_->ReadAt(0, want, head.data())) {
            open_error_ = "short read on image header";
            return false;
        }
        if (any_ps3::ParseDiscRegionTable(head.data(), image_bytes, &info)) {
            crypto_.SetInfo(info);
            AP_INFO("IsoReader: ", info.Summary());
        } else {
            crypto_.SetInfo(any_ps3::DiscEncryptionInfo{});
            AP_INFO("IsoReader: no PS3 disc-encryption layer (plain ISO9660)");
        }
    }

    // Key pickup: 3k3y embedded key first, then sibling .dkey/.key files.
    if (crypto_.NeedsKey()) {
        if (info.has_embedded_key) {
            std::vector<u8> head(4096, 0);
            if (backend_->ReadAt(0, 4096, head.data())) {
                crypto_.SetKey(head.data() + 0xF80);
                key_source_ = KeySource::Embedded3k3y;
                AP_INFO("IsoReader: using 3k3y embedded disc key");
            }
        } else if (sibling_pickup_) {
            for (const std::string& cand : any_ps3::DiscKeyFileCandidates(source_path_)) {
                std::vector<u8> key_bytes;
                FILE* kf = std::fopen(cand.c_str(), "rb");
                if (!kf) continue;
                char kbuf[4096] = {0};
                size_t klen = std::fread(kbuf, 1, sizeof(kbuf), kf);
                std::fclose(kf);
                uint8_t key[16];
                std::string err;
                std::string text(kbuf, kbuf + klen);
                if (klen >= 16 && any_ps3::ParseDiscKey(text, key, &err)) {
                    crypto_.SetKey(key);
                    key_source_ = KeySource::SiblingFile;
                    sibling_key_path_ = cand;
                    AP_INFO("IsoReader: disc key loaded from ", cand.c_str());
                    break;
                }
            }
        }
    }

    if (!ParsePvd()) {
        if (crypto_.NeedsKey()) {
            open_error_ =
                "ISO9660 structures unreadable — the image carries encrypted "
                "regions and no disc key is available. Provide a .dkey file "
                "next to the ISO or select one in the game settings.";
        }
        return false;
    }
    return true;
}

bool IsoReader::SetDiscKeyText(const std::string& text) {
    uint8_t key[16];
    std::string err;
    if (!any_ps3::ParseDiscKey(text, key, &err)) {
        open_error_ = "disc key error: " + err;
        return false;
    }
    return SetDiscKeyRaw(key);
}

bool IsoReader::SetDiscKeyRaw(const uint8_t key[16]) {
    crypto_.SetKey(key);
    key_source_ = KeySource::User;
    // Re-walk the tree through the decrypted view.
    entries_.clear();
    if (!ParsePvd()) {
        open_error_ =
            "disc key accepted but the filesystem still does not parse — "
            "the key is likely wrong for this image";
        return false;
    }
    open_error_.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Sector reads with on-the-fly decryption
// ---------------------------------------------------------------------------

bool IsoReader::ReadSectors(u32 lba, u32 count, u8* out) const {
    if (!backend_) return false;
    u64 off = static_cast<u64>(lba) * kSectorSize;
    u64 need = static_cast<u64>(count) * kSectorSize;
    if (off + need > backend_->Size()) return false;
    if (!backend_->ReadAt(off, static_cast<size_t>(need), out)) return false;
    crypto_.DecryptSectors(lba, out, count);
    return true;
}

u64 IsoReader::image_size() const { return backend_ ? backend_->Size() : 0; }

// ---------------------------------------------------------------------------
// ISO9660 tree (directory records; "CD001" primary volume only)
// ---------------------------------------------------------------------------

bool IsoReader::ParsePvd() {
    std::vector<u8> pvd(kSectorSize);
    if (!ReadSectors(kPvdLba, 1, pvd.data())) {
        open_error_ = "cannot read volume descriptor sector";
        return false;
    }
    if (std::memcmp(pvd.data(), "\x00" "CD001", 5) != 0) {
        AP_ERROR("IsoReader: no ISO9660 volume descriptor at LBA 16 "
                  "(not an ISO, or a UDF-only image)");
        open_error_ = "no ISO9660 volume descriptor at LBA 16 "
                      "(not an ISO image, or a UDF-only image — UDF is not "
                      "supported yet)";
        return false;
    }
    const u8* rec = pvd.data() + 156;
    u8 rec_len = rec[0];
    if (rec_len < 34) {
        open_error_ = "malformed PVD root record";
        return false;
    }
    u32 lba_le = ReadLE32(rec + 2);
    u32 lba_be = ReadBE32(rec + 6);
    u64 sz_le = ReadLE32(rec + 10);
    u64 sz_be = ReadBE32(rec + 14);
    if (lba_le != lba_be || sz_le != sz_be) {
        AP_ERROR("IsoReader: PVD root record endian mismatch");
        open_error_ = "PVD root record endian mismatch";
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
