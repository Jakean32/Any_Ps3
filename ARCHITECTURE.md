# Any_Ps3 — Architecture Document (Phase 0)

Any_Ps3 is a PS3 game launcher built on **ahead-of-time (AOT) recompilation** instead of
classic interpretation/JIT emulation. The game's PPU (PowerPC64 / Cell PPE) and SPU code is
translated **once**, at game-import time, into native code for the host CPU (x86-64 or arm64)
and cached per game. At runtime the translated code executes natively; the PS3 operating
system layer (LV2 syscalls, cellOS/SDK libraries) is provided as High-Level Emulation (HLE).

> **Status disclaimer.** This project is a genuine, working engineering skeleton — not a
> finished commercial-game emulator. Everything that compiles and runs is real and is covered
> by automated tests (see `PHASE_REPORTS.md` and `TEST_PLAN.md`). Everything that is not yet
> functional is explicitly listed as such. No Sony firmware, keys, or copyrighted content is
> included or required by the code; users must supply their own legally-dumped games.

---

## 1. Module map

```
+--------------------------------------------------------------+
|                         HOST LAYER                            |
|  hosts/desktop (SDL2 + Dear ImGui, Win/Linux)                 |
|  hosts/android (Kotlin/Compose UI, JNI, SAF import, gamepad)  |
+------------------------------+-------------------------------+
                               | HostAPI (C++)
+------------------------------v-------------------------------+
|                        RUNTIME (core)                         |
|  Emulator      : load game, boot main PPU thread, main loop    |
|  AotCache      : per-game translated-code cache, manifests    |
|  Config        : global + per-game settings (ini-like)        |
+------//------------------+---------------------+-------------+
       ||                 ||                      ||
+------v------+   +-------v--------+    +--------v----------+
|   HLE /     |   |     CPU       |    |      MEMORY       |
|   LV2       |   |  PPU: decoder, |    |  VM: PS3 address  |
|  syscalls   |   |  disasm, IR    |    |  space, pages,    |
|  cellOS     |   |  lifter,       |    |  64KB/1MB granule |
|  (sys_fs,   |   |  backends:     |    |  SPU local store  |
|  threads,   |   |   - MiniAOT    |    +-------------------+
|  tty, spu,  |   |     (x86-64)   |    +-------------------+
|  memory...) |   |   - LLVM (opt) |    |  PPU <-> SPU      |
+-----+-------+   |   - Interpreter|    |  sync objects      |
      |           |     (fallback) |    +-------------------+
      |           +----------------+
+-----v-----------+----------------------------------------------+
|  I/O: PS3FS (host folder / ISO9660), FS overlay, audio, input |
+---------------------------------------------------------------+
```

### Modules and responsibilities

| Module | Path | Responsibility |
|---|---|---|
| **core: types/log** | `core/src/utils/` | Fixed-width types, big-endian readers, logging |
| **PS3FS** | `core/src/fs/` | Virtual filesystem: `/dev_hdd0`, `/app_home`, `/dev_bdvd` mapped to host dirs or ISO image; ISO9660 parser |
| **SFO** | `core/src/sfo/` | PARAM.SFO parser (title, TITLE_ID, category, versions) |
| **Loaders** | `core/src/loader/` | ELF64 PPU loader; SELF container parser (incl. plaintext fSELF); game import pipeline (ISO / folder / standalone EBOOT) |
| **Memory (VM)** | `core/src/memory/` | PS3 32-bit guest address space: user region, mmap region, stacks, page allocation with LV2 granules |
| **PPU core** | `core/src/cpu/ppu/` | PPC64 decoder + disassembler, block lifter to AOT-IR, backends (MiniAOT x64, LLVM, interpreter fallback) |
| **SPU core** | `core/src/cpu/spu/` | SPU (Cell SPE) decoder + interpreter, local store, MFC DMA engine, mailboxes |
| **LV2 HLE** | `core/src/hle/` | Syscall table (449 IDs registered; subset implemented), process/thread/sync objects, sys_fs host-backed files, sys_tty |
| **RSX** | `core/src/gfx/` | GCM command processing skeleton, backends (Null, Vulkan guarded), shader translation cache framework |
| **Audio** | `core/src/audio/` | Backend interface; Null backend; host backends per platform |
| **Input** | `core/src/input/` | Pad state abstraction, per-game button mapping |
| **Runtime** | `core/src/runtime/` | Emulator orchestration, AOT cache manager, boot pipeline |
| **Hosts** | `hosts/` | Desktop UI (Dear ImGui), Android app (Kotlin + NDK), JNI bridge |

---

## 2. AOT pipeline (the core idea)

```
import time (once per game)                 run time
---------------------------                -----------------------------------------
EBOOT/SELF/PRX --> decrypt-if-needed        native block cache lookup (per-PC hash)
        |                                     |
        v                                     v
parse ELF segments into guest memory       execute native block (MiniAOT / LLVM)
        |                                     |
        v                                     v
discover executable regions               syscall boundary -> LV2 HLE (C++)
        |                                     |
        v                                     v
block walk: decode PPC64 -> AOT-IR         block exit -> next block (direct or via
        |                                 dispatch table; unresolved/SMC pages
        v                                 fall back to interpreter)
emit backend code (x86-64 / arm64 via LLVM)
        |
        v
AOT cache: <cache>/<game_id>/<module>/<block>.bin + manifest(hash, sizes)
```

### 2.1 AOT-IR

The lifter decodes PPC64 basic blocks into a compact SSA-ish IR (`AIROp`):
`GprConst, GprLd, GprSt, Alu(op), LoadG, StoreG, Branch(cond,target), Syscall, HostCall, Nop, Trap`.
Three backends consume the same IR:

1. **MiniAOT (x86-64)** — a small, self-contained machine-code emitter. This is the
   *development and validation backend*: it has no external dependencies, runs in this
   repo's CI, and proves the full AOT path (decode -> IR -> native code -> execute)
   end-to-end, including syscall HLE round-trips.
2. **LLVM backend** — `AIROp` -> LLVM IR via the LLVM-C API, compiled to a module and
   cached as object files. Selected at build time (`-DANYPS3_LLVM=ON`); it is the
   production backend for complex code and for **arm64 (Android)**. (Not compiled in
   environments without LLVM dev packages; guarded by CMake option.)
3. **Interpreter (fallback)** — direct PPC64 interpreter used for pages that cannot be
   statically recompiled (self-modifying code, runtime-generated code, unresolved
   indirect branch targets). Falls back transparently at block-dispatch time.

### 2.2 Why AOT (and why a fallback is mandatory)

PS3 executables are mostly static (ELF + SPRX modules), which makes AOT viable:
translation cost is paid once at import, not per frame. However, games *do* generate
or patch code at runtime (JIT shaders via `cellGcm` "relocation", overlay modules,
anti-tamper code). The runtime therefore tracks page write protection: any write into
an executable page invalidates its cached blocks; if a region is repeatedly modified it
is permanently demoted to the interpreter fallback. Unresolved indirect branches
(bctr with unknown target) dispatch through a block table and fall back on miss.

---

## 3. Runtime data flow (game boot)

```
User picks game
  -> GameInfo (from PARAM.SFO) + content root (folder or ISO)
  -> Emulator::boot:
       1. mount PS3FS: /app_home, /dev_bdvd (or folder), /dev_hdd0 (host dir)
       2. read EBOOT.BIN -> SELF unwrap -> ELF64 load -> map segments into VM
       3. AOT cache lookup: manifest {game_id, module hash, lifter/backend version}
            hit  -> load native block table
            miss -> lift blocks now (with progress reporting to UI)
       4. create PPU main thread (context, 1MB stack, entry = e_entry)
       5. main loop: execute blocks; on `sc` -> LV2 dispatch (r11 = number)
  -> render: RSX backend presents; audio: pull from backend ring
```

### 3.1 Guest memory model (Any_Ps3 logical model)

PS3 user space is a 32-bit address space. Physical main RAM is 256 MiB XDR.
Any_Ps3 uses a flat backing store + region table:

| Region | Range | Use |
|---|---|---|
| USER_LOW | `0x00010000` .. `0x00FFFFFF` | ELF segments, static data |
| USER_HIGH | `0x01000000` .. `0x0FFFFFFF` | LV2 allocations, main stack (top `0x0FF00000`), heap |
| MMAP | `0x10000000` .. `0x1FFFFFFF` | `sys_memory_allocate` (64KB/1MB pages), thread stacks, TLS |
| SPURS/SPU | out-of-line | SPU local stores are 256 KiB buffers owned by each SPU thread (LS addr `0x0..0x3FFFF` per SPU), not part of the PPU map |
| RSX | backend-owned | command buffers live in guest RAM; RSX local memory is backend-private |

Allocation granularity is 64 KiB (LV2 `SYS_MEMORY_PAGE_SIZE_64K`) with an option for
1 MiB blocks, mirroring LV2 behavior. Page flags track `read/write/exec/allocated` so
the AOT layer can implement executable-page invalidation.

### 3.2 PPU <-> SPU synchronization

- SPU thread groups (`sys_spu_thread_group_*`) own 1..6 SPU threads with priority,
  state machine (initialized/ready/running/stopped) and join semantics.
- PPU->SPU: SPU mailboxes (inbound mailbox via `sys_spu_thread_write_spu_mb`,
  signal notification registers), MFC DMA commands issued by SPU code.
- SPU->PPU: outbound mailbox (`SPU_WrOutMbox` ch 28), interrupt mailbox
  (`SPU_WrOutIntrMbox` ch 30 -> event queue), atomic cache operations.
- Implemented as lock-protected queues with condition variables; DMA (MFC) does
  bounds-checked copies between SPU LS buffers and guest RAM.

### 3.3 RSX -> Vulkan

- `gfx/` consumes GCM command buffers (put/get pointers in guest memory) and
  presents via a backend interface. Backends: **Null** (records/validates; used by
  tests), **Vulkan** (guarded by `ANYPS3_VULKAN`; device/swapchain setup and
  image presentation path — command translation is stubbed and reported).
- Shader translation: RSX vertex/fragment programs (binary format in GCM commands)
  are decompiled to GLSL and cached on disk keyed by program hash. The cache layer,
  hashing, and invalidation are implemented and tested; the decompiler itself is a
  documented work-in-progress stub.

---

## 4. Build system

- **Core library + tests**: CMake (`cmake_minimum_required(3.16)`, C++17) with no
  external dependencies. A plain `Makefile` is also provided for quick dev builds.
- **Options**: `ANYPS3_LLVM=ON` (LLVM-C backend), `ANYPS3_VULKAN=ON` (Vulkan backend),
  `ANYPS3_BUILD_HOST_DESKTOP=ON` (SDL2 + Dear ImGui host via FetchContent),
  `ANYPS3_BUILD_TESTS=ON` (default ON).
- **Windows**: `scripts/build_windows.ps1` — CMake + MSVC, FetchContent pulls SDL2 and
  Dear ImGui; `scripts/package_windows.ps1` zips `Any_Ps3.exe` + core files.
- **Android**: `hosts/android` Gradle project; `app/src/main/cpp/CMakeLists.txt` builds
  the core via NDK and links the JNI bridge. `scripts/build_android.sh` runs
  `gradlew assembleRelease` and copies the APK. arm64-v8a primary, x86_64 optional.
- **CI/dev host (this environment)**: core + tests built with g++ and run by
  `scripts/run_tests.sh`; MiniAOT emits and executes native x86-64 code.

### 4.1 Third-party components (see LICENSES.md)

| Component | License | Use |
|---|---|---|
| Dear ImGui | MIT | desktop UI |
| SDL2 | zlib | desktop window/input/audio |
| (optional) LLVM | Apache-2.0 w/ exceptions | AOT backend |
| (optional) Vulkan SDK headers | Apache-2.0 | graphics backend |

No GPL/LGPL code is linked or copied. All PS3-format knowledge is implemented from
publicly documented formats (psdevwiki, IBM Cell BE ISA, PS3 SDK documentation) and
independently written; no emulator code was copied.

---

## 5. Legal / first-run design

- Any_Ps3 ships **no firmware, no keys, no game content, no Sony code**.
- **First-run setup screen** requires the user to explicitly confirm:
  (a) they own the games they import, (b) dumps were made legally,
  (c) they will not use it with content they don't own.
- Encrypted retail SELF/EBOOT files cannot be decrypted by Any_Ps3 (no keys shipped).
  Any_Ps3 supports **plaintext/fSELF** executables (typical for homebrew and
  debug-converted content); for retail files the UI explains the limitation and points
  at user-side options without embedding any circumvention material.
- `/dev_flash` content (PSP remaster, PS2 classics, certain PS1 modes) is not supported.

## 6. Risk list

| # | Risk | Severity | Mitigation |
|---|---|---|---|
| R1 | **UFD**-only disc images: many PS3 dumps are UDF 2.5, not ISO9660 | High | ISO9660 implemented first (hybrid + homebrew ISOs); UDF parser is roadmap work; folder dumps fully supported |
| R2 | Encrypted SELF unusable without user keys | High | By design (legal). fSELF path + clear UX |
| R3 | Self-modifying / generated code | Medium | Page-write invalidation + interpreter fallback (implemented at dispatch level) |
| R4 | Indirect branch recovery (vtables, switch tables) | Medium | Block dispatch table; steady state approaches direct chaining |
| R5 | SPU accuracy (MFC, atomics, dual-issue timing) | Medium | Interpreter + DMA engine tested at functional level; timing-sensitive games need SPURS HLE (roadmap) |
| R6 | RSX/GCM command set breadth | High | Backend abstraction + null backend tests; Vulkan translation is a long-tail effort |
| R7 | Cell OS / SDK library breadth (cellSysutil, cellGame, cellFs...) | High | HLE module registry is extensible; a bootstrapping SDK shim lets simple homebrew run today |
| R8 | Performance on Android (Snapdragon) | Medium | AOT removes per-frame decode cost; profile-driven block chaining later |
| R9 | Multi-threaded PPU correctness (6 hardware threads) | Medium | 1:1 host threads with LV2 priority mapping; scheduler co-yield |
| R10 | Legal exposure | High | No keys/firmware; user-supplied content only; explicit first-run consent |

## 7. Honest current status (updated per phase)

See `PHASE_REPORTS.md` — each phase's build/test results and remaining gaps.
