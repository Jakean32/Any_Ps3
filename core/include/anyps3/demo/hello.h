// Any_Ps3 — built-in hello-world demo entry.
//
// The launcher ships a tiny legal test executable (PPC64 code assembled with
// GNU binutils — no Sony content) so users can exercise the *real* import +
// boot + execute pipeline without owning a game dump. "Play" on the demo runs
// the actual ELF loader -> guest VM -> PPU interpreter -> LV2 HLE ->
// sys_tty_write / _sys_process_exit path that the automated tests cover.
#pragma once

#include "anyps3/loader/import.h"

#include <string>

namespace anyps3 {
namespace demo {

// Creates <config_dir>/demo-src/{PARAM.SFO, EBOOT.BIN} (EBOOT.BIN = copy of
// `hello_elf_path`, a big-endian PPC64 ELF) and pushes it through the normal
// game-import pipeline, producing a regular library entry with
// TITLE_ID "DEMO00001".
//
// hello_elf_path must point to the packaged sample (Windows: samples/hello.elf
// next to the exe; Android: the asset copied to app storage). Returns true on
// success. Idempotent: re-installing overwrites the demo folder and manifest.
bool InstallHelloDemo(const std::string& config_dir, const std::string& hello_elf_path,
                      GameInfo* out = nullptr, std::string* err = nullptr);

// Well-known TITLE_ID of the demo entry.
inline const char* kDemoTitleId = "DEMO00001";

}  // namespace demo
}  // namespace anyps3
