#pragma once
// Abstract random-access byte sources. This is the foundation of the
// "no-copy import": game data is NEVER copied into app storage; we read
// directly from the original location through these interfaces.
//  - FileSource : open by filesystem path (Windows, host tests)
//  - FdSource   : wrap a raw POSIX fd, optionally owning it.
//                 On Android the fd comes from ParcelFileDescriptor
//                 obtained from a persisted SAF content URI.
//  - MemSource  : in-memory buffer (tests)
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace anyps3 {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t size() const = 0;
    // Random access read: copy `len` bytes at `off` into `dst`.
    virtual bool read(uint64_t off, uint32_t len, void* dst) const = 0;
};

class FileSource : public ByteSource {
public:
    // Opens the file, keeping it open for random reads.
    static std::shared_ptr<FileSource> open(const std::string& path, std::string* err);
    ~FileSource() override;
    uint64_t size() const override { return size_; }
    bool read(uint64_t off, uint32_t len, void* dst) const override;

private:
    FileSource() = default;
    void* handle_ = nullptr;   // FILE* (portable)
    uint64_t size_ = 0;
};

// Random-access reader over an existing raw file descriptor.
// Used by Android: ParcelFileDescriptor.detachFd() -> FdSource(fd, owns=true).
// Every read is an independent pread/lseek+read, so it is safe for the
// lifetime of the descriptor and does not cache anything.
class FdSource : public ByteSource {
public:
    FdSource() = default;
    // fd: raw descriptor. owns: close it on destruction.
    static std::shared_ptr<FdSource> attach(int fd, bool owns, std::string* err);
    ~FdSource() override;
    uint64_t size() const override { return size_; }
    bool read(uint64_t off, uint32_t len, void* dst) const override;
    int fd() const { return fd_; }

private:
    int fd_ = -1;
    bool owns_ = false;
    uint64_t size_ = 0;
};

class MemSource : public ByteSource {
public:
    explicit MemSource(std::vector<uint8_t> data) : data_(std::move(data)) {}
    explicit MemSource(const std::string& s)
        : data_(s.begin(), s.end()) {}
    uint64_t size() const override { return data_.size(); }
    bool read(uint64_t off, uint32_t len, void* dst) const override {
        if (off + len > data_.size()) return false;
        memcpy(dst, data_.data() + off, len);
        return true;
    }

private:
    std::vector<uint8_t> data_;
};

}  // namespace anyps3
