#include "bytesource.h"

#include <cstdio>
#include <cstring>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <io.h>
#include <sys/stat.h>
#endif

namespace anyps3 {

// ---------------- FileSource ----------------
std::shared_ptr<FileSource> FileSource::open(const std::string& path, std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (err) *err = "cannot open file: " + path;
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        if (err) *err = "cannot determine size: " + path;
        return nullptr;
    }
    auto out = std::shared_ptr<FileSource>(new FileSource());
    out->handle_ = f;
    out->size_ = (uint64_t)n;
    return out;
}

FileSource::~FileSource() {
    if (handle_) fclose((FILE*)handle_);
}

bool FileSource::read(uint64_t off, uint32_t len, void* dst) const {
    if (off + len > size_) return false;
    FILE* f = (FILE*)handle_;
    if (len == 0) return true;
#ifdef _WIN32
    if (_fseeki64(f, (int64_t)off, SEEK_SET) != 0) return false;
#else
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return false;
#endif
    return fread(dst, 1, len, f) == len;
}

// ---------------- FdSource ----------------
std::shared_ptr<FdSource> FdSource::attach(int fd, bool owns, std::string* err) {
    if (fd < 0) {
        if (err) *err = "invalid file descriptor";
        return nullptr;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        if (err) *err = "fstat failed on fd";
        return nullptr;
    }
    auto out = std::shared_ptr<FdSource>(new FdSource());
    out->fd_ = fd;
    out->owns_ = owns;
    out->size_ = (uint64_t)st.st_size;
    return out;
}

FdSource::~FdSource() {
#ifndef _WIN32
    if (fd_ >= 0 && owns_) close(fd_);
#else
    if (fd_ >= 0 && owns_) _close(fd_);
#endif
}

bool FdSource::read(uint64_t off, uint32_t len, void* dst) const {
    if (len == 0) return true;
    if (off + len > size_) return false;
    uint8_t* p = (uint8_t*)dst;
    uint64_t pos = off;
    while (len > 0) {
        ssize_t got;
#ifndef _WIN32
        got = pread(fd_, p, len, (off_t)pos);
        if (got <= 0) return false;
#else
        if (_lseeki64(fd_, (int64_t)pos, SEEK_SET) != (int64_t)pos) return false;
        got = _read(fd_, p, (uint32_t)len);
        if (got <= 0) return false;
#endif
        p += got;
        pos += (uint64_t)got;
        len -= (uint32_t)got;
    }
    return true;
}

}  // namespace anyps3
