# Any_Ps3

An experimental **PlayStation 3 game launcher based on ahead-of-time (AOT)
recompilation** instead of classic interpretation/JIT emulation: the game's PPU
(PowerPC/Cell) and SPU code is translated once at import time into native code
(x86-64 today, arm64 via the LLVM backend path), cached per game, and executed
natively. The PS3 OS layer (LV2 syscalls) is provided as HLE.

> **Honesty first.** This repository is a working engineering foundation, not a
> finished emulator. Everything that compiles is tested (34 automated tests,
> including PPC64 machine code produced by the real GNU assembler executed
> **natively** through the AOT pipeline). Everything not yet working is listed
> in [PHASE_REPORTS.md](PHASE_REPORTS.md). No Sony firmware, no decryption
> keys, and no game content are included or required by the code.

## What works today (tested)

| Subsystem | Status |
|---|---|
| Game import: ISO9660 images, folder (JB) dumps, PARAM.SFO, ICON0 | ✅ tested |
| ELF64 PPU loader + SELF container (plaintext fSELF) | ✅ tested (encrypted retail refused **by design**) |
| PPU decoder (~90 mnemonics, binutils-validated) | ✅ 150-instruction ground-truth test |
| PPU interpreter (the mandated fallback backend) | ✅ tested with real GNU-assembled programs |
| **AOT pipeline: PPC64 → IR → native x86-64 (MiniAOT)** | ✅ hello-world & loops run 100% natively (`ctx.icount == 0`) |
| Interpreter fallback for un-liftable blocks | ✅ tested mixed-mode + fault paths |
| Guest memory manager (PS3 layout, 64 KiB pages, exec tracking) | ✅ tested |
| LV2 HLE: process/tty/memory/fs/time syscalls (verified numbers) | ✅ end-to-end hello world |
| SPU: decoder (199-op ISA table), interpreter, mailboxes, MFC DMA | ✅ tested incl. PPU↔SPU syscall loop |
| SPU ELF32 loader + thread groups (create/init/start/join) | ✅ tested |
| Shader cache (content-addressed), audio ring, input mapping | ✅ tested |
| AOT disk cache manifests (module hash + lifter version) | ✅ tested |

## Not working yet (see PHASE_REPORTS.md for the full list)

- Encrypted retail SELF/EBOOT (no keys shipped — legal requirement).
- UDF disc images (folder dumps + ISO9660 work).
- VMX/AltiVec, FP arithmetic, real GCM command parsing, Vulkan rendering,
  multi-threaded PPU, SPURS, cellOS module HLE.
- The LLVM backend is compile-guarded and **untested in this repo's CI**
  (the dev environment has no LLVM packages).

## Building

### Core + tests (Linux/macOS dev host — this is the well-tested path)
```bash
make test          # or: scripts/run_tests.sh
# CMake equivalent:
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```

### Windows (Any_Ps3.exe → zip)
```powershell
scripts/build_windows.ps1     # needs VS2022 + CMake; deps fetched automatically
```

### Android (Any_Ps3.apk, arm64-v8a / x86_64)
```bash
scripts/build_android.sh      # needs Android SDK + NDK 27 + JDK 17
```

### Optional backends
```bash
cmake -DANYPS3_LLVM=ON ...      # LLVM AOT backend (needs LLVM dev packages)
cmake -DANYPS3_VULKAN=ON ...    # Vulkan RSX backend skeleton (needs Vulkan SDK)
```

## Using it (first run)

1. **Legal notice:** the app asks for a one-time confirmation that you only
   import games you own and dumped legally.
2. **Add game** — pick a PS3 game folder (JB dump) or ISO image. The title,
   ID and icon are read from PARAM.SFO.
3. **Run** — boots the executable through the AOT runtime with interpreter
   fallback; the runtime log shows AOT cache hits/misses and fallbacks.
4. **Per-game settings** — resolution scale, frame limit, shader cache,
   PPU backend (auto/interpreter), controller mapping.

## Repository layout
```
core/            portable C++17 core (no external dependencies)
  memory/        PS3 guest memory (flat 512 MiB window, 64 KiB pages)
  cpu/ppu/       decoder, interpreter, AOT-IR lifter, MiniAOT x64, LLVM slot
  cpu/spu/       SPU decoder (generated table), interpreter, MFC/mailboxes
  hle/           LV2 syscalls (verified numbers) + sys_spu manager
  gfx/           RSX backend interface, null backend, shader cache
  audio/ input/  backends + mapping
  loader/ fs/    ELF/SELF loaders, ISO9660, import pipeline, library
  runtime/       Emulator orchestration, AOT disk cache
hosts/
  desktop/       SDL2 + Dear ImGui host (Windows/Linux)
  android/       Kotlin/Compose host + NDK build + JNI bridge
tests/           34 tests incl. GNU-assembled PPC64 fixtures (see fixtures/)
tools/reference/ verified syscall table, SPU ISA rows, binutils ground truth
```

## Legal

MIT licensed (see LICENSE). You must supply your own legally-dumped games and
content. Any_Ps3 ships no Sony code, firmware, or keys and cannot decrypt
retail executables. See LICENSES.md for third-party component licenses.

Do not ask for or distribute key material, firmware, or copyrighted game data
in relation to this project.
