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
- **NOT built here** (environment lacks MSVC/SDK/NDK): the Windows .exe and
  the Android .apk. Their sources + build scripts are complete but have not
  been compiled in this environment — treat first builds as potentially
  needing mechanical fixes, and report results.

## Capability matrix (summary)

| Capability | Status |
|---|---|
| Import folder/ISO games, library, per-game settings | ✅ tested |
| PPU decode/interpret (integer subset, binutils-validated) | ✅ tested |
| AOT: PPC64→IR→native x64, interpreter fallback | ✅ tested (100% native execution demonstrated) |
| LV2 HLE subset, hello-world end-to-end | ✅ tested |
| SPU: decode/interpret/mailboxes/DMA/groups | ✅ tested |
| PPU threads, events, PRX/SPRX modules | ❌ future |
| VMX/AltiVec, FP arithmetic | ❌ future |
| UDF images | ❌ future |
| RSX/GCM real rendering | ❌ skeleton only |
| Audio output backends | ❌ interface only |
| LLVM AOT backend | ⚠ written, compile-guarded, untested in CI |
| Windows/Android host binaries | ⚠ sources complete, not built in this environment |
