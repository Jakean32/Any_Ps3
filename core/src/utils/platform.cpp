// Any_Ps3 — platform abstraction (POSIX + Windows). One translation unit,
// #ifdef-separated, so the glob-based build picks up exactly one variant.
#include "anyps3/platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cerrno>

namespace anyps3 {
namespace platform {

#ifdef _WIN32

bool MakeDir(const std::string& path) {
    if (path.empty()) return false;
    if (::CreateDirectoryA(path.c_str(), nullptr)) return true;
    return ::GetLastError() == ERROR_ALREADY_EXISTS && IsDir(path);
}

bool RemoveDir(const std::string& path) { return ::RemoveDirectoryA(path.c_str()) != 0; }

bool IsDir(const std::string& path) {
    DWORD a = ::GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsFile(const std::string& path) {
    DWORD a = ::GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ListDir(const std::string& dir, std::vector<std::string>* names) {
    names->clear();
    // FindFirstFileA wants "dir\\*"; with '/' separators the CRT still accepts
    // "dir/*".
    WIN32_FIND_DATAA fd;
    HANDLE h = ::FindFirstFileA((dir + "/*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        names->push_back(std::move(n));
    } while (::FindNextFileA(h, &fd));
    ::FindClose(h);
    return true;
}

void* AllocRw(size_t size) {
    return ::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

bool ProtectExec(void* mem, size_t size) {
    DWORD old = 0;
    return ::VirtualProtect(mem, size, PAGE_EXECUTE_READ, &old) != 0;
}

bool ProtectRw(void* mem, size_t size) {
    DWORD old = 0;
    return ::VirtualProtect(mem, size, PAGE_READWRITE, &old) != 0;
}

void FreeMem(void* mem, size_t) { ::VirtualFree(mem, 0, MEM_RELEASE); }

#else  // POSIX

bool MakeDir(const std::string& path) {
    if (::mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST && IsDir(path);
}

bool RemoveDir(const std::string& path) { return ::rmdir(path.c_str()) == 0; }

bool IsDir(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool IsFile(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool ListDir(const std::string& dir, std::vector<std::string>* names) {
    names->clear();
    DIR* d = ::opendir(dir.c_str());
    if (!d) return false;
    while (struct dirent* e = ::readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        names->push_back(std::move(n));
    }
    ::closedir(d);
    return true;
}

void* AllocRw(size_t size) {
    return ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

bool ProtectExec(void* mem, size_t size) {
    return ::mprotect(mem, size, PROT_READ | PROT_EXEC) == 0;
}

bool ProtectRw(void* mem, size_t size) {
    return ::mprotect(mem, size, PROT_READ | PROT_WRITE) == 0;
}

void FreeMem(void* mem, size_t size) { ::munmap(mem, size); }

#endif

bool MakeDirs(const std::string& path) {
    std::string cur;
    for (size_t pos = 0; (pos = path.find('/', pos + 1)) != std::string::npos;) {
        cur = path.substr(0, pos);
        if (!cur.empty() && !IsDir(cur)) MakeDir(cur);
    }
    if (!path.empty() && !IsDir(path)) MakeDir(path);
    return IsDir(path);
}

}  // namespace platform
}  // namespace anyps3
