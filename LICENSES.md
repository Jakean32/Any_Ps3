# Any_Ps3 — Third-party licenses

Any_Ps3 itself is MIT (see LICENSE). All *runtime* code in `core/` has **zero
external dependencies** (standard library only). Host layers pull the
following permissive-licensed components at build time:

| Component | License | Used by | Notes |
|---|---|---|---|
| Dear ImGui 1.91.9b (vendored, third_party/imgui) | MIT | hosts/windows | static UI toolkit + Win32/DX11 backends + imgui_stdlib |
| stb_image.h (vendored, third_party/stb) | public domain | hosts/windows | ICON0.PNG cover decoding |
| Dear ImGui | MIT | hosts/desktop | via FetchContent there |
| SDL2 | zlib | hosts/desktop | via FetchContent (MSVC path) |
| MinGW-w64 runtime | runtime exception | Windows exe | statically linked libgcc/libstdc++ into the MIT exe |
| LLVM (optional, `-DANYPS3_LLVM=ON`) | Apache-2.0 with LLVM exceptions | core AOT backend | not linked by default |
| Vulkan SDK headers (optional, `-DANYPS3_VULKAN=ON`) | Apache-2.0 | core RSX backend | not linked by default |
| Android SDK/NDK (hosts/android) | Apache-2.0 terms | build toolchain | developer tooling only |
| Kotlin/Compose + AndroidX | Apache-2.0 | hosts/android (runtime UI) | compiled into the APK; Apache-2.0 is permissive for redistribution |

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
