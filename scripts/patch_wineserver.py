#!/usr/bin/env python3
"""Patch wineserver64's hardcoded nls data paths to a user-owned directory.
Replacements must be <= original length (padded with NUL bytes)."""
import sys

SRC = "/home/z/my-project/tools/wine-root/usr/lib/wine/wineserver64"
DST = "/home/z/my-project/tools/wine-root/usr/lib/wine/wineserver64"
NEW_DIR = "/home/z/aw/nls"          # 14 chars + nul

data = bytearray(open(SRC, "rb").read())
replacements = [
    b"/usr/share/wine/wine/nls",     # 23 chars
    b"/usr/local/share/wine/nls",    # 23
    b"/usr/share/wine/nls",          # 19
]
count = 0
for old in replacements:
    off = data.find(old)
    while off != -1:
        # ensure room: replace with NEW_DIR + NUL padding to same length
        assert len(NEW_DIR) <= len(old)
        data[off:off + len(old) + 1] = NEW_DIR.encode() + b"\x00" * (len(old) - len(NEW_DIR) + 1)
        count += 1
        off = data.find(old)
open(DST, "wb").write(bytes(data))
print(f"patched {count} path strings in wineserver64")
