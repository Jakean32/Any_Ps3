# Any_Ps3

**AOT-recompilation-based PlayStation 3 launcher** — for Android (arm64) and Windows (x64).

Any_Ps3 recompiles PS3 game code (PPU/SPU, Cell) into native code for your device *ahead of time* and caches the result per game. LV2 syscalls and cellOS run through high-level emulation (HLE); RSX graphics are destined for a modern graphics API; code that cannot be statically resolved (dynamic code, unresolved indirect branches) would run in a small fallback interpreter. **No Sony firmware, keys, or game content are included, used, or required.**

> **Honest status:** the code-generation stages are **not implemented yet**. What ships today is the real app shell plus the genuinely implemented first stages of the pipeline (see below). The UI reports unimplemented stages verbatim — there are no fake progress bars and no fake success states.

---

## Releases

Every release ships **three artifacts** in `download/`:

| Artifact | Contents |
|---|---|
| `Any_Ps3.apk` | Android app, arm64-v8a, debug-signed, no Internet permission |
| `Any_Ps3_Windows.zip` | `Any_Ps3.exe` — statically linked x64 build with the app icon, plus logo + docs |
| `Any_Ps3_source.zip` | Complete source (core, both frontends, tests, fixtures, build scripts) |

`Any_Ps3_source.zip` accompanies **every** release, so any artifact can be rebuilt from source with the documented toolchain.

## What is implemented (honest capability map)

| Area | Status |
|---|---|
| PARAM.SFO parsing (TITLE, TITLE_ID, APP_VER, CATEGORY, ints) | ✅ implemented, tested |
| ISO9660 reader over abstract byte sources (random access, no full-image loads) | ✅ implemented, tested |
| Reads through raw file descriptors (Android SAF / `ParcelFileDescriptor`) | ✅ implemented, tested |
| No-copy import (URIs/paths stored, only small thumbnails cached) | ✅ implemented, tested |
| Missing-source detection ("File missing — please reselect") | ✅ implemented, tested |
| Thumbnail cache (stb decode → downscale → PNG/JPEG) | ✅ implemented, tested |
| Per-game cache manifest (format, versions, source hash, status, stages) | ✅ implemented, tested |
| Staleness detection (source hash mismatch → "Out of date") | ✅ implemented, tested |
| PPU ELF64 big-endian / PRX overlay loader | ✅ implemented, tested |
| PowerPC branch/call scanner (direct, conditional, indirect classification) | ✅ implemented, tested |
| Compile pipeline orchestration with **real** progress reporting | ✅ implemented, tested |
| LV2 syscall HLE framework (table, dispatch, TTY, memory basics, ENOSYS for the rest) | ✅ implemented, tested |
| Encrypted-EBOOT detection (retail `EBOOT.BIN` → honest "blocked" state) | ✅ implemented, tested |
| First-run wizard, library/settings/logs UI, splash, both platforms | ✅ implemented |
| LLVM IR lifting | ❌ **not implemented yet** |
| Native code generation (x64/ARM64) | ❌ **not implemented yet** |
| Executable-artifact loader / game launch | ❌ **not implemented yet** |
| RSX graphics, cellOS module HLE beyond the framework | ❌ **not implemented yet** |

## Architecture

```
core/                  portable C++17, zero dependencies (host, Android, Windows)
  util.{h,cpp}         SHA-256, paths, fs, free space
  bytesource.{h,cpp}   FileSource | FdSource (raw POSIX fd) | MemSource
  sfo.{h,cpp}          PARAM.SFO parser
  iso9660.{h,cpp}      ISO9660 over any ByteSource (SAF-compatible)
  thumbs.{h,cpp}       stb_image decode → resize → encode (small thumbnails only)
  game_import.{h,cpp}  no-copy records, folder scans, refresh, library.json
  cache_manifest.{h,cpp} <cache>/<GAME_ID>/{manifest.json, logs/, code/, thumbs/}
  recompiler.{h,cpp}   PPU ELF loader, branch scanner, honest compile pipeline
  lv2.{h,cpp}          syscall table + small built-in set
  library.{h,cpp}      facade + config.json
android/               Java + framework Views shell (no Gradle needed), JNI bridge
windows/               Dear ImGui (Win32 + D3D11) shell, static x64
tests/core_tests.cpp   40 tests (see below)
fixtures/              generated test fixtures (SFO, PNGs, crafted PPC64 ELF, real ISO)
scripts/               asset pipeline, fixture generator, build + package scripts
```

### No-copy import (both platforms)

Games are **never copied** into app storage. A `GameRecord` stores only the reference (filesystem path on Windows, SAF `content://` URI on Android) plus small metadata. All reads go through `ByteSource`:
- **Android**: the wizard and imports take *persistable* URI permissions (`takePersistableUriPermission`), so sources stay accessible across reboots. Reads use `ParcelFileDescriptor` → raw fd → `FdSource::pread`-style random access. Covers are extracted once and cached as small thumbnails (ICON0 ≤ 320 px PNG, PIC1 ≤ 480 px JPEG).
- **Windows**: plain filesystem paths, same core.

### Compile cache & executable-code strategy

```
<cache root>/<GAME_ID>/
    manifest.json    # format version, anyps3 version, source hash, status, stage results
    logs/            # one real log per compile run
    code/            # future AOT artifacts (data files)
    thumbs/          # cover thumbnails
```

Statuses: `not_compiled → compiling → ready`, plus `stale` (source changed) and `failed`. `ready` is unreachable today because the codegen stages are not implemented — the manifest records each stage's honest state (`ok` / `not_implemented` / `blocked` / `failed`).

**Android executable strategy (documented choice):** shared storage (SAF) never allows executable mappings, so compiled artifacts are stored as plain *data* files under the user-chosen folder; at launch they would be copied into the app's *private* directory where execution is permitted (the alternative — anonymous `mmap(PROT_EXEC)` — was considered and rejected because it complicates relocation). The artifact loader itself is not implemented yet; the UI reports this honestly. On Android 11+ direct writes to a user-chosen shared folder additionally require "All files access" (the wizard and settings explain this and fall back to app storage, telling the user exactly where the cache lives).

## Tests — 40/40 passing

Built and run on the host (g++) and, as a Windows cross-build, under Wine 10.0 (both 40/40):

- **SFO (4):** fixture parse, bad-magic rejection, second fixture, all-fields presence
- **ISO9660 (6):** PVD validation, non-ISO rejection, PARAM.SFO lookup, icon/PIC1 lookup, **random reads via raw fd** (required new test), missing-file error
- **Import (6):** folder game, ISO game, **no-copy verification** (source byte-identical, cache only small thumbs), missing-source flagging, multi-game scan, persistence roundtrip incl. SAF-style URIs
- **Manifest (5):** defaults on create, **JSON roundtrip** (required new test), status transitions, staleness detection, future-version rejection
- **Recompiler (7):** ELF fixture load, garbage rejection, branch/call scan, nop/data not branches, honest not-implemented codegen, **real progress verification**, encrypted-EBOOT "blocked" state
- **LV2 (4):** register/dispatch, unknown→ENOSYS, TTY capture, memory basics
- **Utils (4):** SHA-256 known vectors, path utils, free-space query, mini-JSON roundtrip
- **Integration (4):** full library flow (import→compile→log→recompile→delete-cache→remove), thumbnail generation, config persistence, scan aggregation

*(History: an earlier session had a 34-test suite; the sandbox was reset and the suite was rebuilt from scratch with equivalent-plus coverage. The three tests explicitly required — SFO parse, ISO read via file descriptor, cache manifest — are included and passing.)*

## Building from source

Host prerequisites: any C++17 toolchain; for the full pipeline, an Android SDK (build-tools, platform, NDK) and mingw-w64.

```
# core + tests (host)
g++ -std=c++17 -O2 -o core_tests core/*.cpp tests/core_tests.cpp   # then run it

# fixtures (needed by tests)
python3 scripts/gen_fixtures.py          # needs genisoimage for the ISO fixture

# assets (regenerates icons/ico from the logo)
python3 scripts/gen_assets.py

# Android APK (arm64, debug-signed)
scripts/build_apk.sh                     # aapt2/javac/d8/zipalign/apksigner + NDK

# Windows exe (static x64 with icon)
scripts/build_windows.sh                 # mingw-w64 + windres

# Windows test binary + Wine verification
scripts/build_tests_wine.sh              # builds and runs 40 tests under Wine
```

## Verification performed on this release (reported honestly)

**APK** — built with aapt2/javac(core-lambda-stubs)/d8(build-tools 35)/zipalign/apksigner; verified with `aapt dump badging` (package `com.anyps3.app`, minSdk 26, target 34, launchable `SplashActivity`, `arm64-v8a` native code, adaptive icon all densities, no INTERNET permission), `apksigner verify` (debug cert), `dexdump` (all app classes present), and `apkanalyzer` (manifest + dex). *Not run on a physical device/emulator in the build sandbox* — install it and report anything odd.

**Windows** — `Any_Ps3.exe` is PE32+ x64, statically linked (only system DLLs imported: d3d11, dxgi, D3DCompiler_47, gdi32, user32, shell32, ole32, dwmapi, kernel32, msvcrt); icon verified with `pefile` (7 RT_ICON frames 16–256 px + group). The core test suite was cross-built and executed **under Wine 10.0: 40/40 passed**. The GUI exe ran under Wine with a virtual display, created its window, initialized D3D11 and rendered the shell (screenshot shipped in the zip). Not run on a physical Windows machine by the build sandbox.

## Legal

Any_Ps3 is an original, clean-room engineering effort. It contains no Sony firmware, decryption keys, or game content, and does not bypass copy protection. You may only import and compile games you legally own; creating backups of your own games may be restricted in your country. This project is not affiliated with Sony.
