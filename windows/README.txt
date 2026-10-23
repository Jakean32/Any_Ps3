Any_Ps3 0.3.0 "App Shell" — Windows x64 build
==============================================

WHAT THIS IS
------------
Any_Ps3 is a clean-room engineered PlayStation 3 launcher built on an
AOT (ahead-of-time) recompilation architecture: PS3 PPU/SPU code is meant
to be lifted to LLVM IR and compiled to native x64 code per game, with
LV2 syscalls and cellOS provided through high-level emulation (HLE) and
RSX graphics mapped to a graphics API. This release contains the real
app shell plus the honest first stages of that architecture.

HONEST STATUS (no fake progress, ever)
--------------------------------------
Implemented and working today:
  - No-copy game import: ISO images and folder dumps are read in place
    (PARAM.SFO parsed, covers extracted as small cached thumbnails).
  - Per-game compile cache with manifest (version, source hash, status)
    and real per-run compile logs.
  - PPU ELF64 big-endian loader and PowerPC branch/call scanner (the
    analysis stage of the AOT pipeline). Indirect jumps are identified
    as fallback-interpreter candidates.
  - LV2 syscall HLE framework with a small honest syscall set; unknown
    syscalls return CELL_ENOSYS and are logged.
  - First-run wizard, library grid, settings, logs, splash animation.

Not implemented yet (the UI says so explicitly):
  - LLVM IR lifting and native code generation ("Compiler stage not
    implemented yet" is shown verbatim).
  - Executable-artifact loading / game launching.
  - RSX graphics (Vulkan/DX backend), cellOS module HLE beyond the stubs.

LEGAL
-----
Any_Ps3 contains NO Sony firmware, decryption keys, or game content, and
does not bypass copy protection. You may only import and compile games
you legally own; dumping games you own may still be restricted in your
country. This project is not affiliated with Sony.

USAGE
-----
1. Run Any_Ps3.exe. The first-run wizard asks for a compile/cache folder
   (a subfolder "AnyPs3_Cache" is created inside your choice) and shows
   the legal notice.
2. Import games with "+ Import ISO..." (multi-select) or "Scan folder..."
   (recursive scan of a folder with ISOs/folder dumps).
3. Click a game card -> Compile to run the analysis pipeline. Progress
   shown is real; stages that are not implemented are labeled as such.
4. Settings shows free space, cache location, and lets you re-run the
   wizard. Logs shows the activity log.

REQUIREMENTS
------------
Windows 8+ x64 with Direct3D 11 (the exe falls back to WARP software
rendering if no GPU is present). No additional runtimes are needed:
the binary is statically linked (only system DLLs are imported:
d3d11, dxgi, D3DCompiler_47, GDI32, USER32, SHELL32, ole32, dwmapi).

FILES
-----
Any_Ps3.exe                       the application (static x64)
logo_full.png                     app logo (keep next to the exe; used by
                                  splash, wizard and header)
verification_wine_screenshot.png screenshot of this exact exe running
                                  under Wine 10 (see below)
README.txt                        this file

VERIFICATION (done at build time, reported honestly)
----------------------------------------------------
- The full core test suite (40 tests) was cross-compiled with this same
  Windows toolchain and executed under Wine 10.0: 40/40 passed,
  including PARAM.SFO parsing, ISO9660 reads through a raw file
  descriptor, and cache-manifest round-trips.
- This GUI exe was run under Wine with a virtual display: it created
  its window, initialized Direct3D 11, and rendered the shell (screenshot
  included). It was not run on a physical Windows machine by the
  developers of this package - if anything misbehaves on real hardware,
  please report it.

SOURCE
------
Every release ships with Any_Ps3_source.zip - the complete source of the
core, both frontends, tests, fixtures and build scripts.
