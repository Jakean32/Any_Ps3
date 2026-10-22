# Any_Ps3 — Test Plan

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
