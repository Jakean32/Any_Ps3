// Any_Ps3 — audio backend interface + null backend + ring mixer.
// The SDL/AAudio host backends plug in at the host layer in later phases.
#pragma once

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "anyps3/types.h"

namespace anyps3 {

constexpr u32 kAudioSampleRate = 48000;
constexpr u32 kAudioChannels = 2;

// Simple lock-free-ish ring buffer (single producer, single consumer).
class AudioRing {
public:
    explicit AudioRing(size_t capacity_samples)
        : buf_(capacity_samples * kAudioChannels, 0), capacity_(capacity_samples) {}

    size_t Write(const s16* src, size_t samples) {
        size_t free_samples = capacity_ - (write_pos_ - read_pos_);
        size_t n = samples < free_samples ? samples : free_samples;
        for (size_t i = 0; i < n; i++) {
            size_t pos = (write_pos_ + i) % capacity_;
            buf_[pos * 2] = src[i * 2];
            buf_[pos * 2 + 1] = src[i * 2 + 1];
        }
        write_pos_ += n;
        return n;
    }
    size_t Read(s16* dst, size_t samples) {
        size_t avail = write_pos_ - read_pos_;
        size_t n = samples < avail ? samples : avail;
        for (size_t i = 0; i < n; i++) {
            size_t pos = (read_pos_ + i) % capacity_;
            dst[i * 2] = buf_[pos * 2];
            dst[i * 2 + 1] = buf_[pos * 2 + 1];
        }
        read_pos_ += n;
        if (n < samples) {
            // underrun: silence the rest (documented behavior)
            std::memset(dst + n * 2, 0, (samples - n) * kAudioChannels * sizeof(s16));
        }
        return n;
    }
    size_t available() const { return write_pos_ - read_pos_; }

private:
    std::vector<s16> buf_;
    size_t capacity_;
    std::atomic<size_t> read_pos_{0};
    std::atomic<size_t> write_pos_{0};
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    virtual std::string name() const = 0;
    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;
};

AudioBackend* CreateNullAudioBackend();

}  // namespace anyps3
