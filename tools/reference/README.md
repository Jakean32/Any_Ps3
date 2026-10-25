# Reference materials

Ground-truth artifacts used to build and validate Any_Ps3 (all fetched/generated
during development; regenerate with a GNU binutils powerpc64 toolchain):

- `syscall_table.txt` — the LV2 syscall number table (449 entries), extracted
  from the RPCS3 source (`lv2.cpp` syscall registration table) with
  `extract_syscalls.py`. Cross-checked against the ps3recomp syscall docs.
- `spu_rows.txt` — the SPU ISA opcode rows (magn, opcode, mnemonic, 199 rows)
  extracted from RPCS3's `SPUOpcodes.h` decoder table. Used to generate
  `core/src/cpu/spu/spu_dispatch_table.inc`.
- `inventory.s` + `inventory.dump` — the PPC64 instruction inventory assembled
  by GNU binutils 2.44 (`powerpc64-linux-gnu-as`), objdump ground truth used to
  validate the PPU decoder (tests/test_ppu.cpp decodes every word).
- `gen_fixtures.py` — regenerates `tests/fixtures/ppu_fixtures.h` from the
  `.s` sources with any powerpc64 assembler (see scripts/regen_ppu_fixtures.sh).

To get the assembler: `apt-get download binutils-powerpc64-linux-gnu` then
`dpkg-deb -x` (no root needed), as documented in scripts/regen_ppu_fixtures.sh.
