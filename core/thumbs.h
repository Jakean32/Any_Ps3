#pragma once
// Thumbnail cache generation. Covers are loaded directly from the user's
// ISO/folder, downscaled once, and cached as small PNG/JPEG files in the
// app's metadata dir. Original game data is never copied.
#include <cstdint>
#include <string>
#include <vector>

namespace anyps3 {

struct ThumbInfo {
    bool ok = false;
    std::string path;     // resulting thumbnail file
    std::string note;     // human-readable note (e.g. "copied raw, decode failed")
    uint32_t w = 0, h = 0;
    size_t bytes = 0;
};

// Decode `data` (PNG/JPEG), downscale so width <= max_w (aspect kept),
// encode as PNG at `out_path`. If decoding fails, copies the raw bytes
// as fallback (still a small metadata file, never the full game).
ThumbInfo make_thumbnail(const std::string& out_path, const uint8_t* data, size_t n,
                         int max_w, bool jpeg);

}  // namespace anyps3
