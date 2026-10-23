#!/usr/bin/env python3
"""Generate Any_Ps3 test fixtures:
- fixtures/game_dir/PS3_GAME/{PARAM.SFO,ICON0.PNG,PIC1.PNG,EBOOT.BIN}
- fixtures/game_dir2/PS3_GAME/... (second game)
- fixtures/game.iso  (real ISO9660 built with genisoimage)
- fixtures/test.elf  (craft PPC64 BE ELF for loader tests)
"""
import os, struct, subprocess, shutil, sys
from PIL import Image, ImageDraw

BASE = "/home/z/my-project/fixtures"
GENISO = "/home/z/my-project/tools/mingw-root/usr/bin/genisoimage"

def make_sfo(fields):
    """fields: list of (key, value). Strings as UTF-8-special(0x0004),
    ints (python int) as 0x0204. Layout per PS3 PARAM.SFO format."""
    hdr = b"\x00PSF" + struct.pack("<HI", 0x101, 0)  # magic, version 1.1, dummy entries
    entries = []
    keys = []
    data = []
    for k, v in fields:
        if isinstance(v, int):
            entries.append((k, 0x0204, 4, 4, v))
        else:
            b = v.encode("utf-8")
            entries.append((k, 0x0004, len(b) + 4, (len(b) + 4 + 3) & ~3, v))
    key_off = 0
    key_blob = b""
    data_off = 0
    data_blob = b""
    key_positions = []
    data_positions = []
    for k, fmt, ln, maxl, v in entries:
        key_positions.append(len(key_blob))
        key_blob += k.encode() + b"\x00"
        data_positions.append(len(data_blob))
        if fmt == 0x0204:
            data_blob += struct.pack("<I", v)
        else:
            b = v.encode("utf-8")
            data_blob += struct.pack("<I", len(b)) + b
            pad = (maxl - (len(b) + 4))
            data_blob += b"\x00" * max(0, pad)
    key_table_start = 20 + 16 * len(entries)
    data_table_start = key_table_start + len(key_blob)
    out = b"\x00PSF" + struct.pack("<III", 0x101, key_table_start, data_table_start)
    out += struct.pack("<I", len(entries))
    for (k, fmt, ln, maxl, v), kpos, dpos in zip(entries, key_positions, data_positions):
        out += struct.pack("<HHII", kpos & 0xFFFF, fmt, ln, maxl) + struct.pack("<I", dpos)
    out += key_blob
    out += data_blob
    return out

def make_png(path, w, h, color, label):
    im = Image.new("RGB", (w, h), color)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, w - 1, h // 6], fill=(0, 0, 0))
    d.text((8, 6), label, fill=(255, 255, 255))
    im.save(path)

def make_ppc_elf():
    """Craft a small PPC64 big-endian ELF with one R+X segment containing
    hand-encoded PowerPC instructions."""
    # instructions (word index -> encoding)
    # vaddr base 0x10000
    base = 0x10000
    words = [
        0x48000064,  # b      .+0x64        -> 0x10064   (direct branch)
        0x480000FD,  # bl     from 0x10004  -> 0x10100   (direct call, LK=1)
        0x4E800020,  # blr                              (indirect: bclr xo=16)
        0x60000000,  # ori r0,r0,0 (nop — not a branch)
        0x41820008,  # bc    (op16) +8 -> 0x10010+8      (conditional direct branch)
        0x4E800020,  # blr again
        0xFFFFFFFF,  # data word
        0xFFFFFFFF,
    ] + [0x60000000] * 72  # padding so target 0x10100 lands inside (0x10100-0x10000)/4 = 0x40
    code = b"".join(struct.pack(">I", w) for w in words)
    assert base + len(code) >= 0x10104, "call target must be inside segment"
    ehsz, phentsize, phnum = 64, 56, 1
    phoff = 64
    code_off = 0x1000
    total = code_off + len(code)
    eh = b"\x7fELF" + bytes([2, 2, 1, 0]) + b"\x00" * 8
    eh += struct.pack(">HHI", 2, 21, 1)          # e_type=EXEC, e_machine=EM_PPC64, e_version
    eh += struct.pack(">QQQ", base, phoff, 0)    # entry, phoff, shoff
    eh += struct.pack(">IHHHHHH", 0, ehsz, phentsize, phnum, 0, 0, 0)
    # e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx
    eh += b"\x00" * (phoff - len(eh))
    ph = struct.pack(">IIQQQQQQ", 1, 5, code_off, base, base, len(code), len(code), 0x1000)
    eh += ph
    eh += b"\x00" * (code_off - len(eh))
    eh += code
    return eh

def main():
    shutil.rmtree(BASE, ignore_errors=True)

    # ---- game 1 (folder + iso content) ----
    g1 = os.path.join(BASE, "game_root", "PS3_GAME")
    os.makedirs(g1, exist_ok=True)
    sfo1 = make_sfo([
        ("APP_VER", "01.00"),
        ("CATEGORY", "GD"),
        ("PARENTAL_LEVEL", 1),
        ("PS3_SYSTEM_VER", "04.2000"),
        ("TITLE", "AnyPs3 Test Game"),
        ("TITLE_ID", "NPEA00001"),
    ])
    open(os.path.join(g1, "PARAM.SFO"), "wb").write(sfo1)
    make_png(os.path.join(g1, "ICON0.PNG"), 320, 176, (30, 90, 160), "ICON0 NPEA00001")
    make_png(os.path.join(g1, "PIC1.PNG"), 640, 360, (16, 60, 110), "PIC1 NPEA00001")
    open(os.path.join(g1, "EBOOT.BIN"), "wb").write(make_ppc_elf())

    # ---- game 2 (folder only) ----
    g2 = os.path.join(BASE, "game_dir2", "PS3_GAME")
    os.makedirs(g2, exist_ok=True)
    sfo2 = make_sfo([
        ("APP_VER", "01.02"),
        ("CATEGORY", "HG"),
        ("PARENTAL_LEVEL", 3),
        ("TITLE", "Second Fixture Game"),
        ("TITLE_ID", "BLUS30002"),
    ])
    open(os.path.join(g2, "PARAM.SFO"), "wb").write(sfo2)
    make_png(os.path.join(g2, "ICON0.PNG"), 320, 176, (140, 40, 40), "ICON0 BLUS30002")

    # ---- folder-copy of game 1 for direct folder import tests ----
    g1dir = os.path.join(BASE, "game_dir")
    shutil.copytree(os.path.join(BASE, "game_root"), g1dir)

    # ---- ISO built with genisoimage (real ISO9660) ----
    iso = os.path.join(BASE, "game.iso")
    r = subprocess.run([GENISO, "-V", "ANyps3FIXTURE", "-iso-level", "1", "-no-pad",
                        "-o", iso, os.path.join(BASE, "game_root")],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("genisoimage failed:", r.stdout, r.stderr)
        sys.exit(1)
    print("iso size:", os.path.getsize(iso))

    # ---- standalone elf for loader tests ----
    open(os.path.join(BASE, "test.elf"), "wb").write(make_ppc_elf())
    # ---- garbage "encrypted" binary ----
    open(os.path.join(BASE, "encrypted.bin"), "wb").write(
        b"NOTANELF" + bytes(range(256)) * 4)

    print("fixtures in", BASE)
    for root, dirs, files in os.walk(BASE):
        for f in files:
            print(" ", os.path.relpath(os.path.join(root, f), BASE), os.path.getsize(os.path.join(root, f)))

if __name__ == "__main__":
    main()
