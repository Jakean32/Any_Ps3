Any_Ps3 — Windows build (0.1.0 "app shell")
===========================================

WHAT THIS IS
------------
An experimental PlayStation 3 game-launcher shell built around ahead-of-time
(AOT) recompilation. This build contains the complete app shell — game
import, library, settings, logging — plus the real import/boot pipeline and
a built-in hello-world demo that actually executes through the PPU
interpreter. "Play" on a normal game does NOT pretend to run it: it shows the
honest boot log plus the message "Emulation not implemented yet".

REQUIREMENTS
------------
- Windows 10/11 x64 (should also run on 8.1 x64)
- No installer, no redistributables: Any_Ps3.exe is statically linked
  (MinGW-w64, -static); it only loads Windows system components
  (d3d11.dll, d3dcompiler_47.dll and other OS DLLs). If graphics drivers are
  broken it falls back to a software (WARP) device.

RUN
---
1. Unzip anywhere writable (e.g. Desktop\Any_Ps3).
2. Double-click Any_Ps3.exe.
3. Accept the first-run legal notice (you must own your game dumps; this
   program ships no Sony firmware, no decryption keys, no game content).
4. The library opens with one built-in entry, "Hello World (Any_Ps3 demo)".
   Press "Play": the boot log runs and the demo's PPC64 code really executes
   (it prints "Hello from Any_Ps3!" via the emulated sys_tty_write and exits).
5. "Add game (ISO / folder)..." opens a file picker; click it twice to switch
   between ISO images and folder dumps. Folder dumps and ISO9660 images with
   PARAM.SFO are imported through the real pipeline (title, TITLE_ID, ICON0).
6. Per-game settings and global settings are under the top menu.

COMMAND LINE
------------
Any_Ps3.exe            normal start (GUI)
Any_Ps3.exe --selftest headless end-to-end test: installs the demo, boots and
                       executes it, prints the boot log, exits 0 on success
                       (exit code 42 from the guest, TTY output verified).
                       Also writes selftest.log next to the exe.

HONEST STATUS (what works / what does not)
------------------------------------------
Works in this build (tested by 37 automated tests + Wine verification):
- ISO9660 / folder-dump import, PARAM.SFO + ICON0.PNG parsing, game library
- ELF64 (PPC64 big-endian) and plaintext fSELF executable loading
- PPU interpreter execution, LV2 HLE syscalls (process/tty/memory/fs/time)
- The built-in hello-world demo executes end-to-end (this is the same binary
  the Linux CI runs: PPC64 code produced by GNU binutils)
- Per-game settings, AOT cache manifests, content-addressed shader cache
Not implemented yet (never faked):
- Graphics (RSX / GCM / Vulkan), audio, real game execution
- Encrypted retail EBOOTs (no keys shipped — cannot and will not load)
- UDF-only disc images (folder dumps and ISO9660 work)
- The AOT native backend emits x86-64 SysV-ABI code, so on Windows the shell
  runs the interpreter backend by design; native AOT execution is exercised
  on Linux/x64 builds.

FILES
-----
Any_Ps3.exe          the app (single static binary)
samples\hello.elf    the demo executable (GNU-assembled PPC64 test code,
                     no Sony content; byte-identical to the automated-test
                     fixture, md5 21f2b89d14fa1da93dec4810efed156b)
data\                created next to the exe on first run (library, config,
                     caches). Delete it to reset the app.
LICENSES.txt         full license text of every component

VERIFICATION PERFORMISHED ON THIS ARTIFACT
------------------------------------------(by the build pipeline, see PHASE_REPORTS.md)
- Parsed with objdump: imports are Windows system DLLs only (no
  libstdc++-6.dll / libgcc / libwinpthread) -> truly static, runs on clean
  Windows installs.
- Executed under Wine 10.0 (Debian) in a clean prefix:
  * --selftest: PASS (guest exit code 42, TTY "Hello from Any_Ps3!")
  * GUI smoke test under Xvfb: 60 frames rendered, clean exit, library state
    created correctly.

Source code: see the accompanying source archive.
Project reports: PHASE_REPORTS.md documents exactly what each phase delivered.
