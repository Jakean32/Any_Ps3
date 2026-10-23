# Any_Ps3 — Phase Reports

## Phase history

### Phase 1 — Project bootstrap (earlier sessions)
- Defined the AOT-recompilation architecture (PPU/SPU → LLVM IR → native; LV2/cellOS HLE; RSX → modern graphics API; fallback interpreter).
- Legal red lines set: no Sony firmware/keys/game content; honesty rules (no fake progress, no fake success).
- Core library brought up on host with a 34-test suite (import pipeline incl. a hello-world compile flow) and a demo APK that installed and opened on Android.

### Phase 2 — Sandbox reset & rebuild (this session, 2026-10-23)
The sandbox was reset before this phase could start; the entire project tree was lost (only the uploaded logo files survived). Everything was rebuilt from scratch, this time with build scripts persisted for reproducibility.

**Toolchains (all in `tools/`, no root required):**
- Adoptium JDK 21, Android cmdline-tools → build-tools 35.0.0 + platforms;android-34 + NDK 27.2 (arm64 clang)
- mingw-w64 14.2 cross toolchain via `apt-get download` + `dpkg -x` (incl. windres)
- Dear ImGui (Win32 + D3D11 backends), stb headers, genisoimage
- Wine 10.0 (user-local extraction: patched wineserver nls paths, fixed wrapper scripts, added zlib1.dll) + Xvfb for GUI verification

**Asset pipeline (`scripts/gen_assets.py`):** logo selected (stone "3" + mountain temple); palette extracted (accent `#AB874D`, sky `#628BB4`); generated Android adaptive-icon foreground (all densities) + monochrome layer + legacy circle launchers + in-app drawables; Windows multi-frame `.ico` (7 sizes). A first `.ico` attempt embedded only one frame (Pillow `append_images` pitfall) — fixed with the `sizes=` API and re-verified with pefile.

**Core library (`core/`, portable C++17, zero deps):** util/SHA-256, byte sources (File/Fd/Mem), mini-JSON, PARAM.SFO parser, ISO9660 reader, stb thumbnailer, no-copy import + library.json, per-game cache manifests, PPU ELF64BE loader + PowerPC scanner, honest compile pipeline, LV2 syscall framework, Library facade + config.

**Notable bugs found & fixed during bring-up:** ISO9660 "." / ".." stored as bytes 0x00/0x01 (infinite directory recursion); directory records vs sector boundaries; ELF header packing bug in the fixture generator; `printf("%s", std::string)` UB; `list_dir` not clearing its output; same-millisecond compile-log collisions; javac lambda compilation against android.jar needing `core-lambda-stubs.jar`; build-tools 34 d8 crashing under JDK 21 (switched to build-tools 35); d8 rejecting loose .class files (feed a jar); Windows text-mode fds needing `O_BINARY`; Pillow ICO single-frame pitfall; Wine loader/wineserver hardcoded paths (user-local fixes documented above).

**Tests:** rebuilt suite — **40/40 passing on host g++**, and the same suite cross-compiled for Windows runs **40/40 under Wine 10.0**. Includes the three explicitly required tests: SFO parsing, ISO read via raw file descriptor, cache-manifest round-trip. (The original 34-test suite was lost in the reset; coverage was rebuilt to equivalent-plus and is documented in README.)

### Phase 3 — Real app shell (this session)
Replaced the demo UI on both platforms with the real shell, using the user's logo everywhere:

- **Android (`android/`, Java + framework Views, Material-3-style dark theme):**
  - Splash with logo fade/scale animation; Android 12+ system splash uses the adaptive icon.
  - First-run wizard (once; re-runnable from Settings): welcome → cache folder via `ACTION_OPEN_DOCUMENT_TREE` with *persistable* read/write grant, free-space display and low-space warning → legal notice (accept-to-finish). Settings persist in `config.json` via the core.
  - No-copy import: multi-select ISOs via `ACTION_OPEN_DOCUMENT` (persistable URIs), folder scans (raw-path fast scan; SAF `DocumentsContract` walker fallback), all reads through `ParcelFileDescriptor` → fd → native random access. Missing files are flagged and survive reboots ("File missing — reselect").
  - Library grid with covers, status chips (Not compiled / Compiling / Ready / Out of date / Failed / File missing), per-game detail sheet with the honest stage list (✓ ok, — not implemented yet, ! blocked, ✕ failed) and Compile / Recompile / Delete cache / Remove; compile progress dialog with real percentages.
  - Settings (cache folder, free space, All-files-access state + grant, wizard re-run, legal, exec-strategy note) and Logs pages, bottom navigation.
  - *Honest deviation:* Jetpack Compose was not feasible in this sandbox (no Gradle/Maven assembly; assembling Compose AARs by hand is not reliable), so the shell is built with the framework's own UI toolkit in the same visual language (dark M3-style theme, accent from the logo). The UI is fully functional and the code is plain Java 8 for maximum compatibility.
- **Windows (`windows/`, Dear ImGui + Win32 + D3D11, same palette):**
  - Animated splash (logo fade + scale + settle), same wizard flow with `IFileDialog` folder picker and free-space check, library grid with cover textures, detail popup with stage states, settings, logs, compile progress modal — all backed by the *identical* core.
  - `Any_Ps3.exe`: PE32+ x64, statically linked, embedded 7-frame icon (window + shell icon), falls back to WARP if no GPU.

### Verification (this session, honest)
- **APK:** `aapt dump badging`, `apksigner verify`, `dexdump`, `apkanalyzer` — all good (details in README). Not installed on a device in the sandbox.
- **Windows core:** 40/40 tests under Wine 10.0.
- **Windows GUI:** ran under Wine + Xvfb; window created, D3D11 initialized, shell rendered (screenshot in the zip). Not run on physical Windows hardware in the sandbox.
- Artifacts packaged: `Any_Ps3.apk`, `Any_Ps3_Windows.zip`, and — new policy — `Any_Ps3_source.zip` ships with **every** release.

### Next steps (proposed)
1. LLVM IR lifting for the PPU analysis output (the pipeline stage after `analyze`).
2. Native codegen for one honest "hello world" end-to-end compile (analysis → IR → x64/ARM64 → artifact → loader in the private dir).
3. Extend LV2 HLE (process/memory/filesystem) and start cellOS module framework.
4. RSX: capture-command translation skeleton.
5. On-device test pass (install the APK on real hardware, exercise wizard/import/compile on a real owned game).
