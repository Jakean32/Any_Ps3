# Any_Ps3

An experimental **PlayStation 3 game launcher based on ahead-of-time (AOT)
recompilation** instead of classic interpretation/JIT emulation: the game's PPU
(PowerPC/Cell) and SPU code is translated once at import time into native code
(x86-64 today, arm64 via the LLVM backend path), cached per game, and executed
natively. The PS3 OS layer (LV2 syscalls) is provided as HLE.

> **Honesty first.** This repository is a working engineering foundation, not a
> finished emulator. Everything that compiles is tested (66 automated tests
> on the host — more with the LLVM backend linked — including PPC64 machine code
> produced by the real GNU assembler executed **natively** through the AOT
> pipeline). The launcher app shell runs on **Windows and Android** with the
> real import/boot pipeline, and "Play" never pretends to run a game — it shows
> the real boot log plus "Emulation not implemented yet". Everything not yet
> working is listed in [PHASE_REPORTS.md](PHASE_REPORTS.md). No Sony firmware,
> no decryption keys, and no game content are included or required by the
> code; encrypted ISOs are decrypted only with **your own** disc key.

## What works today (tested)

| Subsystem | Status |
|---|---|
| Game import: ISO9660 images, folder (JB) dumps, PARAM.SFO, ICON0 | ✅ tested |
| **Encrypted ISO (Redump/3k3y) import: AES-128-CBC on-the-fly decrypt with a user .dkey, no copying, "needs key" state** | ✅ tested (test-key fixtures; AES OpenSSL-cross-checked) |
| **Robust PARAM.SFO (0x0004/0x0104/0x0204/0x0404, relative layouts) + SFB/filename metadata fallbacks** | ✅ tested |
| ELF64 PPU loader + SELF container (plaintext fSELF) | ✅ tested (encrypted retail refused **by design**; user's own decrypted EBOOT supported) |
| **PS3 modules: ET_SCE_EXEC, OPD entry (pc+r2 TOC), TLS, argv/envp, sceModuleInfo + proc-prx_param import walk** | ✅ tested (binutils-built PS3-style fixture) |
| **cellOS module HLE: 19 modules / 9040 public NIDs, descriptor slot patching, guest trampolines, honest ENOSYS for the rest** | ✅ tested end-to-end (real sysPrxForUser import + unknown-NID refusal) |
| PPU decoder (integer + FP + VMX + reservations, binutils-validated) | ✅ 239-instruction ground-truth test |
| PPU interpreter (the mandated fallback backend) | ✅ tested with real GNU-assembled programs |
| **PPU floating point (double/single, fused mul-add, compares/converts)** | ✅ tested |
| **PPU VMX subset (24 mnemonics) + lwarx/stwcx reservations** | ✅ tested |
| **PPU threads (LV2 create/join/detach/exit/yield + cooperative scheduler)** | ✅ tested |
| **AOT pipeline: PPC64 → IR → native x86-64 (MiniAOT)** | ✅ hello-world & loops run 100% natively (`ctx.icount == 0`) |
| **LLVM backend: PPC64 → LLVM IR → .ll + x86-64 & arm64 objects, ORC execution** | ✅ tested with `-DANYPS3_WITH_LLVM` (see below) |
| Interpreter fallback for un-liftable blocks | ✅ tested mixed-mode + fault paths |
| Guest memory manager (PS3 layout, 64 KiB pages, exec tracking) | ✅ tested |
| LV2 HLE: process/tty/memory/fs/time syscalls (verified numbers) | ✅ end-to-end hello world |
| SPU: decoder (199-op ISA table), interpreter, mailboxes, MFC DMA | ✅ tested incl. PPU↔SPU syscall loop |
| SPU ELF32 loader + thread groups (create/init/start/join) | ✅ tested |
| Shader cache (content-addressed), audio ring, input mapping | ✅ tested |
| AOT disk cache manifests (module hash + lifter version) | ✅ tested |
| **Built-in hello demo (real pipeline install + boot + execute)** | ✅ tested on Linux, Windows (Wine), and in the Android native lib build |
| **Windows app shell: static Any_Ps3.exe (ImGui + Win32 + D3D11)** | ✅ built; `--selftest` + GUI verified under Wine 10 / Xvfb |
| **Android app shell: arm64-v8a debug-signed APK (Compose)** | ✅ built; verified with aapt2/apkanalyzer/apksigner (no on-device run yet) |
| **Android app v0.6.0: log export to Downloads (MediaStore), in-app log viewer + crash report, high-contrast theme, real adaptive icon** | ✅ build-verified (dex contains the new classes; on-device test is the user's install) |

## Not working yet (see PHASE_REPORTS.md for the full list)

- Encrypted retail SELF/EBOOT: Any_Ps3 ships no keys (legal requirement) —
  supply your own decrypted executable in per-game settings instead.
- UDF disc images (folder dumps + ISO9660 work).
- Full VMX (subset of 24 done), FP exception flags/rounding modes, real GCM
  command parsing, Vulkan rendering, true SMP PPU threads (cooperative only),
  SPURS, cellFs SPRX NIDs (first bring-up log will pin them), per-thread TLS.
- The LLVM backend runs in the dev/CI host (LLVM 19 dev packages present); it
  is NOT yet linked into the shipped Windows/Android hosts (interpreter there).
- Real game execution: "Play" boots the real pipeline and reports exactly
  where it stops (bring-up log). Rendering/pad/audio are the next milestones.

## Building

### Core + tests (Linux/macOS dev host — this is the well-tested path)
```bash
make test          # or: scripts/run_tests.sh
# CMake equivalent:
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```

### Windows (Any_Ps3.exe → Any_Ps3-<version>-Windows-x64.zip)

Cross-build with MinGW-w64 (no Visual Studio needed; also the CI path):
```bash
sudo apt install g++-mingw-w64-x86-64 mingw-w64-x86-64-dev   # Debian/Ubuntu
scripts/build_windows_mingw.sh          # → build/windows-mingw/Any_Ps3.exe
```
The exe is statically linked (`-static -static-libgcc -static-libstdc++`); at
runtime it only loads Windows system DLLs (d3d11, d3dcompiler_47, user32, ...).
Smoke test on any Windows machine: `Any_Ps3.exe --selftest` (exit 0 = the full
demo pipeline passed; writes `selftest.log` next to the exe).
Also available headless: `--diag-test` (diagnostics report), `--crash-test`
(crash handler writes `data/crash_last.txt`).
MSVC path (older alternative): `scripts/build_windows.ps1` (hosts/desktop).

### Android (Any_Ps3-<version>-arm64.apk, arm64-v8a, debug-signed)
```bash
# one-time: install SDK bits under tools/android-sdk (or any ANDROID_HOME):
sdkmanager "platforms;android-35" "build-tools;35.0.0" \
          "ndk;27.2.12479018" "cmake;3.22.1"
scripts/build_android.sh                # → download/Any_Ps3-<version>-arm64.apk
```
The Gradle wrapper is committed (`hosts/android/gradlew`), so only a JDK 17+
is required. In this repo the APK was built with AGP 8.7.3 + Kotlin 2.0.21 +
NDK r27.2 + CMake 3.22.1.

### Versioning
The version string lives in ONE file: `VERSION` at the repo root. It is read
by the root CMakeLists, hosts/windows + hosts/desktop + the Android native
CMakeLists (injected as `ANYPS3_VERSION`; the Android build re-configures
when the file changes), the dev Makefile, and Gradle (versionName). Both
UIs display the compiled-in string, so the app, its binaries and its
packages always agree. `versionCode` is a separate monotonic counter in
Gradle (bumped once per release).

### Optional backends
```bash
cmake -DANYPS3_LLVM=ON ...      # LLVM AOT backend (needs LLVM dev packages)
cmake -DANYPS3_VULKAN=ON ...    # Vulkan RSX backend skeleton (needs Vulkan SDK)
```

### CI (reproducibility)
`.github/workflows/` builds and tests everything on every push:
`core-tests.yml` (37 tests), `windows-build.yml` (MinGW cross-build + zip
artifact), `android-build.yml` (APK artifact + verification).

## Using it (first run)

1. **Legal notice:** the app asks for a one-time confirmation that you only
   import games you own and dumped legally.
2. The library starts with one built-in entry, "Hello World (Any_Ps3 demo)" —
   the same PPC64 ELF the automated tests validate. **Play** on it executes it
   for real (PPU interpreter; prints "Hello from Any_Ps3!" via emulated
   `sys_tty_write`, exits with code 42).
3. **Add game** — pick a PS3 game folder (JB dump) or ISO image. The title,
   ID and cover icon are read from PARAM.SFO/ICON0. Windows: the file dialog
   alternates between ISO and folder mode; Android: Storage Access Framework.
4. **Encrypted retail EBOOT** — the disc executable is an encrypted SELF;
   Any_Ps3 ships no keys. Per-game settings → "Decrypted executable" → pick
   your own decrypted ELF/fSELF (from your own legal dump, with your own
   tools) and Play boots it. Retail-encrypted files are refused honestly.
5. **Play on a real game** — runs the real boot pipeline and shows the
   bring-up log: linked imports, MISSING exports (called ones first), the
   first unimplemented syscall by name. Rendering is not implemented yet —
   send that log back and the next release fixes the top entries.
6. **Settings** — per-game (disc key, decrypted executable, PPU backend,
   controller mapping) and global (data/cache locations, log level).

## Repository layout
```
core/            portable C++17 core (no external dependencies)
  memory/        PS3 guest memory (flat 512 MiB window, 64 KiB pages)
  cpu/ppu/       decoder, interpreter, AOT-IR lifter, MiniAOT x64, LLVM slot
  cpu/spu/       SPU decoder (generated table), interpreter, MFC/mailboxes
  demo/          built-in hello-world demo (shipped sample ELF)
  hle/           LV2 syscalls (verified numbers) + sys_spu manager
  gfx/           RSX backend interface, null backend, shader cache
  audio/ input/  backends + mapping
  loader/ fs/    ELF/SELF loaders, ISO9660, import pipeline, library
  runtime/       Emulator orchestration, AOT disk cache
  platform.h     portability layer (Linux / MinGW / Android NDK)
hosts/
  windows/       Dear ImGui + Win32 + D3D11 host (single static exe)
  android/       Kotlin/Compose host + NDK build + JNI bridge + gradlew
  desktop/       older SDL2 host (kept for the MSVC build_windows.ps1 path)
third_party/     vendored: Dear ImGui 1.91.9b (MIT), stb_image (public domain)
tests/           37 tests incl. GNU-assembled PPC64 fixtures (see fixtures/)
tools/reference/ verified syscall table, SPU ISA rows, binutils ground truth
.github/         CI: core tests + Windows cross-build + Android APK
```

## Legal

MIT licensed (see LICENSE). You must supply your own legally-dumped games and
content. Any_Ps3 ships no Sony code, firmware, or keys and cannot decrypt
retail executables. See LICENSES.md for third-party component licenses.

Do not ask for or distribute key material, firmware, or copyrighted game data
in relation to this project.
