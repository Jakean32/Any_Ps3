# Any_Ps3 — Phase Reports (honest capability matrix)

> After every phase: what was built, what was tested, what works, what doesn't.
> Test environment for all "verified" claims: Debian trixie x86-64, g++ 14.2,
> GNU binutils 2.44 powerpc64 as ground truth for PPC64 encodings.

## Phase 0 — Architecture document
- **Delivered**: `ARCHITECTURE.md` (modules, AOT pipeline, memory model, sync model,
  build system, risk list).
- **Status**: complete.

## Phase 1 — Skeleton, formats, import pipeline
- **Delivered**: CMake + Makefile builds; zero-dependency core library; logging;
  config/global+per-game settings; ISO9660 reader **and** minimal ISO writer
  (tests build synthetic game images and round-trip them); PARAM.SFO parser +
  builder; ELF64 loader; SELF container parser (plaintext fSELF supported,
  encrypted retail refused by design — no keys shipped); game import pipeline
  (ISO / folder / library index with per-game manifests); UI hosts
  (desktop Dear ImGui + Android Gradle/Kotlin/Compose — source-complete,
  see caveats).
- **Verified by tests** (all pass): SFO parse/build/handmade-layout; ISO
  read/write round trip; import from ISO and from folder; rejection cases.
- **Not working / not implemented**:
  - UDF (most retail disc dumps are UDF 2.5, not ISO9660) — folder dumps work
    today; UDF is roadmap work (risk R1).
  - Encrypted retail SELF — **by design** (legal): Any_Ps3 ships no keys and
    cannot decrypt retail EBOOTs. Plaintext fSELF / plain ELF only.
  - Android .apk and Windows .exe were NOT built in this environment (no
    Android SDK/NDK, no MSVC/SDL). Hosts are complete sources + build scripts.

## Phase 2 — PPU lifter (PPC64 → IR → native)
- **Delivered**: PPU decoder/disassembler covering the integer subset
  (~90 mnemonics incl. rotates, CR ops, DS-forms, traps, SPRs); reference
  interpreter (the mandated fallback backend); block lifter to AOT-IR;
  **MiniAOT x86-64 backend** (self-contained emitter: W^X executable pages,
  inline guest-memory bounds checks, embedded syscall calls respecting SysV);
  lazy per-block cache + fallback bookkeeping; LLVM backend slot
  (compile-guarded, **untested** — no LLVM in this environment).
- **Verified by tests**:
  - `ppu_decode_ground_truth_inventory`: all 150 instructions of a GNU
    `as`-assembled inventory decode correctly (the opcode tables were derived
    from binutils objdump, not from memory).
  - `ppu_interp_alu`, `ppu_interp_branch`: real machine code (assembled from
    `tests/fixtures/*.s`) executes with correct semantics — ALU, rotates
    (sradi/rldicl/rlwinm), cmp/bgt loops, mtctr/bdnz loops.
  - `aot_hello_runs_natively`, `aot_branch_loops_run_natively`: the same
    programs execute **100% as native x86-64 code** (`ctx.icount == 0`,
    `fallback_count == 0`).
  - `aot_alu_runs_mixed_mode`: blocks with unlift-able ops (srawi, rldicl,
    etc.) fall back to the interpreter with identical results.
  - `aot_fault_on_bad_memory_access`: compiled code faulting on an unmapped
    guest store sets `ctx->error` + `error_pc` instead of corrupting memory.
- **Not working / not implemented**:
  - VMX/AltiVec (mandatory for real games), FP arithmetic (lfs/lfd/stfs/stfd
    do raw/exact conversions only), segmented addressing, lwarx/stwcx
    reservations (approximated as plain loads/stores), BL linkage and
  indirect branches in the AOT path (interpreter handles them).
  - The **LLVM backend** compiles only when `-DANYPS3_LLVM=ON` and is *not
    validated in CI* — treat as a starting point, not a finished backend.

## Phase 3 — Memory manager + LV2 HLE + hello world
- **Delivered**: guest VM (flat 512 MiB window, 64 KiB pages, page
  allocation/exec tracking, big-endian typed access); LV2 syscall layer with
  the **verified** convention (r11 = number, r3–r10 args, r3 return) and the
  verified number table (research/syscall_table.txt): getpid(1),
  _sys_process_exit(22)/exit2(26), _sys_ppu_thread_exit(41), yield(43),
  usleep(141), time(145), timebase(147), sys_memory_allocate/free/size
  (348/349/352), sys_tty_write(403), sys_fs open/read/write/close/lseek/stat
  (801–818); Emulator::Boot/Run orchestration with AOT/interpreter selection
  and stats; AOT disk cache manifests (Fnv-1a module hash).
- **Verified by tests**: `ppu_full_pipeline_hello` runs the complete
  GNU-toolchain-produced `hello.elf` end to end: ELF64 load → segment mapping
  → interpreter execution → LV2 HLE → `sys_tty_write` prints
  "Hello from Any_Ps3!\n" → `_sys_process_exit(42)` returns 42.
- **Not working / not implemented**: threads (single main thread only),
  mutexes/events, PRX/SPRX loading, SPU syscalls (Phase 4), a real /dev_hdd0
  layout, encryption of any kind.

## Phase 4 — SPU lifter + threading
- **Delivered**: SPU decoder generated from the verified 199-row ISA table
  (`spu_dispatch_table.inc`; RPCS3 fill_table semantics: (opcode, magnitude)
  rows cover index ranges, low bits holding immediate fragments). SPU
  interpreter: 128×128-bit registers as 4 little-endian lanes, 256 KiB local
  store, ~60 mnemonics (arithmetic, select, quadword loads/stores, relative +
  indirect branches, channels). Channel semantics: channel number is an
  immediate in the RA field; blocking mailboxes (SPU_WrOutMbox single-slot,
  SPU_RdInMbox 4-deep queue with condition variables), MFC put/get DMA
  (synchronous simplification), SNR registers, STOP/STOPD exit codes.
  sys_spu HLE: group_create(170), initialize(172, simplified image contract:
  raw SPU ELF in guest memory), group_start(173) on host threads,
  group_join(178), exit_status(165), write_spu_mb(190), write_snr(184),
  LS read/write(181/182). SPU ELF32 (EM_SPU=23) loader + test ELF builder.
- **Verified by tests**: decoder table spot checks; arithmetic/SELB semantics;
  mailbox write + stop codes; MFC DMA put lands in guest memory; ELF loader;
  `spu_syscall_end_to_end` — a full PPU↔SPU round trip via real syscall
  dispatch (PPU pushes 0x555 into the SPU inbound mailbox; the SPU reads it,
  adds 1, writes the outbound mailbox, stops with code 7; the PPU group_join
  and get_exit_status observe the results).
- **Not working / not implemented**: ~135 of 199 SPU mnemonics (SHUFFB,
  float ops, byte rotates, hint branches HBRA/HBRR — their split encoding is
  deliberately omitted), async MFC queues/tags, SPU interrupt routing to
  event queues (Phase 6+), raw_spu syscalls, isolated SPU, SPURS/cellSpurs.

## Phase 5 — RSX → Vulkan
- **Delivered**: backend interface; **null backend** (validates, records
  presents); guarded **Vulkan skeleton** (compile-guarded; Initialize reports
  not-ready rather than pretending); command processor consuming a clearly
  documented *synthetic test stream* (flip/draw/label); flip + stats
  bookkeeping; **content-addressed shader cache** (FNV-1a of program data,
  on-disk blobs, store/load/invalidate) — the layer a real GCM shader
  translator will plug into.
- **Verified by tests**: command processor stats/labels; shader cache round
  trip, content addressing, invalidation.
- **Not working / not implemented (major, honest)**: real GCM command-buffer
  parsing, RSX method registers, vertex/fragment program decompilation, actual
  Vulkan rendering. This phase is an architecture skeleton only.

## Phase 6 — Audio, input, cache, fallback
- **Delivered**: audio backend interface + null backend + SPSC ring mixer with
  defined underrun behavior (tested); input abstraction: 16-button guest pad
  state + per-game mapping via GameSettings ("pad_cross" etc., tested); AOT
  disk cache manager (module-hash manifests, tested store/load/clear);
  interpreter fallback fully wired into the runtime (RunAot falls back
  per-block and single-steps un-liftable regions — proven by the mixed-mode
  test in Phase 2).
- **Not working / not implemented**: real audio output backends (SDL/XAudio2/
  AAudio), touch overlay → PadMapping wiring in the Android host (UI glue
  exists in PadInput.kt), executable-page write invalidation (Vm tracks exec
  pages but no guest store hooks InvalidateRange yet).

## Phase 7 — Packaging
- **Delivered**: Windows build script (CMake + MSVC + FetchContent deps →
  Any_Ps3_Windows.zip with Any_Ps3.exe + README); Android build script (NDK +
  Gradle → Any_Ps3.apk; wrapper jar intentionally not committed, documented);
  test runner script; fixture regeneration script (any powerpc64 binutils);
  README, TEST_PLAN, LICENSE (MIT), LICENSES, this file; reference table
  dumps (verified syscall list, SPU ISA rows, binutils inventory) under
  tools/reference/.
- **Built in this environment**: the core library + full test suite on
  Linux x86-64 (both `make` and `cmake` paths), 34/34 tests passing.
- **NOT built here** (environment lacked MSVC/SDK/NDK at the time): the
  Windows .exe and the Android .apk. *(Superseded by the App Shell phase,
  which built both — see below.)*

## Phase 8 — App shell (launchable UI on both targets)
- **Delivered**:
  - **Core portability layer** (`anyps3/platform.h` + one `#ifdef`-split
    `platform.cpp`): directories, listing, W^X executable memory. The whole
    core now compiles unchanged on Linux, MinGW-w64 and the Android NDK.
  - **AOT host-ABI guard** in `Emulator::Boot`: the MiniAOT backend emits
    x86-64 **SysV-ABI** code, so Windows x64 (Win64 ABI) and Android arm64
    builds force the PPU interpreter and log it — never pretend AOT works
    where it cannot.
  - **Built-in hello demo** (`anyps3/demo/hello.h`): ships the *exact*
    GNU-binutils-assembled PPC64 ELF the automated tests validate (md5
    `21f2b89d14fa1da93dec4810efed156b`), installs it as a regular library
    entry (DEMO00001) through the normal import pipeline. "Play" on it
    genuinely boots + executes; on real games "Play" shows the honest
    boot log + "Emulation not implemented yet".
  - **Windows host** (`hosts/windows`): Dear ImGui 1.91.9b + Win32 + D3D11,
    statically linked single Any_Ps3.exe (MinGW-w64 `-static`); dark theme;
    library with real ICON0.PNG covers (stb_image), search, add-game via the
    Vista+ IFileOpenDialog (ISO ↔ folder modes), per-game settings, app
    settings, always-on log panel, first-run legal consent, `--selftest`
    headless mode + `ANYPS3_GUI_FRAMES=N` smoke-test mode.
  - **Android host fixed + finished** (`hosts/android`): Kotlin 2.0.21 Compose
    plugin fix, real cover decoding (BitmapFactory), SAF import (ISO copy +
    folder tree via DocumentFile), demo install from the bundled asset,
    per-game + global settings screens, log panel, honest "Play" banner.
  - **Gradle wrapper generated and committed** (8.9), `.github/workflows` for
    core tests + both host builds (CI fallback / reproducibility).
- **Built and verified in this environment**:
  - Windows: cross-compiled with Debian MinGW-w64 (gcc 14.2, installed via
    `apt-get download` + `dpkg-deb -x`, no root). `objdump` confirms imports
    are Windows system DLLs only (no libstdc++/libgcc/libwinpthread) → the
    exe runs on a clean install with zero prerequisites.
  - **Wine 10.0 verification (headless + GUI)**, in a clean prefix with the
    extracted Debian wine (paths patched to a user prefix; a zlib1.dll PE was
    built from source because Debian's wine omits it; lavapipe supplies the
    Vulkan ICD that wine's X11 driver requires):
    * `Any_Ps3.exe --selftest` → exit 0, **deterministic across many runs**:
      demo installed, ELF loaded (vaddr 0x10000), segments mapped, **PPU
      interpreter executed the real PPC64 code**, TTY "Hello from Any_Ps3!",
      guest exit code 42. Verified with the exact zip contents.
    * GUI under Xvfb (ANYPS3_GUI_FRAMES): window + D3D11 (wine → wined3d →
      llvmpipe) + ImGui frames verified in **5+ runs including 3 consecutive
      clean passes** and a library-flow run (after consent, the GUI created
      the library with the demo entry on disk and rendered 90 frames, exit 0).
      Caveat: in this sandbox, GUI launches are sensitive to the invocation
      context (stdio redirection / script wrapping through the agent tool can
      flip wine's console handling → CreateWindow fails with error 1400);
      direct invocations are stable. scripts/gui_smoke.sh documents the
      working recipe. This is a sandbox artifact, not an app defect — the
      same binary selftests cleanly in all contexts.
  - Android: cmdline-tools + platforms;android-35 + build-tools 35.0.0 +
    NDK 27.2.12479018 + CMake 3.22.1 installed under `tools/android-sdk`;
    `./gradlew assembleDebug` green. Verified with aapt2/apkanalyzer/
    apksigner: package com.anyps3.app, minSdk 28, targetSdk 35, **native-code
    arm64-v8a only**, lib/arm64-v8a/libanyps3.so present, assets/hello.elf
    present, debug-signed (Android Debug cert) → installs directly.
- **Not verified** (honest limits):
  - No on-device Android test: no arm64 device/emulator in this environment.
    The APK is structurally valid (aapt2/apkanalyzer/apksigner) and the JNI
    bridge compiles against the same core that passes 37/37 tests, but
    first-launch behavior on a phone is untested.
  - Wine ≠ real Windows: GPU path untested under Wine (D3D11 ran, but real
    driver behavior on Windows 10/11 is unverified). The exe should be
    smoke-tested on a real Windows machine (`--selftest` makes it a one-liner).
  - Compose adds ~20 MB dex; release builds should enable R8 later.

## Phase 9 — v0.4.0 "Encrypted Discs & PPU Core" (encrypted ISO import + PPU milestone 1 + LLVM AOT artifacts)

**TASK 1 — encrypted ISO import, fixed and verified (the user-reported Redump failure):**
- **Disc-encryption layer** (`core/src/disc.cpp`, `any_ps3/disc.h`): clean-room AES-128
  (FIPS-197; tables derived from GF(2^8) math, validated against the FIPS C.1
  vector and cross-checked byte-for-byte against OpenSSL on 40 random CBC
  buffers), region-table parser (sector-0 table, N/2-1 layout, plain/encrypted
  alternating spans, stored-end-minus-one semantics), 3k3y marker + embedded
  key detection, disc-key parsing (raw16 / 32 / 64 hex, separator- and
  0x-tolerant), sibling .dkey/.key discovery. No keys or key databases shipped
  (legal red line).
- **ISO reader rebuilt fd-first** (`fs/iso9660`): fd-backed or in-memory
  backends with sector-granular on-the-fly decryption; multi-GB images are
  never loaded into RAM and never copied. Honest open errors when filesystem
  structures sit in encrypted regions without a key.
- **Import pipeline** (`loader/import.cpp`): encrypted games import with the
  state "Encrypted - disc key needed" instead of failing; metadata degrades
  honestly PARAM.SFO -> PS3_DISC.SFB -> filename (title-id pattern scan);
  diagnostics log PARAM.SFO size/first-16-bytes/magic and the encryption-layer
  summary; per-game disc keys via settings; Android imports through
  ParcelFileDescriptor fds (no copy, persistable URI + reselect flow).
- **Robust PARAM.SFO** (`sfo/sfo.cpp`): all data formats 0x0004/0x0104/0x0204
  (UTF-8) + 0x0404 (int), unknown formats kept as raw bytes, absolute AND
  relative (old 0x0004-family) table interpretations with best-effort
  selection, lenient entry skipping, truncated-table tolerance.
- **Verified by tests** (48 -> 52 main suite + 40/40 legacy ASAN): 11 new
  disc tests + 3 PPU-semantics tests + 1 threads test, all against
  self-generated ISOs encrypted with a TEST AES key (plain/encrypted/broken
  SFO/SFB/filename fallbacks, wrong-key honesty, fd imports, FIPS+region-table
  validation).
- **Not verifiable here (honest)**: no real Redump image was downloaded
  (copyright); the layer is validated against the PS3Dec r5 reference
  semantics + our own encrypted fixtures. The user's next import log will
  confirm on real data.

**TASK 2 milestone 1 — PPU core completed (first milestone of the roadmap):**
- **Floating point**: full double + single arithmetic including FUSED
  multiply-add (std::fma), fsqrt/fres/frsqrte/fsel, frsp/fctiw/fctiwz,
  fcmpu/fcmpo (NaN -> FU), fneg/fabs/fnabs/fmr, FPSCR move/set ops, CR1
  updates. FP exception flags (FX/VX/OX) and non-default rounding modes are
  NOT modeled (documented simplification).
- **VMX/AltiVec subset** (24 mnemonics, binutils-validated): vector
  loads/stores (lvx/stvx family), splats, logic (vand..vnor), modular
  add/sub (byte/half/word), vmaddfp/vnmsubfp (vA*vC+vB lane semantics),
  vsel, vperm, word merges, vscr. ~24 of ~200 VMX ops; the rest fall back
  to honest errors.
- **Reservations**: lwarx/ldarx/stwcx./stdcx. with per-thread reservations,
  stwcx always clears, CR0 success bit, cross-thread increments observed
  under the scheduler.
- **PPU threads**: LV2 _sys_ppu_thread_create/join/detach/exit/yellow +
  cooperative round-robin scheduler (interpreter slices; AOT fast path hands
  over via a yield-out contract). Threads.s fixture: create + join with a
  lwarx/stwcx loop across the scheduler.
- **PPC64 -> LLVM IR -> native (AOT artifacts)**: the LLVM backend
  (`cpu/ppu/aot_llvm.cpp`, -DANYPS3_WITH_LLVM) lifts blocks to LLVM IR,
  emits human-readable .ll AND native object files for **x86-64 and arm64**,
  and executes blocks in-process through ORC LLJIT. Tests: hello +
  branch fixtures run 100% natively (ctx->icount == 0), unlift-able blocks
  fall back with identical results, native stores fault cleanly, artifacts
  verified (llvm-objdump symbols). LLVM 19 opaque-pointer GEP pitfall found
  + fixed (element type, not pointer type: offsets silently scaled by 8).
- **Major pre-existing bug found + fixed**: the X-form logical family
  (or/xor/and/.../extsb..cntlzd + shifts + D-form logicals) had SOURCE and
  DESTINATION swapped (PPC writes "op rA, rS, rB": dest = bits 11-15).
  Existing fixtures only used src==dest forms, so the suite never caught it;
  the new threads fixture (mr r7,r3) exposed it. Whole family corrected.
- **Honest limits**: threads run through interpreter slices (no true SMP);
  the LLVM backend is not yet linked into the shipped Android/Windows hosts
  (interpreter there; AOT execution validated on the Linux/x86-64 host);
  FP exception flags unmodeled; VMX partial.

**Verification (this build):** 52/52 host tests + 58/58 with the LLVM backend
linked (6 LLVM tests) + 40/40 legacy ASAN/UBSAN suite. Windows exe: Wine 10.0
selftest exit 0 (demo boot, real PPC64 execution, TTY Hello, guest exit 42)
+ GUI smoke under Xvfb (D3D11 via wined3d/llvmpipe, 90 frames, screenshot
shipped). Android APK: clean Gradle assembleDebug; aapt2/apksigner/apkanalyzer
verify (v0.4.0, arm64-v8a, libanyps3.so contains the new code, signature OK);
NO on-device run in this environment (no device). AES cross-checked against
OpenSSL. The 5-6 new SETTINGS are real only (disc key, log level; resolution
scale/frame limit/shader cache removed from the UI because nothing consumes
them yet).

## Capability matrix (summary)

| Capability | Status |
|---|---|
| Import folder/ISO games, library, per-game settings | ✅ tested |
| Encrypted (Redump/3k3y) ISO import + on-the-fly decrypt with user key | ✅ tested (test-key fixtures; real-image check pending user log) |
| PPU decode/interpret (integer + FP + VMX subset + reservations) | ✅ tested |
| PPU threads (create/join/yield, cooperative scheduler) | ✅ tested |
| AOT: PPC64→IR→native x64, interpreter fallback | ✅ tested (100% native execution demonstrated; Linux/x64 hosts) |
| LLVM AOT artifacts (.ll + x86-64 + arm64 objects) + ORC native exec | ✅ tested (Linux host; not yet linked into shipped hosts) |
| LV2 HLE subset, hello-world end-to-end | ✅ tested |
| SPU: decode/interpret/mailboxes/DMA/groups | ✅ tested |
| Built-in demo entry installs + executes via the real pipeline | ✅ tested (Linux CI + Wine + Windows exe) |
| Windows shell exe (static MinGW build) | ✅ built; selftest + GUI verified under Wine |
| Android shell apk (arm64-v8a, debug-signed) | ✅ built; verified with aapt2/apkanalyzer/apksigner (no on-device run yet) |
| Events/semaphores, PRX/SPRX modules | ❌ future |
| VMX full set, FP exception flags | ❌ future |
| UDF images | ❌ future |
| RSX/GCM real rendering | ❌ skeleton only |
| Audio output backends | ❌ interface only |
| LLVM AOT backend | ⚠ written, compile-guarded, untested in CI |

## Phase 10 — v0.4.1: library ID fix, encrypted-ISO UX, diagnostics (this release)

Two catch-up goals were addressed: (a) the two release sets requested by the
user (the previous release had only been delivered as the Windows zip — the
matching APK + source zip are now rebuilt from the exact same tree state),
and (b) three fixes (library UI bug, encrypted-ISO usability, diagnostics).

### VERSION numbering — what really happened
The user's task name "v0.3.0" refers to the release whose Windows ZIP they
already have. The artifacts of that release are INTERNALLY versioned **0.4.0**
("Encrypted Discs & PPU Core"); v0.3.0 was the FIRST DEMO's version ("App
Shell"), an older state. To keep the GitHub release self-consistent with the
Windows ZIP the user already has, the catch-up set is delivered as
**v0.4.0** and the new release as **v0.4.1**. Disclosed to the user, nothing
renamed silently. The version now lives in ONE `VERSION` file at the repo
root, read by: root CMakeLists (project VERSION + ANYPS3_VERSION define),
hosts/windows + hosts/desktop + Android cpp CMakeLists (same define, with
CMAKE_CONFIGURE_DEPENDS so NDK caches re-read it), the dev Makefile
(VERSION_FLAGS), and Gradle (versionName). Both UIs display the compiled-in
string; the Android in-app status line and the Gradle versionName can no
longer disagree (this exact skew was found and fixed during this release).

### TASK 2 — ImGui "2 visible items with conflicting ID!" (fixed + reproduced)
- Windows shell DrawGameRow: PushID(title_id)/PopID per row (the reported
  Play/Settings/Remove collision). Desktop host: stable game-id IDs replace
  positional ones. Android LazyColumn: items(key = { it.id }).
- Reproduced on the OLD delivered exe under Wine: with two games and the
  cursor parked on a row's buttons, the old build's frame stops completing
  right at the second row and an ImGui error tooltip appears under the
  cursor (screenshot shipped in the Windows zip as verification/
  id_conflict_reproduced_v0.4.0.png). The fixed build renders both rows,
  keeps running, exits 0 (verification/library_fixed_0.4.1.png).
- Input delivery under Wine/Xvfb was proven by clicking the "Settings"
  button (screen change captured) and by the Export-log click (below).

### TASK 3 — encrypted ISO UX ("Ready to compile" flow)
- Core: InspectGame/InspectGameFd accept an explicit disc key; an explicit
  key that does not unlock fails HONESTLY (the library entry is untouched);
  ReadDiscKeyFileAsText (hex text / colon / raw 16 bytes / whitespace
  variants); EffectiveDiscKeyText (typed hex wins over the key file);
  GameLibrary remove now also deletes settings.ini + the stored dkey.bin
  (previously a removed game LEFT ITS KEY ON DISK); ReplaceLibraryEntry
  moves settings + key when the id changes (UNKNOWN -> real TITLE_ID).
- Boot: the per-game key (typed hex, key file, or import-time sibling) is
  applied to EBOOT reads; NeedsKey refusal message now mentions the hex form.
- Windows UI: per-game settings offer the file picker AND a 32-hex paste
  field; Save triggers an in-place re-read worker ("disc key accepted —
  image unlocked: <title> [id] — Ready to compile"); rows show a
  "Ready to compile" chip. Android: same flow through
  nativeSetGameKeyFd/nativeSetGameKeyText + re-read via the persisted SAF
  grant; "Encrypted (decryption active) — Ready to compile" chip.
- Android SAF limitation (honest): sibling .dkey auto-pickup is structurally
  impossible for single-file SAF grants (Android does not expose sibling
  documents without a tree grant). Automatic pickup works on path-backed
  hosts (Windows/Linux); on Android the settings picker covers it.
- Tests (all self-generated ISOs + test key, no Sony content):
  key-file variants, effective-key precedence, full path re-key flow
  (NeedsKey -> unlock -> entry moves to the real id, settings follow),
  fd re-key, wrong-explicit-key refusal, removal-deletes-key, typed-hex
  persistence. The real-Redump path was NOT tested here (no copyrighted
  content in this environment) — the user's next import log will confirm.

### TASK 4 — diagnostics
- Log levels are real and four-way: 0 Info+ (default), 1 Warn+, 2 everything
  incl. debug (AP_DEBUG now runtime-filtered, no longer compiled out),
  3 errors-only. Unit-tested (logging_levels_really_filter etc.).
- Windows "Export log": anyps3-diagnostics-<ts>.txt next to the exe with
  version, OS (RtlGetVersion), CPU (__cpuid brand), GPU (DXGI adapters),
  Vulkan runtime (loader probed dynamically, instance extensions + physical
  devices), cache folder, per-game status (incl. AOT cache presence) and the
  last 500 log lines. Verified three ways under Wine: --diag-test
  (content + trimming checks), a real click on the button (grid-swept under
  Xvfb; report file produced), and the report shipped in the zip.
- Crash handler: SetUnhandledExceptionFilter + SIGSEGV/SIGABRT/SIGFPE
  handlers write data/crash_last.txt (version, origin, raw module-relative
  stack addresses — no symbols shipped); next start shows a "Previous
  session crashed" dialog with Export + Dismiss. --crash-test verified under
  Wine (report content shipped). A latent Begin/End imbalance in that dialog
  (End skipped when Begin returns false) was found and fixed.
- Android: AnyPs3App uncaught-exception handler writes crash_last.txt;
  MainActivity shows it with Export (share sheet via FileProvider) /
  Dismiss / Keep; app settings gain an "Export diagnostics (share sheet)"
  button (device/SoC/ABIs/OS/GLES/Vulkan-feature + per-game status + last
  500 lines). NOT verified on a real device in this environment — code is
  compiled and packaged; structural verification only (honest).

### Housekeeping / fixes found on the way
- settings.ini controller mappings NEVER loaded (compare(0,5,"pad_") can
  never match a 4-char prefix) — fixed; found by the new dkey_hex test.
- Desktop host was un-compilable (std::string InputText without
  imgui_stdlib) — fixed (include + CMake source); syntax-verified.
- Desktop settings: removed the three unwired controls (resolution scale /
  frame limit / shader cache) that violated the "no placeholder settings"
  rule; kept the real PPU-backend combo.
- Source-zip packaging is now scripted + audited (scripts/package_source.sh):
  no build outputs, no caches, no key material (the old v0.4.0 source zip
  had accidentally shipped core/build artifacts — this one is clean).

### Verification summary (what was actually run)
- 62/62 host tests green (52 before + 7 re-key + 3 logging tests).
- Windows: MinGW-w64 static build (cmake path, VERSION file read);
  --selftest exit 0 (guest 42, TTY Hello); --diag-test OK;
  --crash-test writes a real report; GUI verified under Wine + Xvfb
  (2-game library without the ID-conflict error, crash dialog rendered,
  Export-log click produced the file). NOT verified on real Windows
  hardware. Wine + llvmpipe present-path can freeze the captured screen
  mid-session (a capture artifact, not an app fault; the app exits cleanly).
- Android: full Gradle assembleDebug; aapt2 badging (versionCode 2,
  versionName 0.4.1, minSdk 28, targetSdk 35), apksigner verify, arm64-v8a
  native lib contains "0.4.1" (the NDK cmake cache was caught serving a
  stale version define and fixed with CMAKE_CONFIGURE_DEPENDS). NO
  on-device run in this environment.
- cmake 4.4.4 was used to configure/verify all three CMake projects read
  the VERSION file correctly (previously the builds were manual).

### What is still NOT working (unchanged)
Encrypted retail EBOOTs (refused, no keys), RSX/GCM graphics, audio output,
PRX/SPRX modules, SPU threading beyond the interpreter, UDF images, native
AOT on the shipped hosts. "Play" still executes the boot pipeline + the
built-in hello-world demo only. Milestone 2 (SELF loading with user keys +
cellOS PRX HLE) remains the next big step toward real games.

## Phase 11 — v0.5.0 "Real executables: module HLE bring-up pipeline"

**Goal (fixed target, multi-session):** make Minecraft: PlayStation 3 Edition
(the user's own legal dump) boot to gameplay on Windows x64 and Android arm64.
This phase builds the platform that goal needs first: the ability to load a
REAL PS3 executable, resolve its system-module imports to HLE, start it, and
produce a bring-up report precise enough to drive the next sessions. RESUME.md
(multi-session state + next-10-tasks) and CHANGELOG.md now live at the repo
root and are shipped in every source zip.

### Delivered
- **PS3 module facts in the loader** (`loader/elf.*`): ET_SCE_EXEC (0xFE00) /
  ET_SCE_PPURELEXEC (0xFFA4) acceptance; all program headers kept; TLS info
  phdr (0x60000000); sys_proc_prx_param phdr (0x60000002, magic 0x1b434cec);
  sceModuleInfo VA via the first PT_LOAD p_paddr trick (adopted for SCE
  types, or when p_paddr lands inside the first segment's file image — old
  demo ELFs with p_paddr == p_vaddr are untouched). Guest-side parsers for
  both module-info and proc-param (mapped-memory reads, so fSELF images work
  identically).
- **Import discovery** (`ReadModuleImports`): walks libstub entries (0x2C
  each) in mapped guest memory — module name, NID tables, per-function slot
  addresses (func_entry_table), attribute-0x2000 use-addresses, var/TLS
  import warnings. Format facts cross-checked across two public sources
  (psdevwiki PRX page structures; the PSL1GHT stub pattern + sprxlinker
  semantics: addrs[i] slot ← descriptor address).
- **cellOS module HLE** (`hle/modules.*`): registry of 19 modules / 9040
  public NIDs, generated by `tools/reference/gen_module_tables.py` from the
  vendored PSL1GHT MIT stub tables (tools/reference/psl1ght/) plus
  NID→name facts from a public display table (numbers/names only; no code).
  Every import resolves: known+implemented → a real C++ handler; known but
  unimplemented or unknown → an honest CELL_ENOSYS stub that logs
  `HLE MISSING module.name (nid)` once and counts calls. Real handlers:
  sysPrxForUser core (thread/mutex/prx/memory syscalls via the LV2 layer,
  guest-memory lwmutex with cooperative retry, spinlocks, strings/memory/
  printf family, bump heap, `_sys_qsort` with a REAL guest comparator via
  synchronous interpreter re-entry, exit-callback registry), cellSysutil
  params/callbacks, cellSysmodule, cellGcmSys init-level, cellPad (honest
  no-backend state), libc/libstdcxx basics.
- **LV2**: sys_prx syscalls (480-483, 494-496, verified numbers) backed by
  the HLE registry; honest ENOSYS logs now name the syscall
  (`gen_syscall_names.py` → `syscall_names.h`, 449 verified names).
- **Boot v2** (`runtime/emulator.cpp`): entry via the 8-byte OPD descriptor
  (pc + r2 = TOC; previously r2 was hardcoded 0), TLS block allocation +
  image copy + r13, argv/envp/stack-frame process-spawn layout, import
  linking with bring-up notes (slots patched, MISSING summary — called
  entries first), and real guest execution of sys_process_at_Exitspawn
  callbacks after the main thread exits.
- **Decrypted-executable support** (the encrypted-EBOOT path the goal
  needs): GameSettings.eboot_path + Windows file dialog + Android SAF
  picker with a validated app-private copy (`eboot.dec.elf`), checked
  through the same loader Play uses (a picked encrypted SELF is refused
  honestly). Legal line unchanged: no keys shipped; the user supplies their
  own decrypted file from their own dump.
- **Fixtures + tests**: `tests/fixtures/module.s` — a PS3-style module built
  by the real powerpc64 binutils assembler (OPD entry descriptor,
  sceModuleInfo + libstub + NID tables hand-crafted, the verified PowerOpen
  import-stub sequence, a real sysPrxForUser NID import plus an unknown
  NID). 4 new tests (end-to-end module boot: link → real HLE call → honest
  ENOSYS → exit 42; proc-param fallback; demo direct-entry unchanged; prx
  registry honesty). Suite: **66/66**.

### Real bugs found by the new fixtures (honesty pay-off)
1. **Interpreter hardwired `gpr[0] = 0` every step** — dropped ALL legitimate
   writes to r0. Real retail import stubs use r0 as scratch (mflr r0 / lwz
   r0 / mtctr r0), so no real EBOOT could ever have booted. Fixed: r0 is a
   real register; rA=0 addressing still means zero base (ra_val), and X-form
   rB is now always a real register index (the old `rb==0 → 0` substitution
   was wrong once r0 became real). The AOT backends never had the hardwire —
   the interpreter was the inconsistent one.
2. **Trampoline `lis` mis-encoded as `addi`** (0x396… instead of 0x3D6…) —
   the HLE dispatch number arrived at LV2 as 4097 instead of 0x10000001.
   Caught by the module fixture; verified against binutils output.
3. **ParsePs3ModuleInfo double-offset** (guest_mem already contained the VA).
4. ppu-fixtures generator: toolchain discovery fixed for the in-repo tools
   dir (LD_LIBRARY_PATH), sources read from tests/fixtures/, and archived
   fixture bytes (fp/vmx/sync/threads, sources lost with the old research/
   dir) are carried over from the previous header when their .s is absent —
   clearly marked in the generated header.

### Verification (this build)
- 66/66 host tests (62 + 4 module tests).
- Windows: MinGW-w64 static build (PE32+ x64); Wine 10: `--selftest` exit 0
  (demo ELF → PPU execution → TTY Hello → guest exit 42 through the NEW
  boot path), `--diag-test` OK (524-line report), `--crash-test` writes
  "Any_Ps3 0.5.0 crash report", GUI 100 frames under Xvfb + clean exit.
  Screenshots were not captured this round (the sandbox's llvmpipe capture
  path is flaky); logs + reports are shipped in the zip instead.
- Android: full Gradle assembleDebug; aapt2 badging (versionCode 3,
  versionName 0.5.0, arm64-v8a, targetSdk 35), apksigner verify OK,
  libanyps3.so contains the 0.5.0 string and the module-HLE code (73
  sysPrxForUser/HLE-MISSING strings). NO on-device run (no device).
- Source zip: audited by package_source.sh (no build outputs, no keys, no
  local config; 630 files) and now includes RESUME.md + CHANGELOG.md.

### Not working yet (unchanged + new)
Rendering (cellGcm command processing, RSX→Vulkan), pad/audio backends for
games, SPU beyond ~60/199 mnemonics, SPURS, per-thread TLS copy, libstub
TLS-variable imports, AOT linked into shipped hosts, true SMP threads. The
cellFs SPRX NID set is not in the vendored public tables yet (games' file IO
imports will surface in the first bring-up log; the LV2 fs syscalls behind
them are already implemented). Minecraft itself cannot boot this release:
its EBOOT is encrypted (the user must attach their own decrypted executable
once), and rendering does not exist yet — the next milestones in RESUME.md.

## Phase 12 — v0.6.0 "Android app overhaul: working log download, readable UI, real icon" (session 2, 2026-10-25)

Scope: direct response to the user's Android-13 bug report ("app crashes when
pressing export logs"; unreadable text "no black on black"; "a real app
icon"; clean design; more features). The emulation core is UNCHANGED from
v0.5.0 — this release is the Android host only.

What was done (all inside the shipped APK):

1. Log export rebuilt crash-proof. Root-cause analysis: the v0.5.0 dex was
   disassembled and DOES contain the try/catch(Exception) version, so an
   Error-class failure (UnsatisfiedLinkError/NoClassDefFoundError/OOM) or a
   device-specific share-sheet failure were the remaining candidates; the
   exact exception could not be reproduced in this sandbox (no device).
   Fix (architectural): every export entry now catches Throwable, and the
   PRIMARY path writes a real file via MediaStore into Download/Any_Ps3/
   (Android 10+, no permissions, NO share sheet involved). API-28 fallback:
   app-documents dir. Share sheet kept as a secondary path.
2. In-app log viewer (new screen): level filter, search, selectable text,
   copy-all, save, share, auto-scroll. Logs are readable without any export.
3. In-app crash-report screen (replaces the old platform dialog): full stack
   trace, selectable, Copy / Save to Downloads / Share / Delete; closing
   keeps the report for the next start. This is the instrument that will
   capture any remaining crash verbatim.
4. Readability: explicit high-contrast dark palette (background #0B0E13,
   onBackground #EFF3FA ≈ 15:1, secondary #BAC6DA ≥ 4.5:1 — every text has
   an explicit checked foreground color), minimum font 12sp, log viewer
   12sp monospace @ 17sp line height (was 10sp before).
5. Design: game cards with status chips (ready/encrypted-key-needed/
   decryption-active), one primary action per card (Fitts + Von Restorff),
   extended Import FAB, dismissible feedback banners for every action,
   remove-confirmation dialog, empty state with brand logo, log peek bar.
6. Real adaptive launcher icon: gold "3" (brand motif) with glow over a
   night-blue gradient + the brand's mountain silhouette; monochrome layer
   (Android 13 themed icons); legacy circle PNGs all densities; 512px store
   icon. Generated by scripts/gen_android_icons.py (PIL + numpy only).
7. Latent bugs fixed: SettingsScreen wrote snapshot state during composition
   (illegal in Compose; now a local mutableStateMapOf saved on Save); UI
   state lost on rotation (AppUiState moved into the Application); manifest
   configChanges extended.
8. New real features: storage stats (library vs cache), delete-old-log-files
   action, honest About panel (version + engine status + works/in-development
   list), timestamped log header.

Verification (honest): Gradle assembleDebug green; aapt2 badging 0.6.0 /
versionCode 4 / minSdk 28 / targetSdk 35 / arm64-v8a; apksigner verify OK;
the new classes (Diagnostics, LogViewerScreen, CrashReportScreen,
saveToDownloads/MediaStore) confirmed inside the shipped dex; icon contrast
reviewed via a vision model (8/10 on a dark wallpaper). NOT verified: any
on-device behavior (no Android hardware in this sandbox) — the user's install
is the test, and any remaining crash now produces a capturable in-app report.

Known limits: the Android-13 crash fix is architecture-based (no device
reproduction was possible); MediaStore path requires Android 10+ (user has
13; API 28 gets the fallback dir + share); themed-icon monochrome quality
depends on launcher tinting.
