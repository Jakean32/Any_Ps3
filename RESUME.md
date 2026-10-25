# Any_Ps3 — RESUME.md (multi-session bring-up state)

> **Session protocol:** this file is the complete cross-session state. A new
> session starts from here — read this file top to bottom, then continue with
> "Next tasks". Also read `CHANGELOG.md` for the one-line history and
> `worklog.md` (project-level) for past-phase detail. Goal, rules and honest
> limits are recorded here so no other context is needed.

## THE GOAL (fixed target)

Make **Minecraft: PlayStation 3 Edition** (user's own legal dump, already
imported in the app as an ISO) **boot to gameplay on Windows x64 and Android
arm64**. Everything serves this goal first. Generic code only — no per-game
hacks that hide real bugs. Legal red lines: never bundle/download Sony
firmware, keys, or game content. Honesty rules: never fake progress/success;
say exactly what was built, what was tested and what was not.

Multi-session mode: 5+ sessions expected. Never declare the goal reached
without evidence: a log from the user's device showing the game running past
the title screen (frames rendered, input handled). The user sends bring-up
logs when the game hits unimplemented things; fix entries in
frequency/importance order, then continue the milestones.

## Current state (v0.6.0, session 2)

**Session 2 (2026-10-25) was an Android app-fix session** (user bug report:
log-export crash on Android 13, unreadable text, missing icon/design). The
emulation core is UNCHANGED from v0.5.0 (66/66 tests still describe it).

**Android app v0.6.0 (this session, all in the shipped APK):**

- Export crash FIXED: every export path catches `Throwable`; PRIMARY path is
  MediaStore `Download/Any_Ps3/` (no share sheet, no permissions, Android
  10+; API 28 fallback = app-documents dir). Share = secondary.
- New: in-app log viewer (level filter, search, select/copy, save, share,
  auto-scroll) + in-app crash-report screen (Copy/Save/Share/Delete).
- New: high-contrast dark theme (explicit colors everywhere, min 12sp fonts,
  12sp mono log @17sp line height — old log was 10sp), game cards + status
  chips, FAB, feedback banners, remove-confirm, empty state, rotation-safe
  app-scoped state (AppUiState in Application).
- New: real adaptive launcher icon (gold "3" + brand mountain silhouette,
  night-blue gradient; monochrome layer for Android-13 themed icons; legacy
  PNGs all densities; `scripts/gen_android_icons.py`).
- Fixed latent Compose bug: SettingsScreen wrote snapshot state during
  composition (`pad = pad + …` in the render loop) → now a local
  mutableStateMapOf, saved on Save.
- Verified: aapt2 0.6.0/versionCode 4/arm64-v8a, apksigner OK, new classes
  (Diagnostics/LogViewerScreen/CrashReportScreen/MediaStore) in the dex.
  NOT verified: on-device run (no device here). If the user still crashes,
  the new crash screen captures the exact stack trace — ask for it.

**What works (emulation core, unchanged from v0.5.0, 66/66 host tests green):**

- Game import: folder dumps, ISO9660, encrypted (Redump/3k3y) ISOs with the
  user's .dkey; library, covers, per-game settings (disc key, decrypted
  executable, controller mapping, PPU backend, log level).
- PPU: extensive interpreter (integer/FP/VMX-subset/reservations; **gpr[0]
  is now a real register — the old blanket hardwire was a genuine retail-boot
  killer, found by the new module fixture**), cooperative threads,
  MiniAOT x64 (Linux host), LLVM AOT backend (Linux dev host).
- **PS3 module pipeline (this session's milestone): ET_SCE_EXEC/ET_SCE_PPURELEXEC
  loading, OPD entry descriptors (pc + r2=TOC), sceModuleInfo (first PT_LOAD
  p_paddr trick) + sys_proc_prx_param (phdr 0x60000002) import discovery,
  libstub walk (0x2C entries), NID resolution against the built-in cellOS
  module HLE, guest trampolines (lis r11,0x1000/ori/sc → Lv2 HLE range
  dispatch), descriptor slot patching (LV2 module-manager contract), TLS
  block (r13), argv/envp/stack setup, exit callbacks (real guest execution).**
- cellOS HLE: 19 modules / 9040 known NIDs from public tables (PSL1GHT MIT +
  verified NID-name facts; see `tools/reference/gen_module_tables.py`).
  REAL implementations for: sysPrxForUser core (threads, lwmutex with
  cooperative retry, spinlocks, prx registry, memory, strings, printf family,
  heap, qsort with a real guest comparator), cellSysutil params/callbacks,
  cellSysmodule, cellGcmSys init-level, cellPad (honest no-backend state),
  libc/libstdcxx basics. Everything else resolves to an HONEST ENOSYS stub
  that logs `HLE MISSING module.name (nid)` once — the bring-up report.
- LV2: verified syscall numbers with names in honest ENOSYS logs; sys_prx
  load/start/stop/unload/id-by-name (HLE registry-backed).
- SPU: decoder (199-op table), interpreter (~60 mnemonics), mailboxes, MFC
  DMA, ELF32 loader, thread groups. RSX: null backend + shader cache
  framework only.
- Hosts: Windows static ImGui exe (selftest/GUI verified under Wine),
  Android Compose APK (arm64-v8a; structural verification only).

**What is broken / missing (honest):**

0. **User-reported Android crash on "export logs" is believed fixed in
   v0.6.0 but NOT confirmed on the user's device yet** — get the user to
   test "Save log report to Downloads" and report (or send the crash
   screen if anything still dies).
1. **The user's Minecraft ISO still cannot boot its own executable**: the
   disc's EBOOT.BIN is an encrypted retail SELF. Any_Ps3 ships no keys.
   NEW this session: per-game "decrypted executable" picker (Windows file
   dialog, Android SAF + validated app-private copy) — the user must supply
   their own decrypted ELF/fSELF once, then Play boots it.
2. Rendering: cellGcm command processing + RSX→Vulkan = not implemented
   (null backend only). No frames can be produced yet.
3. Input: no host pad backend is wired into games yet (cellPad honestly
   reports "no pads connected").
4. Audio: no output backend for games.
5. SPU: ~140/199 mnemonics missing; SPURS not implemented (Minecraft uses it).
6. Threads: cooperative slices only; per-thread TLS not copied on create.
7. TLS variables imported via libstub (ntls) are parsed but not resolved.
8. AOT backends not linked into shipped Windows/Android hosts (interpreter).
9. Android: no on-device run yet (structural verification only).
10. No real-Redump-ISO test possible in this sandbox (self-generated
    encrypted fixtures only).

## Exact next tasks (priority order)

1. **Android v0.6.0 user test (needs the user):** install
   Any_Ps3-0.6.0-arm64.apk → Settings → "Save log report to Downloads" →
   confirm the file appears in Download/Any_Ps3/ and NO crash. If anything
   still crashes, the in-app crash screen shows the exact stack trace —
   ask for it (Save/Share) and fix surgically. Also check: new icon, text
   readability, rotation, import sheet, log viewer.
2. **User test of the execution pipeline (needs the user):** user attaches
   their decrypted EBOOT (their own tools, their own dump) in per-game
   settings → Play → send the bring-up log. The log shows: linked imports,
   `MISSING` list (called ones first), first unimplemented syscall with its
   NAME, and how far PPU execution got. Fix entries in frequency order.
3. **cellFs NIDs:** games import file IO from the "cellFs" SPRX whose export
   NIDs are NOT yet in the public tables I vendored (PSL1GHT has no libfs
   stub). Fetch the cellFs NID list (public NID databases) and implement the
   calls on top of the existing LV2 sys_fs layer (801-818 are done).
4. **Per-thread TLS copy on _sys_ppu_thread_create** + libstub TLS-variable
   resolution (errno etc. live there; likely needed before cellGcm works).
5. **sys_lwcond + sys_event queues** (games block on events almost
   immediately; flip/vsync + audio all use event queues).
6. **cellGcm real work:** gcmGetConfiguration/MapMainMemory/control-buffer
   setup; then the RSX command processor over real GCM put/get pointers and
   the first cleared frame through the Vulkan backend skeleton.
7. **SPU expansion** (~140 mnemonics: SHUFFB, float ops, rotates) + async MFC
   + SPURS HLE — Minecraft's chunk/lighting jobs run on SPUs via SPURS.
8. **Host pad backends** (Windows raw input/XInput; Android gamepad) wired to
   the input module → ioPadGetData returns real data.
9. **Audio ring → host backends** (WASAPI/XAudio2, AAudio/Oboe).
10. **Wire the LLVM AOT backend into the shipped hosts** (arm64 execution on
    Android; x64 on Windows) — needed for real-game performance.

## Known bugs / sharp edges

- Interpreter `ea_x` now uses r[rb] always (rB is a real index; the old code
  zero-substituted rb==0 — harmless while gpr0 was hardwired, wrong now).
- `settings.ini` keeps `eboot_path` on disk; removing a game deletes it
  (GameLibrary::Remove deletes the whole games/<id>/ dir — verify it covers
  eboot.dec.elf too).
- ModuleHost trampoline area (256 KiB @ 0x10000000) supports ~10k imports;
  exhausted → honest resolve failure (logged).
- The guest heap for `_sys_malloc` is a bump allocator (free is a no-op) at
  0x10400000, 8 MiB; callback stack at 0x10C00000; TLS at 0x10D00000.
  sys_memory_allocate goes top-down from 0x1FFFF000; thread stacks at
  0x1F000000 down. Do not move these without updating threads.cpp/modules.cpp.
- Wine + llvmpipe GUI captures can freeze mid-session (capture artifact).
- ppu fixtures: fp/vmx/sync/threads sources are archived in releases; their
  bytes are carried over by `tools/reference/gen_fixtures.py` when the .s is
  absent. module.s IS in-repo (regenerate with
  `python3 tools/reference/gen_fixtures.py`; needs
  `tools/ppc64tools/usr/bin/powerpc64-linux-gnu-as`, installed via
  `apt-get download binutils-powerpc64-linux-gnu && dpkg-deb -x … /home/z/my-project/tools/ppc64tools`).

## Build & verify commands (this environment)

```bash
cd /home/z/my-project/Any_Ps3
# core + tests (66):
make -j$(nproc) test            # or scripts/run_tests.sh
# regenerate NID/syscall tables (after editing tools/reference/psl1ght/*):
python3 tools/reference/gen_module_tables.py
python3 tools/reference/gen_syscall_names.py
# regenerate PPC64 fixtures (needs tools/ppc64tools):
python3 tools/reference/gen_fixtures.py
# Windows (MinGW cross; exe → zip):
bash scripts/build_windows_mingw.sh
# Windows verification under Wine (see worklog for the wine prefix recipe —
# paths under /tmp/my-project/tools; `wine FinalAny_Ps3.exe` style scripts
# in scripts/):
bash scripts/wine_final_verify.sh    # selftest + diag/crast tests + GUI smoke
# Android (needs /tmp/my-project/tools: SDK/NDK/gradle/JDK):
bash scripts/build_android.sh        # APK → download/
bash scripts/build_android_env.sh assembleDebug  # sandbox env (SDK/NDK/JDK/gradle paths)
# app icon regeneration (PIL + numpy):
python3 scripts/gen_android_icons.py
# source zip (audited: no build outputs, no keys):
bash scripts/package_source.sh
```

Toolchain survival: /tmp/my-project/tools has NDK 27.2, SDK, gradle 8.11.1,
JDK 21, mingw debs, wine 10, imgui, llvm. **/tmp can be wiped by sandbox
resets** — if gone: re-download (no root needed: apt-get download +
dpkg-deb -x). The project tree itself lives in /home/z/my-project/Any_Ps3.
Deliverables ALWAYS go to /home/z/my-project/download/.

## Release state

- v0.6.0 (this session): `Any_Ps3-0.6.0-arm64.apk` (versionCode 4),
  `Any_Ps3-0.6.0-source.zip`, `Any_Ps3-icon-512.png`,
  `SHA256SUMS-0.6.0.txt` (see download/). Android-focused release;
  Windows host functionally unchanged since v0.5.0.
- v0.5.0 (previous): `Any_Ps3-0.5.0-Windows-x64.zip` (bab8244a — still the
  current Windows build), `Any_Ps3-0.5.0-arm64.apk` (8e07ddf0),
  `Any_Ps3-0.5.0-source.zip` (896cddc9), `SHA256SUMS-0.5.0.txt`.
  versionCode 3.
- Old releases v0.4.0/v0.4.1 remain (ID-conflict fix, disc-key UX,
  diagnostics, encrypted ISO support).
- Version lives in ONE `VERSION` file (CMake/Gradle/Makefile/both UIs read
  it; bump versionCode in hosts/android/app/build.gradle.kts by hand each
  release — currently 4).

## Honesty ledger (what was NOT verified)

- Windows: Wine 10 only, never real hardware. Android: no on-device run.
- The module-HLE pipeline is verified with self-made PS3-style fixtures
  (real binutils PPC64 code); a REAL retail EBOOT has never been loaded here
  (no copyrighted content in this sandbox). The user's first real-EBOOT
  bring-up log is the actual test.
- libc/libstdcxx NID sets come from a public NID→name table (rpcs3 display
  table, facts only) — the module names real games use for them ("libc" etc.)
  are best-effort aliases; the first bring-up log will confirm or correct.
- sysModuleLoad accepts unknown CELL_SYSMODULE ids with an OK + explicit log
  line (documented; ids not yet mapped to names).
