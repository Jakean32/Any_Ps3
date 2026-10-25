// Any_Ps3 — byte readers & the random-access file-descriptor source.
//
// FdSource is THE abstraction for "import without copying":
//  - Android: the fd comes from ParcelFileDescriptor (SAF). We NEVER read the
//    whole ISO into RAM and never copy it into app storage; all access is
//    seek + bounded reads (random access).
//  - Windows/Linux: fd from open() on the stored path.
// FdSource does NOT take ownership of the descriptor.
#pragma once

#include "types.h"

#ifdef _WIN32
#include <io.h>
#include <sys/types.h>
#include <sys/stat.h>
#define AP_READ(fd, buf, len) ::_read((fd), (buf), (unsigned)(len))
#define AP_LSEEK(fd, off) ::_lseeki64((fd), (off), SEEK_SET)
#define AP_FSTAT(fd, st) ::_fstati64((fd), (st))
using ap_stat_t = struct _stati64;
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#define AP_READ(fd, buf, len) ::read((fd), (buf), (len))
#define AP_LSEEK(fd, off) ::lseek((fd), (off), SEEK_SET)
#define AP_FSTAT(fd, st) ::fstat((fd), (st))
using ap_stat_t = struct stat;
#endif

namespace any_ps3 {

// ---- little/big endian scalars from raw bytes ----
inline uint16_t rd16le(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t rd64le(const uint8_t* p) {
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}
inline uint16_t rd16be(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
inline uint64_t rd64be(const uint8_t* p) {
    return ((uint64_t)rd32be(p) << 32) | rd32be(p + 4);
}

// Sequential reader over an in-memory buffer (throws Error on overflow).
struct ByteReader {
    const uint8_t* p;
    size_t n;
    size_t off = 0;
    ByteReader(const uint8_t* data, size_t len) : p(data), n(len) {}
    size_t remaining() const { return off < n ? n - off : 0; }
    void need(size_t k) const { if (off + k > n) throw Error("buffer underrun"); }
    uint8_t  u8()  { need(1); return p[off++]; }
    uint16_t u16le() { need(2); uint16_t v = rd16le(p + off); off += 2; return v; }
    uint32_t u32le() { need(4); uint32_t v = rd32le(p + off); off += 4; return v; }
    uint64_t u64le() { need(8); uint64_t v = rd64le(p + off); off += 8; return v; }
    uint16_t u16be() { need(2); uint16_t v = rd16be(p + off); off += 2; return v; }
    uint32_t u32be() { need(4); uint32_t v = rd32be(p + off); off += 4; return v; }
    uint64_t u64be() { need(8); uint64_t v = rd64be(p + off); off += 8; return v; }
    const uint8_t* take(size_t k) { need(k); const uint8_t* r = p + off; off += k; return r; }
};

// Random-access reader over an open file descriptor (non-owning, seek+read).
// NOTE: not safe for concurrent use of the SAME fd from multiple threads.
class FdSource {
public:
    explicit FdSource(int fd) : fd_(fd) { refresh_size(); }

    uint64_t size() const { return size_; }

    // Reads up to len bytes at absolute offset; returns bytes read (0 at EOF).
    size_t read_at(uint64_t offset, void* buf, size_t len) {
        if (fd_ < 0 || offset > size_) return 0;
        uint64_t avail = size_ - offset;
        if (len > avail) len = (size_t)avail;
        if (len == 0) return 0;
        if (AP_LSEEK(fd_, (long long)offset) < 0) return 0;
        uint8_t* dst = (uint8_t*)buf;
        size_t total = 0;
        while (total < len) {
            long long r = AP_READ(fd_, dst + total, len - total);
            if (r <= 0) break;
            total += (size_t)r;
        }
        return total;
    }

    bool read_exact(uint64_t offset, void* buf, size_t len) {
        return read_at(offset, buf, len) == len;
    }

private:
    void refresh_size() {
        ap_stat_t st;
        size_ = (AP_FSTAT(fd_, &st) == 0) ? (uint64_t)st.st_size : 0;
    }
    int fd_;
    uint64_t size_ = 0;
};

// Sequential writer over an open fd (non-owning; caller closes).
class FdWriter {
public:
    explicit FdWriter(int fd) : fd_(fd) { AP_LSEEK(fd_, 0); }
    bool write_all(const void* buf, size_t len) {
        const uint8_t* p = (const uint8_t*)buf;
        size_t total = 0;
        while (total < len) {
#ifdef _WIN32
            unsigned chunk = (unsigned)((len - total > 0x20000000u) ? 0x20000000u : len - total);
            int r = ::_write(fd_, p + total, chunk);
#else
            ssize_t r = ::write(fd_, p + total, len - total);
#endif
            if (r <= 0) return false;
            total += (size_t)r;
        }
        return true;
    }
    bool write_str(const std::string& s) { return write_all(s.data(), s.size()); }

private:
    int fd_;
};

// Read whole local file (tests / Windows host paths only — NOT used for ISO import).
inline bool read_file_path(const std::string& path, std::vector<uint8_t>& out, size_t cap) {
#ifdef _WIN32
    int fd = ::_open(path.c_str(), 0x8000 /*_O_BINARY*/ | 0x1000 /*_O_RDONLY*/);
    if (fd < 0) return false;
#else
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
#endif
    FdSource src(fd);
    uint64_t sz = src.size();
    if (sz > cap) { ::close(fd); return false; }
    out.resize((size_t)sz);
    bool ok = src.read_exact(0, out.data(), out.size());
    ::close(fd);
    return ok;
}

inline bool write_file_path(const std::string& path, const void* data, size_t len) {
#ifdef _WIN32
    int fd = ::_open(path.c_str(), 0x8000 | 0x1001 /*_O_WRONLY|_O_CREAT|_O_TRUNC*/, 0644);
    if (fd < 0) return false;
#else
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
#endif
    FdWriter w(fd);
    bool ok = w.write_all(data, len);
    ::close(fd);
    return ok;
}

} // namespace any_ps3
