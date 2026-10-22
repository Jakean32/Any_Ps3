# Any_Ps3 — Third-party licenses

Any_Ps3 itself is MIT (see LICENSE). All *runtime* code in `core/` has **zero
external dependencies** (standard library only). Host layers pull the
following permissive-licensed components at build time:

| Component | License | Used by | Notes |
|---|---|---|---|
| Dear ImGui | MIT | hosts/desktop | UI framework (FetchContent) |
| SDL2 | zlib | hosts/desktop | window/input/audio (FetchContent) |
| LLVM (optional, `-DANYPS3_LLVM=ON`) | Apache-2.0 with LLVM exceptions | core AOT backend | not linked by default |
| Vulkan SDK headers (optional, `-DANYPS3_VULKAN=ON`) | Apache-2.0 | core RSX backend | not linked by default |
| Android SDK/NDK (hosts/android) | Apache-2.0 terms | build toolchain | developer tooling only |

No GPL/LGPL/AGPL code is linked into, copied from, or derived from. Formats
knowledge (PARAM.SFO, SELF, ELF64, ISO9660, PPC64/SPU ISA, LV2 syscall numbers)
was implemented from public documentation and independently validated against
GNU binutils output (see tools/reference/); no emulator source was copied.

## Trademark & content notice

"PlayStation" is a trademark of Sony Interactive Entertainment. Any_Ps3 is not
affiliated with or endorsed by Sony. Any_Ps3 contains no Sony firmware,
decryption keys, or copyrighted content, and cannot decrypt retail game
executables. Users must supply legally-obtained content and are responsible
for complying with local law.
