// Any_Ps3 — small platform abstraction so the core builds unchanged on Linux,
// Windows (MinGW/MSVC) and Android (NDK). Everything the core needs beyond the
// C++17 standard library lives behind these functions; call sites must not
// include <unistd.h>/<dirent.h>/<sys/mman.h> directly.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace anyps3 {
namespace platform {

// --- filesystem ------------------------------------------------------------
// Note: paths passed here use '/' separators (Windows CRT accepts them).

// mkdir -p a single component (no recursive walk here). Returns true if the
// directory exists afterwards.
bool MakeDir(const std::string& path);

// rmdir (empty directory only).
bool RemoveDir(const std::string& path);

bool IsDir(const std::string& path);
bool IsFile(const std::string& path);

// Directory entry names (no "." / ".."), unsorted. Returns false if the
// directory cannot be opened.
bool ListDir(const std::string& dir, std::vector<std::string>* names);

// mkdir -p (all components).
bool MakeDirs(const std::string& path);

// --- executable memory (W^X, used by the native AOT backend) ---------------
// MiniAOT writes translated blocks with RW protection and flips them to RX
// before executing (write-xor-exec). These are the only privileged-memory
// operations in the core.

// Anonymous RW mapping (not executable). Returns nullptr on failure.
void* AllocRw(size_t size);

// Flip RW mapping -> read+execute. Returns false on failure.
bool ProtectExec(void* mem, size_t size);

// Flip RX mapping back to read+write (before release / patching).
bool ProtectRw(void* mem, size_t size);

void FreeMem(void* mem, size_t size);

}  // namespace platform
}  // namespace anyps3
