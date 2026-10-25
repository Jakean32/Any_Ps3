# Any_Ps3 — Test Plan

> v0.4.0 additions: disc-encryption tests (AES FIPS/OpenSSL vectors, region
> table, key parsing, encrypted-ISO import states, fallback chains), PPU
> FP/VMX/reservation semantic tests, PPU threads test, and the LLVM AOT
> tests (native ORC execution + .ll/x64/arm64 artifact emission) run with
> -DANYPS3_WITH_LLVM. Totals: 52 host tests (58 with LLVM) + 40 legacy-suite
> tests under ASAN/UBSAN.

## Philosophy

Any_Ps3 follows a "do not fake functionality" rule: every capability claim in
the README maps to an automated test, and every untested area is explicitly
listed. PPC64 fixtures are not hand-assembled guesses — they are produced by
the real GNU binutils powerpc64 assembler and embedded as byte arrays
(`tests/fixtures/ppu_fixtures.h`, sources in `tests/fixtures/*.s`), so decoder
and interpreter bugs surface immediately.

## Running

```bash
make test                     # Linux dev host (g++ or clang++)
cmake -S . -B build && cmake --build build -j && ctest --test-dir build
```
Each test prints `[ RUN ]`/`[ OK ]`/`[FAIL]`; the binary exits non-zero on any
failure. Suites: `./build/anyps3_tests <filter>` (e.g. `spu`, `aot`, `iso`).

## Current suites (34 tests)

### Formats & import (Phase 1)
| Test | Verifies |
|---|---|
| `sfo_roundtrip`, `sfo_handmade_layout`, `sfo_rejects_garbage` | PARAM.SFO parse/build, byte layout |
| `iso_roundtrip`, `iso_rejects_non_iso` | ISO9660 reader + minimal writer round trip |
| `elf_loads_builder_output`, `elf_rejects_*` | ELF64 loader, endian/class checks |
| `self_loads_plaintext_fself`, `self_rejects_encrypted` | SELF container; encrypted retail refused |
| `import_iso`, `import_folder`, `import_rejects_bad` | import pipeline + library index |

### PPU decoder & interpreter (Phase 2)
| Test | Verifies |
|---|---|
| `ppu_decode_ground_truth_inventory` | all 150 GNU-assembled instructions decode to the expected mnemonic (BO/AA/LK checked for branches) |
| `ppu_interp_alu` | ALU/rotate semantics on real machine code (addi, mullw, srawi, srwi, slwi, sradi, rldicl, subf) |
| `ppu_interp_branch` | cmpwi/bgt loop + mtctr/bdnz loop on real machine code |
| `ppu_full_pipeline_hello` | complete GNU-produced ELF → loader → VM → interpreter → LV2 → `sys_tty_write` prints, `_sys_process_exit(42)` returns 42 |

### AOT pipeline (Phase 2)
| Test | Verifies |
|---|---|
| `aot_lifts_and_compiles_hello_block` | PPC64 → IR → x86-64 compilation |
| `aot_hello_runs_natively` | 100% native execution: `ctx.icount == 0`, zero fallbacks |
| `aot_branch_loops_run_natively` | native conditional + CTR loops with correct results |
| `aot_alu_runs_mixed_mode` | un-liftable ops fall back to interpreter, identical results |
| `aot_fault_on_bad_memory_access` | native store to unmapped guest memory faults cleanly |

### SPU (Phase 4)
| Test | Verifies |
|---|---|
| `spu_decode_table` | IL/ILA/A/SELB/STQD/BR/WRCH/NOP/STOP decodings |
| `spu_arithmetic_and_selb` | per-lane arithmetic + byte select |
| `spu_out_mbox_and_stop` | SPU_WrOutMbox + stop codes |
| `spu_mfc_dma_put` | MFC put channel sequence → guest memory contents |
| `spu_elf_loader_and_run` | SPU ELF32 (EM_SPU) load + run |
| `spu_syscall_end_to_end` | PPU: group_create → initialize(ELF) → start → write_spu_mb → SPU: rdch/ai/wrch/stop → PPU: join + exit status |

### RSX / audio / input / cache (Phases 5–6)
| Test | Verifies |
|---|---|
| `rsx_null_backend_and_test_stream` | command processor + flip/draw/label bookkeeping |
| `shader_cache_roundtrip` | content-addressed store/load/invalidate/count |
| `audio_ring_basic` | ring write/read + underrun silence |
| `input_mapping` | per-game controller mapping → pad state |
| `aot_disk_cache_manifest` | module-hash manifests: store/load/clear + hash change |

## Future tests (tracked work)

- PPU threads (create/join), event flags, lwarx/stwcx reservations.
- GCM command buffer parsing with real RSX captures.
- Shader decompilation golden tests (RSX program → GLSL).
- LLVM backend: the same AOT fixtures through `-DANYPS3_LLVM=ON`.
- Android: instrumented test that boots the synthetic hello world on device.
- Windows: CI runner executing `make`-equivalent via MSVC + the test suite.

## Manual test matrix (when host UIs are built)

| Host | Steps | Expected |
|---|---|---|
| Windows | run Any_Ps3.exe → first-run consent → add a folder dump → run | boot log appears; hello-world homebrew exits 42 |
| Android | install APK → consent → SAF-pick ISO → import → run | library entry; boot log in runtime panel |

## v0.4.1 additions (this release)

| Test | Where | Status |
|---|---|---|
| rekey_key_file_text_variants (hex text, newline-padded, colon form, raw 16 bytes, garbage, missing) | tests/test_rekey.cpp | ✅ |
| rekey_effective_key_prefers_typed_hex | tests/test_rekey.cpp | ✅ |
| rekey_path_import_then_unlock (NeedsKey import -> key attached -> in-place re-read -> entry moves UNKNOWN->real id, settings + key follow) | tests/test_rekey.cpp | ✅ |
| rekey_fd_locked_then_unlocked_with_key (Android fd path) | tests/test_rekey.cpp | ✅ |
| rekey_wrong_explicit_key_refuses_and_keeps_entry (honest failure, library untouched) | tests/test_rekey.cpp | ✅ |
| rekey_remove_game_deletes_settings_and_key (no key leak) | tests/test_rekey.cpp | ✅ |
| rekey_dkey_hex_persists_in_settings_ini (+ caught the pad_ prefix-loading bug) | tests/test_rekey.cpp | ✅ |
| logging_levels_really_filter (4 levels genuinely change emitted lines) | tests/test_logging.cpp | ✅ |
| logging_level_clamps / logging_level_persists_in_config | tests/test_logging.cpp | ✅ |
| Windows --selftest under Wine (exit 0, guest 42) | scripts/wine-run.sh | ✅ |
| Windows --diag-test under Wine (report + last-500 trimming + sections) | scripts/wine_final_verify.sh | ✅ |
| Windows --crash-test under Wine (crash_last.txt content) | scripts/wine_final_verify.sh | ✅ |
| ID-conflict reproduction: old exe vs fixed exe, 2 games, parked cursor | scripts/wine_gui_verify.sh | ✅ (screenshots in the zip) |
| Export-log click under Xvfb (real mouse → diagnostics file) | scripts/wine_export_click_test.sh | ✅ (grid-swept click) |
| APK badging/signature/native-version (aapt2, apksigner, strings on libanyps3.so) | scripts/build_android_env.sh | ✅ |
| Android crash handler / share-sheet export on a real device | manual | ❌ not run here (no device) |

Not yet covered (unchanged): real Redump ISO import (needs the user's log),
on-device Android flows, real-Windows-hardware GUI runs.

## v0.5.0 additions — PS3 module pipeline (session 1 of the Minecraft bring-up)

Fixture: `tests/fixtures/module.s` — a PS3-style module assembled by the real
powerpc64 binutils (OPD entry descriptor, sceModuleInfo + libstub + NID
tables hand-crafted in .data, the verified PowerOpen import-stub sequence).
Regenerate with `python3 tools/reference/gen_fixtures.py` (needs
`tools/ppc64tools`, installed via apt-get download binutils-powerpc64-linux-gnu
+ dpkg-deb -x — no root).

- module_ps3_style_boot_end_to_end: build the ET_SCE_EXEC image with the
  Elf64Builder PS3 facts → load → map → OPD entry parse (pc + r2=TOC) →
  import walk (exactly 2 imports; correct NIDs + slots) → link (both slots
  patched with descriptor addresses) → PPU interpreter runs the REAL guest
  stub sequence: mflr/std/stdu/lwz/lwz/mtctr/bctrl into the trampoline →
  sc → the real sysPrxForUser `_sys_strlen` handler returns 2 → the unknown
  NID returns CELL_ENOSYS (0x80010002) → exit code 42. Bring-up bookkeeping
  verified: known import marked implemented, unknown import recorded with
  call_count 1 and name nid_deadbeef, MISSING summary present.
- module_proc_param_fallback_imports: same imports discovered through the
  sys_proc_prx_param block (phdr 0x60000002) when sceModuleInfo's libstub
  window is zeroed.
- module_direct_entry_demo_unchanged: plain ET_EXEC demo images keep direct
  entry (no OPD interpretation) and still run to exit 42 + TTY Hello —
  the whole pre-existing suite remains on this path.
- module_prx_registry_and_honest_refusal: sys_prx registry id lifecycle
  (load/start/stop/unload; id-by-name; gone after unload) and honest
  refusal of unknown modules.

Known bug fixed by this fixture set (kept here for the record): the
interpreter previously hardwired gpr[0]=0 (real PPC r0 is a normal register;
the PowerOpen stubs use it), and the HLE trampoline lis was mis-encoded as
addi — both would have broken every retail EBOOT at the first import call.

## v0.6.0 additions — Android host overhaul (session 2)

User-device test script (this sandbox has no Android hardware — these are
the checks the user runs on their Android 13 device):

1. Install Any_Ps3-0.6.0-arm64.apk over the old app (no uninstall needed;
   same debug key, versionCode 4 > 3).
2. Launcher: new gold "3" icon visible; if the launcher supports themed
   icons, enable it and confirm the monochrome variant tints with the
   wallpaper.
3. Readability: library, settings, per-game settings and the log viewer
   must all show clearly readable light-on-dark text (no black-on-black
   anywhere); minimum readable size everywhere (log = 12sp monospace).
4. Export logs (THE bug report): Settings → "Save log report to Downloads"
   → a toast states "Report saved: Downloads/Any_Ps3/Any_Ps3-log-….txt", no
   crash; the file is visible in the system Files app under Downloads/Any_Ps3.
   Secondary: "Share diagnostics report" opens the share sheet.
5. Log viewer: tap the log bar at the bottom of the library → full-screen
   log; filter chips, search, auto-scroll toggle, long-press to select text,
   Copy / Save / Share buttons all respond.
6. Rotation: rotate the device on the library → list, log and banner
   survive (state is app-scoped now).
7. Import sheet: "Import game" FAB → the in-app sheet with ISO/folder
   options; both open the system file picker.
8. Remove: the trash button asks for confirmation; cancelling keeps the
   game; confirming removes it (and its settings + key).
9. Crash instrument: if ANYTHING crashes, the next start shows the in-app
   crash report with the full stack trace; Save to Downloads / Share work
   from there (this is the report to send back for a surgical fix).
10. Per-game settings: open, edit controller-mapping numbers, Save → no
    recomposition loop (the old build could loop forever here); re-open to
    confirm the values persisted.
