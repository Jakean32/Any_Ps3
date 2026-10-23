#include "thumbs.h"

#include <cstdio>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "stb/stb_image.h"
#include "stb/stb_image_resize.h"
#include "stb/stb_image_write.h"

#include "util.h"

namespace anyps3 {

ThumbInfo make_thumbnail(const std::string& out_path, const uint8_t* data, size_t n,
                         int max_w, bool jpeg) {
    ThumbInfo info;
    info.path = out_path;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(data, (int)n, &w, &h, &comp, 0);
    if (!px) {
        // Fallback: keep raw bytes (they are small metadata assets).
        if (write_file(out_path, data, n)) {
            info.ok = true;
            info.bytes = n;
            info.note = "decode failed, raw file kept as thumbnail";
            return info;
        }
        info.note = "decode failed and write failed";
        return info;
    }
    int target_w = w, target_h = h;
    if (w > max_w) {
        target_w = max_w;
        target_h = (int)(((int64_t)h * max_w) / w);
        if (target_h < 1) target_h = 1;
    }
    std::vector<unsigned char> dst((size_t)target_w * target_h * comp);
    if ((size_t)target_w * target_h * comp != dst.size() || target_w < 1 || target_h < 1) {
        stbi_image_free(px);
        info.note = "resize dimensions out of range";
        return info;
    }
    if (target_w != w || target_h != h) {
        if (!stbir_resize_uint8(px, w, h, 0, dst.data(), target_w, target_h, 0, comp)) {
            stbi_image_free(px);
            info.note = "resize failed";
            return info;
        }
    } else {
        dst.assign(px, px + dst.size());
    }
    stbi_image_free(px);

    int written = 0;
    if (jpeg && comp >= 3) {
        // force RGB for jpeg
        std::vector<unsigned char> rgb((size_t)target_w * target_h * 3);
        for (size_t i = 0; i < (size_t)target_w * target_h; i++) {
            rgb[i * 3 + 0] = dst[i * comp + 0];
            rgb[i * 3 + 1] = dst[i * comp + 1];
            rgb[i * 3 + 2] = dst[i * comp + 2];
        }
        written = stbi_write_jpg(out_path.c_str(), target_w, target_h, 3, rgb.data(), 85);
    } else if (comp == 3) {
        written = stbi_write_png(out_path.c_str(), target_w, target_h, 3, dst.data(), 0);
    } else if (comp == 4) {
        written = stbi_write_png(out_path.c_str(), target_w, target_h, 4, dst.data(), 0);
    } else if (comp == 1) {
        written = stbi_write_png(out_path.c_str(), target_w, target_h, 1, dst.data(), 0);
    } else if (comp == 2) {  // gray+alpha -> expand to RGBA
        std::vector<unsigned char> rgba((size_t)target_w * target_h * 4);
        for (size_t i = 0; i < (size_t)target_w * target_h; i++) {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = dst[i * 2 + 0];
            rgba[i * 4 + 3] = dst[i * 2 + 1];
        }
        written = stbi_write_png(out_path.c_str(), target_w, target_h, 4, rgba.data(), 0);
    }
    if (!written) {
        info.note = "encode failed";
        return info;
    }
    info.ok = true;
    info.w = (uint32_t)target_w;
    info.h = (uint32_t)target_h;
    info.bytes = file_size(out_path);
    info.note = "ok";
    return info;
}

}  // namespace anyps3
