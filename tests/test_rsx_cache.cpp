// RSX + shader cache + audio ring + input mapping + AOT disk cache tests.
#include "framework.h"

#include <cstdio>

#include "anyps3/audio/audio.h"
#include "anyps3/gfx/rsx.h"
#include "anyps3/input/input.h"
#include "anyps3/loader/elf.h"
#include "anyps3/runtime/emulator.h"

using namespace anyps3;

TEST(rsx_null_backend_and_test_stream) {
    std::unique_ptr<RsxBackend> b(CreateNullBackend());
    CHECK_TRUE(b->Initialize(nullptr));
    RsxCommandProcessor proc(b.get());
    // Build a BE command buffer in the documented test format.
    std::vector<u32> be;
    auto cmd = [&](u32 c) {
        u8 t[4];
        WriteBE32(t, c);
        u32 v;
        std::memcpy(&v, t, 4);
        be.push_back(v);
    };
    cmd(0x01000003);  // draw x3
    cmd(0xF00000AA);   // label 0xAA
    cmd(0xFF000000);   // present
    cmd(0x77777777);   // unknown
    proc.Execute(be.data(), be.size());
    CHECK_EQ(proc.stats().draws, 3);
    CHECK_EQ(proc.last_label(), 0xAA);
    CHECK_EQ(proc.stats().flips, 1);
    CHECK_EQ(proc.stats().unknown_cmds, 1);
    b->Shutdown();
}

TEST(shader_cache_roundtrip) {
    std::string root = "/tmp/anyps3_shader_cache_test";
    std::string cmd = "rm -rf " + root;
    std::system(cmd.c_str());
    ShaderCache cache(root);
    const u8 program[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    u64 key = ShaderCache::Key(program, sizeof(program));

    std::vector<u8> blob;
    CHECK_TRUE(!cache.Lookup(key, &blob));  // miss
    cache.Store(key, "decompiled-shader", 17);
    CHECK_TRUE(cache.Lookup(key, &blob));  // hit
    CHECK_STR_EQ(std::string(reinterpret_cast<const char*>(blob.data()), blob.size()),
                 "decompiled-shader");
    CHECK_EQ(cache.EntryCount(), 1);
    // Key is content-addressed: different data -> different key -> miss.
    const u8 program2[] = {0x01, 0x02, 0x03, 0x04, 0x06};
    u64 key2 = ShaderCache::Key(program2, sizeof(program2));
    CHECK_TRUE(key2 != key);
    std::vector<u8> blob2;
    CHECK_TRUE(!cache.Lookup(key2, &blob2));
    // Invalidate removes the blob.
    CHECK_TRUE(cache.Invalidate(key));
    CHECK_TRUE(!cache.Lookup(key, &blob));
    CHECK_EQ(cache.EntryCount(), 0);
    std::system(cmd.c_str());
}

TEST(audio_ring_basic) {
    AudioRing ring(64);
    s16 src[8] = {1, 2, 3, 4, 5, 6, 7, 8};  // 4 samples
    CHECK_EQ(ring.Write(src, 4), 4);
    CHECK_EQ(ring.available(), 4);
    s16 dst[16] = {};
    size_t got = ring.Read(dst, 8);  // 4 real + 4 underrun silence
    CHECK_EQ(got, 4);
    CHECK_EQ(dst[0], 1);
    CHECK_EQ(dst[6], 7);
    CHECK_EQ(dst[7], 8);
    CHECK_EQ(dst[8], 0);  // underrun silence
    CHECK_EQ(dst[9], 0);
    CHECK_EQ(ring.available(), 0);
}

TEST(input_mapping) {
    GameSettings gs;
    gs.controller["cross"] = 13;   // host keycode 13
    gs.controller["start"] = 40;
    PadMapping m;
    m.Load(gs);
    CHECK_EQ(m.size(), 2);
    PadState pad;
    u32 guest = m.Translate(13, true, &pad);
    CHECK_EQ(guest, PAD_CROSS);
    CHECK_TRUE(pad.Held(PAD_CROSS));
    CHECK_TRUE(!pad.Held(PAD_START));
    guest = m.Translate(40, true, &pad);
    CHECK_EQ(guest, PAD_START);
    m.Translate(13, false, &pad);
    CHECK_TRUE(!pad.Held(PAD_CROSS));
    CHECK_TRUE(pad.Held(PAD_START));
    // unmapped host code
    CHECK_EQ(m.Translate(999, true, &pad), PAD_NONE);
}

TEST(aot_disk_cache_manifest) {
    // AotCache manifests: store -> load -> clear; hash change invalidates.
    std::string root = "/tmp/anyps3_aot_cache_test";
    std::system(("rm -rf " + root).c_str());
    AotCache cache(root);

    ElfImage img;
    ElfSegment seg;
    seg.vaddr = 0x10000;
    seg.flags = 0x5;  // PF_X
    seg.bytes = {0x38, 0x60, 0x00, 0x01, 0x60, 0x00, 0x00, 0x00};
    seg.file_size = seg.bytes.size();
    seg.mem_size = seg.bytes.size();
    img.segments.push_back(seg);

    AotCache::Manifest m;
    m.game_id = "TEST00001";
    m.module_hash = AotCache::HashModule(img);
    m.lifter_version = 1;
    m.backend_id = 1;
    m.block_count = 7;
    CHECK_TRUE(cache.Store("TEST00001", m, {1, 2, 3}));

    AotCache::Manifest loaded;
    CHECK_TRUE(cache.Load("TEST00001", &loaded));
    CHECK_STR_EQ(loaded.game_id, "TEST00001");
    CHECK_EQ(loaded.block_count, 7);
    CHECK_EQ(loaded.module_hash, AotCache::HashModule(img));

    // Hash is stable for identical code.
    CHECK_EQ(AotCache::HashModule(img), AotCache::HashModule(img));
    // Different code -> different hash.
    ElfImage img2 = img;
    img2.segments[0].bytes[3] = 0x02;
    CHECK_TRUE(AotCache::HashModule(img2) != AotCache::HashModule(img));

    CHECK_TRUE(cache.Clear("TEST00001"));
    AotCache::Manifest gone;
    CHECK_TRUE(!cache.Load("TEST00001", &gone));
}
