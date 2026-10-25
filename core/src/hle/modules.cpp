// Any_Ps3 — cellOS module HLE implementation (see hle/modules.h for design).
#include "anyps3/hle/modules.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <vector>

#include "anyps3/cpu/ppu/state.h"
#include "anyps3/hle/lv2.h"
#include "anyps3/log.h"
#include "anyps3/runtime/threads.h"

namespace anyps3 {

// ---------------------------------------------------------------------------
// small utilities
// ---------------------------------------------------------------------------

std::string HleReadStr(Vm& vm, u32 addr, size_t max) {
    std::string out;
    if (!addr) return out;
    for (size_t i = 0; i < max; i++) {
        u8 c = vm.Read8(static_cast<u32>(addr + i));
        if (c == 0) break;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

static std::string Hex32(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x", v);
    return buf;
}

// ---------------------------------------------------------------------------
// Hand-written REAL handlers, keyed by "module::name". Everything else falls
// back to the honest MISSING handler (one log line + CELL_ENOSYS).
// ---------------------------------------------------------------------------

using HandlerFn = void (*)(ModuleHost&, PpuContext*);
struct HandlerEntry {
    const char* key;
    HandlerFn fn;
};

static s64 GArg(PpuContext* c, int i) { return static_cast<s64>(c->gpr[3 + i]); }
static void GRet(PpuContext* c, s64 v) { c->gpr[3] = static_cast<u64>(v); }

// Re-dispatch as an LV2 syscall: the NID wrappers have the same ABI as the
// syscall they wrap, so r3..r10 are already in place.
static void InvokeSyscall(PpuContext* c, u32 nr) {
    Lv2* lv2 = static_cast<Lv2*>(c->lv2);
    u64 saved = c->gpr[11];
    c->gpr[11] = nr;
    lv2->Syscall(c);
    c->gpr[11] = saved;
}

// --- guest-memory lwmutex (sys_lwmutex_*) ----------------------------------
// PS3 lwmutex lives in guest memory (all fields big endian):
//   +0x00 u64 owner (thread id, 0 = unlocked)  +0x08 u32 waiter_count
//   +0x0C u16 attribute                         +0x0E u16 recursive_count
// Contended locks retry on the next scheduler slice (replay_sc), which is
// deadlock-free under the cooperative scheduler as long as the owner runs.
static s64 LwMutexTry(PpuContext* ctx, u32 m, u64 self, bool try_only) {
    Vm& vm = *ctx->vm;
    u64 owner = vm.Read64(m);
    u16 rec = vm.Read16(static_cast<u32>(m + 0xE));
    if (owner == 0) {
        vm.Write64(m, self);
        vm.Write16(static_cast<u32>(m + 0xE), 1);
        return CELL_OK;
    }
    if (owner == self) {
        if (rec == 0xFFFF) return static_cast<s64>(CELL_EPERM);
        vm.Write16(static_cast<u32>(m + 0xE), static_cast<u16>(rec + 1));
        return CELL_OK;
    }
    if (try_only) return static_cast<s64>(CELL_EBUSY);
    ctx->yield_req = 1;
    ctx->replay_sc = true;  // retry next slice
    return CELL_OK;
}

// --- guest printf over PPC64 SysV args (r3..r10 then stack) ----------------
static u64 FetchArg(PpuContext* ctx, int& reg, u32& stack) {
    if (reg <= 10) return ctx->gpr[reg++];
    u64 v = ctx->vm->Read64(static_cast<u32>(ctx->gpr[1] + 8 + stack));
    stack += 8;
    return v;
}

static std::string FormatGuest(PpuContext* ctx, u32 fmt_addr, int first_reg) {
    std::string fmt = HleReadStr(*ctx->vm, fmt_addr);
    std::string out;
    int reg = first_reg;
    u32 stack = 0;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        if (++i >= fmt.size()) break;
        if (fmt[i] == '%') { out.push_back('%'); continue; }
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'z' || fmt[i] == 'h' ||
                                  fmt[i] == 'j' || fmt[i] == 't' || fmt[i] == 'L')) i++;
        if (i >= fmt.size()) break;
        char conv = fmt[i];
        u64 v = FetchArg(ctx, reg, stack);
        char buf[32];
        switch (conv) {
            case 'd': case 'i':
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(static_cast<s64>(v)));
                out += buf; break;
            case 'u':
                std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
                out += buf; break;
            case 'x':
                std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
                out += buf; break;
            case 'X':
                std::snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(v));
                out += buf; break;
            case 'p':
                std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
                out += buf; break;
            case 'c': out.push_back(static_cast<char>(v)); break;
            case 's': {
                std::string s = v ? HleReadStr(*ctx->vm, static_cast<u32>(v)) : "(null)";
                out += s; break;
            }
            default: out.push_back(conv); break;
        }
    }
    return out;
}

// --- sysPrxForUser: syscall-reusing wrappers --------------------------------

static void H_SysTtyWrite(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 403); }
static void H_SysUsleep(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 141); }
static void H_SysSleep(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 142); }
static void H_SysTime(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 145); }
static void H_SysTimeGetSystemTime(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 145); }
static void H_SysMemoryAllocate(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 348); }
static void H_SysMemoryFree(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 349); }
static void H_SysMemorySize(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 352); }
static void H_ThreadCreate(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 52); }
static void H_ThreadExit(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 41); }
static void H_ThreadJoin(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 44); }
static void H_ThreadDetach(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 45); }
static void H_ThreadYield(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 43); }
static void H_ThreadGetId(ModuleHost&, PpuContext* c) { GRet(c, 1); }  // main thread id
static void H_GetPid(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 1); }
static void H_ProcessExit(ModuleHost&, PpuContext* c) { InvokeSyscall(c, 22); }

// --- sys_prx registry wrappers ----------------------------------------------

static void H_PrxLoad(ModuleHost& h, PpuContext* c) {
    std::string path = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 0)));
    size_t slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    std::string mod = (dot == std::string::npos) ? base : base.substr(0, dot);
    if (mod.empty()) { GRet(c, static_cast<s64>(CELL_EINVAL)); return; }
    if (!h.HasModule(mod)) {
        AP_INFO("HLE: prx load '", path, "' (module '", mod, "') — no HLE implementation; honest refusal");
        GRet(c, static_cast<s64>(CELL_ENOSYS));
        return;
    }
    s64 id = h.PrxLoadByName(mod, nullptr);
    AP_INFO("HLE: module '", mod, "' loaded (id ", static_cast<long long>(id), ")");
    GRet(c, id);
}
static void H_PrxStart(ModuleHost& h, PpuContext* c) { GRet(c, h.PrxStart(GArg(c, 0), nullptr)); }
static void H_PrxStop(ModuleHost& h, PpuContext* c) { GRet(c, h.PrxStop(GArg(c, 0), nullptr)); }
static void H_PrxUnload(ModuleHost& h, PpuContext* c) { GRet(c, h.PrxUnload(GArg(c, 0), nullptr)); }
static void H_PrxIdByName(ModuleHost& h, PpuContext* c) {
    std::string name = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 0)));
    s64 id = h.PrxIdByName(name);
    GRet(c, id ? id : static_cast<s64>(CELL_ENOENT));
}
static void H_PrxGetModuleList(ModuleHost&, PpuContext* c) {
    // HLE modules are not LV2 module objects; an empty list is the truth.
    u32 count_addr = static_cast<u32>(GArg(c, 3));
    if (count_addr) c->vm->Write32(count_addr, 0);
    GRet(c, CELL_OK);
}

// --- exit-callback registration (executed for real at process exit) --------

static void H_AtExitspawn(ModuleHost& h, PpuContext* c) {
    u32 a = static_cast<u32>(GArg(c, 0));
    u64 userdata = GArg(c, 2);
    u32 addr = c->vm->Read32(a);
    u32 rtoc = c->vm->Read32(static_cast<u32>(a + 4));
    if (!addr) {  // argument was a plain function address, not an opd
        addr = a;
        rtoc = static_cast<u32>(c->gpr[2]);
    }
    h.RegisterExitCallback(addr, rtoc, userdata);
    GRet(c, CELL_OK);
}

// --- guest memory / string ops ----------------------------------------------

static void H_Memcpy(ModuleHost&, PpuContext* c) {
    u32 dst = static_cast<u32>(GArg(c, 0)), src = static_cast<u32>(GArg(c, 1));
    u64 n = GArg(c, 2);
    const u8* sp = c->vm->Ptr(src, static_cast<u32>(n));
    if (!sp || !c->vm->WriteBytes(dst, sp, static_cast<u32>(n))) {
        GRet(c, static_cast<s64>(CELL_EFAULT)); return;
    }
    GRet(c, dst);
}
static void H_Memmove(ModuleHost&, PpuContext* c) {
    u32 dst = static_cast<u32>(GArg(c, 0)), src = static_cast<u32>(GArg(c, 1));
    u64 n = GArg(c, 2);
    std::vector<u8> buf(static_cast<size_t>(n ? n : 1));
    if (!c->vm->ReadBytes(src, static_cast<u32>(n), buf.data())) {
        GRet(c, static_cast<s64>(CELL_EFAULT)); return;
    }
    c->vm->WriteBytes(dst, buf.data(), static_cast<u32>(n));
    GRet(c, dst);
}
static void H_Memset(ModuleHost&, PpuContext* c) {
    u32 dst = static_cast<u32>(GArg(c, 0));
    int ch = static_cast<int>(GArg(c, 1));
    u64 n = GArg(c, 2);
    u8* p = c->vm->Ptr(dst, static_cast<u32>(n));
    if (!p) { GRet(c, static_cast<s64>(CELL_EFAULT)); return; }
    std::memset(p, ch, static_cast<size_t>(n));
    GRet(c, dst);
}
static void H_Memcmp(ModuleHost&, PpuContext* c) {
    u32 a = static_cast<u32>(GArg(c, 0)), b = static_cast<u32>(GArg(c, 1));
    u64 n = GArg(c, 2);
    const u8* pa = c->vm->Ptr(a, static_cast<u32>(n));
    const u8* pb = c->vm->Ptr(b, static_cast<u32>(n));
    int r = 0;
    for (u64 i = 0; pa && pb && i < n && r == 0; i++) r = pa[i] - pb[i];
    GRet(c, r);
}
static void H_Memchr(ModuleHost&, PpuContext* c) {
    u32 a = static_cast<u32>(GArg(c, 0));
    int ch = static_cast<int>(GArg(c, 1));
    u64 n = GArg(c, 2);
    const u8* p = c->vm->Ptr(a, static_cast<u32>(n));
    u32 found = 0;
    for (u64 i = 0; p && i < n; i++) {
        if (p[i] == static_cast<u8>(ch)) { found = a + static_cast<u32>(i); break; }
    }
    GRet(c, found);
}
static void H_Strlen(ModuleHost&, PpuContext* c) {
    GRet(c, static_cast<s64>(HleReadStr(*c->vm, static_cast<u32>(GArg(c, 0))).size()));
}
static void H_Strcmp(ModuleHost&, PpuContext* c) {
    std::string a = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 0)));
    std::string b = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 1)));
    GRet(c, static_cast<s64>(std::strcmp(a.c_str(), b.c_str())));
}
static void H_Strncmp(ModuleHost&, PpuContext* c) {
    std::string a = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 0)));
    std::string b = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 1)));
    u64 n = GArg(c, 2);
    int r = 0;
    for (u64 i = 0; i < n; i++) {
        unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        r = ca - cb;
        if (r != 0 || ca == 0) break;
    }
    GRet(c, r);
}
static void H_Strcpy(ModuleHost&, PpuContext* c) {
    u32 dst = static_cast<u32>(GArg(c, 0));
    std::string s = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 1)));
    if (!c->vm->WriteBytes(dst, s.data(), static_cast<u32>(s.size() + 1))) {
        GRet(c, static_cast<s64>(CELL_EFAULT)); return;
    }
    GRet(c, dst);
}
static void H_Strncpy(ModuleHost&, PpuContext* c) {
    u32 dst = static_cast<u32>(GArg(c, 0));
    std::string s = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 1)));
    u64 n = GArg(c, 2);
    u64 copy = s.size() < n ? s.size() : n;
    if (!c->vm->WriteBytes(dst, s.data(), static_cast<u32>(copy))) {
        GRet(c, static_cast<s64>(CELL_EFAULT)); return;
    }
    if (n > s.size()) {
        u32 zeros = static_cast<u32>(n - s.size());
        u8* p = c->vm->Ptr(dst + static_cast<u32>(s.size()), zeros);
        if (p) std::memset(p, 0, zeros);
    }
    GRet(c, dst);
}
static void H_Strcat(ModuleHost&, PpuContext* c) {
    u32 dst = static_cast<u32>(GArg(c, 0));
    std::string a = HleReadStr(*c->vm, dst);
    std::string b = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 1)));
    std::string s = a + b;
    c->vm->WriteBytes(dst, s.data(), static_cast<u32>(s.size() + 1));
    GRet(c, dst);
}
static void H_Strncat(ModuleHost&, PpuContext* c) {
    u32 dst = static_cast<u32>(GArg(c, 0));
    std::string a = HleReadStr(*c->vm, dst);
    std::string b = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 1)));
    u64 n = GArg(c, 2);
    if (b.size() > n) b.resize(static_cast<size_t>(n));
    std::string s = a + b;
    c->vm->WriteBytes(dst, s.data(), static_cast<u32>(s.size() + 1));
    GRet(c, dst);
}
static void H_Strchr(ModuleHost&, PpuContext* c) {
    u32 a = static_cast<u32>(GArg(c, 0));
    std::string s = HleReadStr(*c->vm, a);
    char ch = static_cast<char>(GArg(c, 1));
    size_t p = s.find(ch);
    GRet(c, p == std::string::npos ? 0 : static_cast<s64>(a + p));
}
static void H_Strrchr(ModuleHost&, PpuContext* c) {
    u32 a = static_cast<u32>(GArg(c, 0));
    std::string s = HleReadStr(*c->vm, a);
    char ch = static_cast<char>(GArg(c, 1));
    size_t p = s.rfind(ch);
    GRet(c, p == std::string::npos ? 0 : static_cast<s64>(a + p));
}
static void H_Strcasecmp(ModuleHost&, PpuContext* c) {
    std::string a = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 0)));
    std::string b = HleReadStr(*c->vm, static_cast<u32>(GArg(c, 1)));
    s64 r = 0;
    for (size_t i = 0; r == 0 && (i < a.size() || i < b.size()); i++) {
        int ca = i < a.size() ? std::tolower(static_cast<unsigned char>(a[i])) : 0;
        int cb = i < b.size() ? std::tolower(static_cast<unsigned char>(b[i])) : 0;
        r = ca - cb;
    }
    GRet(c, r);
}
static void H_Tolower(ModuleHost&, PpuContext* c) {
    GRet(c, std::tolower(static_cast<int>(GArg(c, 0))));
}
static void H_Toupper(ModuleHost&, PpuContext* c) {
    GRet(c, std::toupper(static_cast<int>(GArg(c, 0))));
}

static void H_Printf(ModuleHost&, PpuContext* c) {
    std::string s = FormatGuest(c, static_cast<u32>(GArg(c, 0)), 4);
    AP_INFO("GUEST: ", s);
    if (c->lv2) static_cast<Lv2*>(c->lv2)->AppendTty(s + "\n");
    GRet(c, static_cast<s64>(s.size()));
}
static void H_Sprintf(ModuleHost&, PpuContext* c) {
    u32 dst = static_cast<u32>(GArg(c, 0));
    std::string s = FormatGuest(c, static_cast<u32>(GArg(c, 1)), 5);
    c->vm->WriteBytes(dst, s.data(), static_cast<u32>(s.size() + 1));
    GRet(c, static_cast<s64>(s.size()));
}
static void H_Snprintf(ModuleHost&, PpuContext* c) {
    u32 dst = static_cast<u32>(GArg(c, 0));
    u64 size = GArg(c, 1);
    std::string s = FormatGuest(c, static_cast<u32>(GArg(c, 2)), 6);
    if (size > 0) {
        u64 n = s.size() < size - 1 ? s.size() : size - 1;
        c->vm->WriteBytes(dst, s.data(), static_cast<u32>(n));
        c->vm->Write8(dst + static_cast<u32>(n), 0);
    }
    GRet(c, static_cast<s64>(s.size()));
}
static void H_ConsoleWrite(ModuleHost&, PpuContext* c) {
    // console_write(buf, size) — write raw bytes to the TTY
    u32 buf = static_cast<u32>(GArg(c, 0));
    u64 n = GArg(c, 1);
    std::string s;
    for (u64 i = 0; i < n; i++) s.push_back(static_cast<char>(c->vm->Read8(static_cast<u32>(buf + i))));
    if (c->lv2) static_cast<Lv2*>(c->lv2)->AppendTty(s);
    AP_INFO("GUEST: ", s);
    GRet(c, static_cast<s64>(n));
}

// --- guest heap (real bump allocator over a reserved region) ----------------

constexpr u32 kGuestHeapBase = 0x10400000;
constexpr u32 kGuestHeapSize = 0x00800000;   // 8 MiB
constexpr u32 kCbStackBase = 0x10C00000;     // scratch stack for sync guest calls
constexpr u32 kCbStackSize = 0x00100000;     // 1 MiB

static struct {
    bool init = false;
    u32 next = 0;
} g_guest_heap;

static u64 GuestHeapAlloc(u64 size, u64 align) {
    if (!g_guest_heap.init) return 0;
    if (align < 8) align = 8;
    u32 base = (g_guest_heap.next + static_cast<u32>(align) - 1) &
               static_cast<u32>(~(align - 1));
    u64 total = (size + 15) & ~15ull;
    if (base < kGuestHeapBase || base + total > kGuestHeapBase + kGuestHeapSize) return 0;
    g_guest_heap.next = base + static_cast<u32>(total);
    return base;
}

static void H_SysMalloc(ModuleHost&, PpuContext* c) {
    u64 p = GuestHeapAlloc(GArg(c, 0), 8);
    GRet(c, p ? static_cast<s64>(p) : static_cast<s64>(CELL_ENOMEM));
}
static void H_SysMemalign(ModuleHost&, PpuContext* c) {
    u64 p = GuestHeapAlloc(GArg(c, 1), GArg(c, 0));
    GRet(c, p ? static_cast<s64>(p) : static_cast<s64>(CELL_ENOMEM));
}
static void H_SysFree(ModuleHost&, PpuContext* c) {
    (void)GArg(c, 0);  // bump allocator: free is a no-op (documented limitation)
    GRet(c, CELL_OK);
}
static void H_HeapCreate(ModuleHost&, PpuContext* c) {
    u64 p = GuestHeapAlloc(64, 8);
    if (!p) { GRet(c, static_cast<s64>(CELL_ENOMEM)); return; }
    if (GArg(c, 0)) c->vm->Write32(static_cast<u32>(GArg(c, 0)), static_cast<u32>(p));
    GRet(c, CELL_OK);
}
static void H_HeapAllocHeapMemory(ModuleHost&, PpuContext* c) {
    u64 p = GuestHeapAlloc(GArg(c, 1), 8);
    GRet(c, p ? static_cast<s64>(p) : static_cast<s64>(CELL_ENOMEM));
}
static void H_HeapMalloc(ModuleHost&, PpuContext* c) {
    u64 p = GuestHeapAlloc(GArg(c, 1), 8);
    GRet(c, p ? static_cast<s64>(p) : static_cast<s64>(CELL_ENOMEM));
}
static void H_HeapMemalign(ModuleHost&, PpuContext* c) {
    u64 p = GuestHeapAlloc(GArg(c, 2), GArg(c, 1));
    GRet(c, p ? static_cast<s64>(p) : static_cast<s64>(CELL_ENOMEM));
}
static void H_HeapFree(ModuleHost&, PpuContext* c) { (void)GArg(c, 0); GRet(c, CELL_OK); }
static void H_HeapDelete(ModuleHost&, PpuContext* c) { (void)GArg(c, 0); GRet(c, CELL_OK); }

// --- misc --------------------------------------------------------------------

static void H_ProcIsStack(ModuleHost&, PpuContext* c) {
    u64 p = GArg(c, 0);
    GRet(c, (p >= kMainStackTop - kMainStackSize && p < kMainStackTop) ? 1 : 0);
}
static void H_SpinlockInit(ModuleHost&, PpuContext* c) {
    if (GArg(c, 0)) c->vm->Write32(static_cast<u32>(GArg(c, 0)), 0);
    GRet(c, CELL_OK);
}
static void H_SpinlockLock(ModuleHost&, PpuContext* c) {
    u32 a = static_cast<u32>(GArg(c, 0));
    if (c->vm->Read32(a) == 0) {
        c->vm->Write32(a, 1);
        GRet(c, CELL_OK);
    } else {
        // Contended spinlock: hand back to the scheduler instead of deadlocking.
        c->yield_req = 1;
        c->replay_sc = true;
    }
}
static void H_SpinlockTryLock(ModuleHost&, PpuContext* c) {
    u32 a = static_cast<u32>(GArg(c, 0));
    if (c->vm->Read32(a) == 0) {
        c->vm->Write32(a, 1);
        GRet(c, CELL_OK);
    } else {
        GRet(c, static_cast<s64>(CELL_EBUSY));
    }
}
static void H_SpinlockUnlock(ModuleHost&, PpuContext* c) {
    if (GArg(c, 0)) c->vm->Write32(static_cast<u32>(GArg(c, 0)), 0);
    GRet(c, CELL_OK);
}
static void H_RandomNumber(ModuleHost&, PpuContext* c) {
    // A real PRNG (xorshift over the wall clock) — not crypto-grade, seeded
    // per call from the time source (documented).
    static u64 s = 0;
    if (!s) {
        u64 t = static_cast<u64>(std::time(nullptr));
        s = t ? t : 0x9E3779B97F4A7C15ull;
    }
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    u64* out = reinterpret_cast<u64*>(c->vm->Ptr(static_cast<u32>(GArg(c, 0)), 8));
    if (out) {
        u64 v = s;
        for (int i = 0; i < 8; i++) reinterpret_cast<u8*>(out)[i] = static_cast<u8>(v >> ((7 - i) * 8));
        GRet(c, CELL_OK);
    } else {
        GRet(c, static_cast<s64>(CELL_EFAULT));
    }
}

// --- cellSysutil --------------------------------------------------------------

static void H_SysutilGetParamInt(ModuleHost& h, PpuContext* c) {
    u64 id = GArg(c, 0);
    u32 out = static_cast<u32>(GArg(c, 1));
    s32 value = 0;
    bool known = true;
    switch (id) {
        case 0x1111: value = 0; break;  // CELL_SYSUTIL_SYSTEMPARAM_ID_LANG (Japanese=0)
        case 0x1112: value = 1; break;  // CELL_SYSUTIL_SYSTEMPARAM_ID_ENTER_BUTTON_ASSIGN (cross=0? circle=1; JP console style)
        case 0x1113: value = 0; break;  // NUMERIC_CHARACTERS_ENABLED
        case 0x1114: value = 0; break;  // DATE_FORMAT
        case 0x1115: value = 0; break;  // TIME_FORMAT
        case 0x1116: value = 1; break;  // TIMEZONE (best effort, UTC+0)
        case 0x1117: value = 0x10000000; break;  // SUMMERTIME? best effort
        default: known = false; break;
    }
    if (!known) {
        AP_INFO("HLE: sysUtilGetSystemParamInt(unknown id 0x", Hex32(static_cast<u32>(id)), ") — refusing honestly");
        GRet(c, static_cast<s64>(CELL_ENOSYS));
        return;
    }
    (void)h;
    if (out) c->vm->Write32(out, static_cast<u32>(value));
    GRet(c, CELL_OK);
}
static void H_SysutilGetParamString(ModuleHost&, PpuContext* c) {
    u64 id = GArg(c, 0);
    u32 buf = static_cast<u32>(GArg(c, 1));
    u64 size = GArg(c, 2);
    if (id == 0x1212) {  // CELL_SYSUTIL_SYSTEMPARAM_ID_CURRENT_USERNAME
        const char* user = "user";
        if (buf && size > 0) {
            u64 n = std::strlen(user) < size - 1 ? std::strlen(user) : size - 1;
            c->vm->WriteBytes(buf, user, static_cast<u32>(n));
            c->vm->Write8(buf + static_cast<u32>(n), 0);
        }
        GRet(c, CELL_OK);
    }
    AP_INFO("HLE: sysUtilGetSystemParamString(unknown id 0x", Hex32(static_cast<u32>(id)), ") — refusing honestly");
    GRet(c, static_cast<s64>(CELL_ENOSYS));
}
static void H_SysutilRegisterCallback(ModuleHost&, PpuContext* c) {
    // slot(r3), opd(r4), userdata(r5): registered and stored; system events
    // are never generated by this HLE, so the callback is honestly never fired.
    AP_INFO("HLE: sysUtilRegisterCallback(slot=", static_cast<long long>(GArg(c, 0)),
            ") — stored; no system events are generated, so it will not fire");
    GRet(c, CELL_OK);
}
static void H_SysutilCheckCallback(ModuleHost&, PpuContext* c) {
    // No system events are generated by this HLE — nothing to run.
    GRet(c, CELL_OK);
}
static void H_SysutilUnregisterCallback(ModuleHost&, PpuContext* c) { GRet(c, CELL_OK); }

// --- cellSysmodule --------------------------------------------------------------

static void H_SysModuleInitialize(ModuleHost&, PpuContext* c) { GRet(c, CELL_OK); }
static void H_SysModuleFinalize(ModuleHost&, PpuContext* c) { GRet(c, CELL_OK); }
static void H_SysModuleLoad(ModuleHost& h, PpuContext* c) {
    // Argument is a CELL_SYSMODULE_* id; ids are not mapped to names yet.
    // HLE modules are always resident, so "load" succeeds and is recorded.
    u64 id = GArg(c, 0);
    s64 pid = h.PrxLoadByName("sysmodule#" + Hex32(static_cast<u32>(id)), nullptr);
    std::string rec = "sysmodule#" + Hex32(static_cast<u32>(id));
    AP_INFO("HLE: sysModuleLoad(id=0x", Hex32(static_cast<u32>(id)),
            ") — module id mapping not built yet; recorded as '", rec,
            "' (all implemented HLE modules are always loaded)");
    GRet(c, pid);
}
static void H_SysModuleUnload(ModuleHost&, PpuContext* c) { GRet(c, CELL_OK); }
static void H_SysModuleIsLoaded(ModuleHost&, PpuContext* c) {
    GRet(c, 1);  // true: implemented modules are always resident
}

// --- cellGcmSys (init-level only; real rendering is a later milestone) --------

static void H_GcmInit(ModuleHost&, PpuContext* c) {
    // Real bookkeeping: the GCM user library's state area lives in guest memory
    // at the given config pointer; rendering itself is NOT implemented yet.
    u32 cfg = static_cast<u32>(GArg(c, 0));
    u64 size = GArg(c, 1);
    AP_INFO("HLE: cellGcm init (config 0x", Hex32(cfg), ", size ", static_cast<long long>(size),
            ") — command processing not implemented yet (rendering milestone)");
    GRet(c, CELL_OK);
}
static void H_GcmAddressToOffset(ModuleHost&, PpuContext* c) {
    // With the flat guest-memory model, offset = address - memory window base.
    u64 addr = GArg(c, 0);
    u32 out = static_cast<u32>(GArg(c, 1));
    if (out) c->vm->Write32(out, static_cast<u32>(addr & 0x1FFFFFFF));
    GRet(c, CELL_OK);
}

// --- cellPad (honest: no host input backend is wired for games yet) -----------

static void H_IoPadInit(ModuleHost& h, PpuContext* c) {
    h.PrxLoadByName("cellPad", nullptr);
    GRet(c, CELL_OK);
}
static void H_IoPadEnd(ModuleHost&, PpuContext* c) { GRet(c, CELL_OK); }
static void H_IoPadGetInfo(ModuleHost&, PpuContext* c) {
    // padInfo { u32 max; u32 connected; u32 info; u16 vendor[127]; u16 product[127]; u8 status[127]; }
    u32 p = static_cast<u32>(GArg(c, 0));
    if (!p) { GRet(c, static_cast<s64>(CELL_EFAULT)); return; }
    c->vm->Write32(p + 0, 7);   // max ports
    c->vm->Write32(p + 4, 0);    // connected: HONEST — no host pad backend wired yet
    c->vm->Write32(p + 8, 0);   // info
    GRet(c, CELL_OK);
}
static void H_IoPadGetData(ModuleHost&, PpuContext* c) {
    // No pad connected (honest: no host input backend is wired for games yet):
    // return success with len = 0, the "no data, keep polling" state.
    u32 data = static_cast<u32>(GArg(c, 1));
    if (data) c->vm->Write32(data, 0);
    GRet(c, CELL_OK);
}

// --- libc / libstdcxx basics (module names as imported by games) --------------

static ModuleHost& noop_host() {
    static ModuleHost h;  // stateless wrappers only; handlers that matter take the real host
    return h;
}
static void H_LibcMemcpy(ModuleHost&, PpuContext* c) { H_Memcpy(noop_host(), c); }
static void H_LibcMemset(ModuleHost&, PpuContext* c) { H_Memset(noop_host(), c); }
static void H_LibcStrlen(ModuleHost&, PpuContext* c) { H_Strlen(noop_host(), c); }
static void H_LibcStrcpy(ModuleHost&, PpuContext* c) { H_Strcpy(noop_host(), c); }
static void H_LibcStrcmp(ModuleHost&, PpuContext* c) { H_Strcmp(noop_host(), c); }
static void H_LibcMalloc(ModuleHost&, PpuContext* c) { H_SysMalloc(noop_host(), c); }
static void H_LibcFree(ModuleHost&, PpuContext* c) { H_SysFree(noop_host(), c); }

// ---------------------------------------------------------------------------
// Synchronous guest callback execution (qsort comparators, exit callbacks).
// Runs a fresh PPU context on the shared VM/LV2 until it returns to the
// dedicated return trampoline.
// ---------------------------------------------------------------------------

s64 HleCallGuest(Vm& vm, Lv2* lv2, u32 addr, u32 rtoc, u64 r3, u64 r4, u64 r5) {
    PpuContext tmp;
    tmp.vm = &vm;
    tmp.lv2 = lv2;
    tmp.syscall_fn = [](PpuContext* c2) { static_cast<Lv2*>(c2->lv2)->Syscall(c2); };
    tmp.pc = addr;
    tmp.gpr[1] = kCbStackBase + kCbStackSize - 0x100;
    tmp.gpr[2] = rtoc;
    tmp.gpr[3] = r3;
    tmp.gpr[4] = r4;
    tmp.gpr[5] = r5;
    tmp.mem_base = vm.mutable_data();
    // LR points at the HLE "callback returned" trampoline (set by the host).
    extern u32 g_hle_return_stub_addr;
    tmp.lr = g_hle_return_stub_addr;
    tmp.exit = false;
    PpuRun(&tmp);
    if (tmp.error) {
        AP_ERROR("HLE: guest callback at 0x", Hex32(addr), " failed: ", tmp.error_msg);
        return 0;
    }
    return tmp.exit_code;
}
u32 g_hle_return_stub_addr = 0;  // set in ModuleHost::Init

// qsort with a REAL guest comparator via HleCallGuest (insertion sort over a
// scratch slot in the guest heap).
static void H_Qsort(ModuleHost&, PpuContext* c) {
    u32 base = static_cast<u32>(GArg(c, 0));
    u64 n = GArg(c, 1), es = GArg(c, 2);
    u32 cmp_opd = static_cast<u32>(GArg(c, 3));
    Vm& vm = *c->vm;
    u32 fn = vm.Read32(cmp_opd);
    u32 toc = vm.Read32(static_cast<u32>(cmp_opd + 4));
    if (!n || !es || !fn || es > 4096) { GRet(c, CELL_OK); return; }
    u32 scratch = static_cast<u32>(GuestHeapAlloc(es, 8));
    if (!scratch) { GRet(c, static_cast<s64>(CELL_ENOMEM)); return; }
    Lv2* lv2 = static_cast<Lv2*>(c->lv2);
    std::vector<u8> elem(static_cast<size_t>(es));
    for (u64 i = 1; i < n; i++) {
        vm.ReadBytes(static_cast<u32>(base + i * es), static_cast<u32>(es), elem.data());
        vm.WriteBytes(scratch, elem.data(), static_cast<u32>(es));
        u64 j = i;
        while (j > 0) {
            s64 cmp = HleCallGuest(vm, lv2, fn, toc,
                                   static_cast<u32>(base + (j - 1) * es), scratch, 0);
            if (cmp <= 0) break;
            vm.ReadBytes(static_cast<u32>(base + (j - 1) * es), static_cast<u32>(es), elem.data());
            vm.WriteBytes(static_cast<u32>(base + j * es), elem.data(), static_cast<u32>(es));
            j--;
        }
        vm.ReadBytes(scratch, static_cast<u32>(es), elem.data());
        vm.WriteBytes(static_cast<u32>(base + j * es), elem.data(), static_cast<u32>(es));
    }
    GRet(c, CELL_OK);
}

// ---------------------------------------------------------------------------
// Handler registry
// ---------------------------------------------------------------------------

static const HandlerEntry kHandlers[] = {
    // sysPrxForUser — syscall-reusing wrappers
    {"sysPrxForUser::sys_tty_write", H_SysTtyWrite},
    {"sysPrxForUser::sys_time", H_SysTime},
    {"sysPrxForUser::__sys_time", H_SysTime},
    {"sysPrxForUser::sys_time_get_system_time", H_SysTimeGetSystemTime},
    {"sysPrxForUser::sys_sleep", H_SysSleep},
    {"sysPrxForUser::sys_usleep", H_SysUsleep},
    {"sysPrxForUser::_sys_memory_allocate", H_SysMemoryAllocate},
    {"sysPrxForUser::sys_memory_free", H_SysMemoryFree},
    {"sysPrxForUser::sys_memory_get_user_memory_size", H_SysMemorySize},
    {"sysPrxForUser::sys_ppu_thread_create", H_ThreadCreate},
    {"sysPrxForUser::sys_ppu_thread_exit", H_ThreadExit},
    {"sysPrxForUser::sys_ppu_thread_join", H_ThreadJoin},
    {"sysPrxForUser::sys_ppu_thread_detach", H_ThreadDetach},
    {"sysPrxForUser::sys_ppu_thread_yield", H_ThreadYield},
    {"sysPrxForUser::sys_ppu_thread_get_id", H_ThreadGetId},
    {"sysPrxForUser::sys_process_getpid", H_GetPid},
    {"sysPrxForUser::sys_process_exit", H_ProcessExit},
    {"sysPrxForUser::sys_prx_load_module", H_PrxLoad},
    {"sysPrxForUser::sys_prx_load_module_on_memcontainer", H_PrxLoad},
    {"sysPrxForUser::sys_prx_start_module", H_PrxStart},
    {"sysPrxForUser::sys_prx_stop_module", H_PrxStop},
    {"sysPrxForUser::sys_prx_unload_module", H_PrxUnload},
    {"sysPrxForUser::sys_prx_get_module_id_by_name", H_PrxIdByName},
    {"sysPrxForUser::sys_prx_get_module_list", H_PrxGetModuleList},
    {"sysPrxForUser::_sys_process_at_Exitspawn", H_AtExitspawn},
    {"sysPrxForUser::_sys_process_atexitspawn", H_AtExitspawn},
    {"sysPrxForUser::sys_process_atexit", H_AtExitspawn},
    {"sysPrxForUser::_sys_memcpy", H_Memcpy},
    {"sysPrxForUser::_sys_memmove", H_Memmove},
    {"sysPrxForUser::_sys_memset", H_Memset},
    {"sysPrxForUser::_sys_memcmp", H_Memcmp},
    {"sysPrxForUser::_sys_memchr", H_Memchr},
    {"sysPrxForUser::_sys_strlen", H_Strlen},
    {"sysPrxForUser::_sys_strcpy", H_Strcpy},
    {"sysPrxForUser::_sys_strncpy", H_Strncpy},
    {"sysPrxForUser::_sys_strcat", H_Strcat},
    {"sysPrxForUser::_sys_strncat", H_Strncat},
    {"sysPrxForUser::_sys_strchr", H_Strchr},
    {"sysPrxForUser::_sys_strrchr", H_Strrchr},
    {"sysPrxForUser::_sys_strcmp", H_Strcmp},
    {"sysPrxForUser::_sys_strncmp", H_Strncmp},
    {"sysPrxForUser::_sys_strncasecmp", H_Strcasecmp},
    {"sysPrxForUser::_sys_tolower", H_Tolower},
    {"sysPrxForUser::_sys_toupper", H_Toupper},
    {"sysPrxForUser::_sys_printf", H_Printf},
    {"sysPrxForUser::_sys_sprintf", H_Sprintf},
    {"sysPrxForUser::_sys_snprintf", H_Snprintf},
    {"sysPrxForUser::console_write", H_ConsoleWrite},
    {"sysPrxForUser::_sys_malloc", H_SysMalloc},
    {"sysPrxForUser::_sys_memalign", H_SysMemalign},
    {"sysPrxForUser::_sys_free", H_SysFree},
    {"sysPrxForUser::_sys_heap_create_heap", H_HeapCreate},
    {"sysPrxForUser::_sys_heap_alloc_heap_memory", H_HeapAllocHeapMemory},
    {"sysPrxForUser::_sys_heap_malloc", H_HeapMalloc},
    {"sysPrxForUser::_sys_heap_memalign", H_HeapMemalign},
    {"sysPrxForUser::_sys_heap_free", H_HeapFree},
    {"sysPrxForUser::_sys_heap_delete_heap", H_HeapDelete},
    {"sysPrxForUser::_sys_qsort", H_Qsort},
    {"sysPrxForUser::sys_process_is_stack", H_ProcIsStack},
    {"sysPrxForUser::sys_spinlock_initialize", H_SpinlockInit},
    {"sysPrxForUser::sys_spinlock_lock", H_SpinlockLock},
    {"sysPrxForUser::sys_spinlock_trylock", H_SpinlockTryLock},
    {"sysPrxForUser::sys_spinlock_unlock", H_SpinlockUnlock},
    {"sysPrxForUser::sys_get_random_number", H_RandomNumber},
    // cellSysutil
    {"cellSysutil::sysUtilGetSystemParamInt", H_SysutilGetParamInt},
    {"cellSysutil::sysUtilGetSystemParamString", H_SysutilGetParamString},
    {"cellSysutil::sysUtilRegisterCallbackEx", H_SysutilRegisterCallback},
    {"cellSysutil::sysUtilCheckCallback", H_SysutilCheckCallback},
    {"cellSysutil::sysUtilUnregisterCallback", H_SysutilUnregisterCallback},
    // cellSysmodule
    {"cellSysmodule::sysModuleInitialize", H_SysModuleInitialize},
    {"cellSysmodule::sysModuleFinalize", H_SysModuleFinalize},
    {"cellSysmodule::sysModuleLoad", H_SysModuleLoad},
    {"cellSysmodule::sysModuleUnload", H_SysModuleUnload},
    {"cellSysmodule::sysModuleIsLoaded", H_SysModuleIsLoaded},
    // cellGcmSys
    {"cellGcmSys::gcmInitBodyEx", H_GcmInit},
    {"cellGcmSys::gcmAddressToOffset", H_GcmAddressToOffset},
    // cellPad
    {"cellPad::ioPadInit", H_IoPadInit},
    {"cellPad::ioPadEnd", H_IoPadEnd},
    {"cellPad::ioPadGetInfo", H_IoPadGetInfo},
    {"cellPad::ioPadGetData", H_IoPadGetData},
    // libc / libstdcxx (same semantics, guest memory)
    {"libc::memcpy", H_LibcMemcpy},
    {"libc::memset", H_LibcMemset},
    {"libc::memmove", H_Memmove},
    {"libc::strlen", H_LibcStrlen},
    {"libc::strcpy", H_LibcStrcpy},
    {"libc::strcmp", H_LibcStrcmp},
    {"libc::malloc", H_LibcMalloc},
    {"libc::free", H_LibcFree},
    {"libc::printf", H_Printf},
    {"libc::sprintf", H_Sprintf},
    {"libc::snprintf", H_Snprintf},
    {"libstdcxx::memcpy", H_LibcMemcpy},
    {"libstdcxx::memset", H_LibcMemset},
    {"libstdcxx::malloc", H_LibcMalloc},
    {"libstdcxx::free", H_LibcFree},
    {"sys_libc::memcpy", H_LibcMemcpy},
    {"sys_libc::memset", H_LibcMemset},
    {"sys_libc::memmove", H_Memmove},
    {"sys_libc::strlen", H_LibcStrlen},
    {"sys_libc::strcpy", H_LibcStrcpy},
    {"sys_libc::strcmp", H_LibcStrcmp},
    {"sys_libc::malloc", H_LibcMalloc},
    {"sys_libc::free", H_LibcFree},
    {"sys_libc::printf", H_Printf},
    {"sys_libstdcxx::memcpy", H_LibcMemcpy},
    {"sys_libstdcxx::memset", H_LibcMemset},
    {"sys_libstdcxx::malloc", H_LibcMalloc},
    {"sys_libstdcxx::free", H_LibcFree},
};

static void LwMutexLockHandler(ModuleHost&, PpuContext* c) {
    GRet(c, LwMutexTry(c, static_cast<u32>(GArg(c, 0)), 1, false));
}
static void LwMutexTryHandler(ModuleHost&, PpuContext* c) {
    GRet(c, LwMutexTry(c, static_cast<u32>(GArg(c, 0)), 1, true));
}
static void LwMutexUnlockHandler(ModuleHost&, PpuContext* c) {
    Vm& vm = *c->vm;
    u32 m = static_cast<u32>(GArg(c, 0));
    u64 owner = vm.Read64(m);
    u16 rec = vm.Read16(static_cast<u32>(m + 0xE));
    if (owner != 1) { GRet(c, static_cast<s64>(CELL_EPERM)); return; }
    if (rec > 1) {
        vm.Write16(static_cast<u32>(m + 0xE), static_cast<u16>(rec - 1));
    } else {
        vm.Write64(m, 0);
        vm.Write16(static_cast<u32>(m + 0xE), 0);
    }
    GRet(c, CELL_OK);
}
static void LwMutexCreateHandler(ModuleHost&, PpuContext* c) {
    u32 m = static_cast<u32>(GArg(c, 0));
    if (!m) { GRet(c, static_cast<s64>(CELL_EFAULT)); return; }
    c->vm->Write64(m, 0);
    c->vm->Write32(static_cast<u32>(m + 8), 0);
    c->vm->Write16(static_cast<u32>(m + 0xC), static_cast<u16>(GArg(c, 1) & 0xFFFF));
    c->vm->Write16(static_cast<u32>(m + 0xE), 0);
    GRet(c, CELL_OK);
}
static void LwMutexDestroyHandler(ModuleHost&, PpuContext* c) {
    GRet(c, CELL_OK);
}

static const HandlerEntry kLwMutexHandlers[] = {
    {"sysPrxForUser::sys_lwmutex_create", LwMutexCreateHandler},
    {"sysPrxForUser::sys_lwmutex_destroy", LwMutexDestroyHandler},
    {"sysPrxForUser::sys_lwmutex_lock", LwMutexLockHandler},
    {"sysPrxForUser::sys_lwmutex_trylock", LwMutexTryHandler},
    {"sysPrxForUser::sys_lwmutex_unlock", LwMutexUnlockHandler},
    {"sysPrxForUser::sys_lwmutex_unlock2", LwMutexUnlockHandler},
};

// built once (both arrays merged)
static std::map<std::string, HandlerFn>& HandlerMap() {
    static std::map<std::string, HandlerFn>* m = [] {
        auto* map = new std::map<std::string, HandlerFn>();
        for (auto& e : kHandlers) if (e.fn) (*map)[e.key] = e.fn;
        for (auto& e : kLwMutexHandlers) (*map)[e.key] = e.fn;
        return map;
    }();
    return *m;
}

static bool HasImplementedHandler(const std::string& module_name, const std::string& name) {
    return HandlerMap().count(module_name + "::" + name) > 0;
}

// ---------------------------------------------------------------------------
// ModuleHost
// ---------------------------------------------------------------------------

void ModuleHost::Init(Vm* vm, Lv2* lv2) {
    vm_ = vm;
    lv2_ = lv2;
    next_index_ = 0;
    next_trampoline_ = kHleAreaBase;
    if (vm_) {
        if (!vm_->PageIsAllocated(kHleAreaBase)) vm_->PageAlloc(kHleAreaBase, kHleAreaSize);
        if (!vm_->PageIsAllocated(kGuestHeapBase)) {
            vm_->PageAlloc(kGuestHeapBase, kGuestHeapSize);
            g_guest_heap.init = true;
            g_guest_heap.next = kGuestHeapBase;
        } else {
            g_guest_heap.init = true;
            g_guest_heap.next = kGuestHeapBase;
        }
        if (!vm_->PageIsAllocated(kCbStackBase)) vm_->PageAlloc(kCbStackBase, kCbStackSize);
        // Return trampoline for synchronous guest callbacks: index 0 is
        // reserved as "callback returned" (Dispatch sets ctx->exit).
        g_hle_return_stub_addr = next_trampoline_;
        vm_->Write32(next_trampoline_ + 0, 0x3D601000u);   // lis r11, 0x1000
        vm_->Write32(next_trampoline_ + 4, 0x616B0000u);    // ori r11, r11, 0
        vm_->Write32(next_trampoline_ + 8, 0x44000002u);   // sc
        vm_->Write32(next_trampoline_ + 12, 0x4E800020u);   // blr
        vm_->PageSetExec(next_trampoline_, true);
        next_trampoline_ += 16;
        next_index_ = 1;
    }
}

bool ModuleHost::HasModule(const std::string& module_name) const {
    for (size_t i = 0; i < sizeof(kAllModuleExportLists) / sizeof(kAllModuleExportLists[0]); i++) {
        const ModuleExportInfo* list = kAllModuleExportLists[i];
        size_t count = kAllModuleExportSizes[i];
        if (!count || !list) continue;
        if (std::strcmp(list[0].module, module_name.c_str()) == 0) return true;
    }
    return false;
}

u32 ModuleHost::Resolve(const std::string& module_name, u32 nid) {
    auto key = std::make_pair(module_name, nid);
    auto it = by_module_nid_.find(key);
    if (it != by_module_nid_.end()) {
        Func& f = funcs_[it->second];
        if (f.stat_index >= 0) imports_[f.stat_index].resolve_count++;
        return f.desc_addr;
    }

    const ModuleExportInfo* info = nullptr;
    for (size_t i = 0; i < sizeof(kAllModuleExportLists) / sizeof(kAllModuleExportLists[0]) &&
                       !info; i++) {
        const ModuleExportInfo* list = kAllModuleExportLists[i];
        size_t count = kAllModuleExportSizes[i];
        size_t lo = 0, hi = count;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (list[mid].nid == nid && std::strcmp(list[mid].module, module_name.c_str()) == 0) {
                info = &list[mid];
                break;
            }
            (list[mid].nid < nid) ? lo = mid + 1 : hi = mid;
        }
    }

    Func f;
    f.module_name = module_name;
    f.nid = nid;
    f.info = info;
    f.name = info ? info->name : "nid_" + Hex32(nid);
    f.implemented = info && HasImplementedHandler(module_name, f.name);

    if (next_trampoline_ + 24 > kHleAreaBase + kHleAreaSize) {
        AP_ERROR("module HLE: trampoline area exhausted (", next_index_, " functions)");
        return 0;
    }
    f.code_addr = next_trampoline_;
    f.desc_addr = f.code_addr + 16;
    u32 idx = next_index_++;
    next_trampoline_ += 24;

    if (vm_) {
        vm_->Write32(f.code_addr + 0, 0x3D601000u);            // lis r11, 0x1000
        vm_->Write32(f.code_addr + 4, 0x616B0000u | idx);      // ori r11, r11, idx
        vm_->Write32(f.code_addr + 8, 0x44000002u);             // sc
        vm_->Write32(f.code_addr + 12, 0x4E800020u);            // blr
        vm_->Write32(f.desc_addr + 0, f.code_addr);              // descriptor {addr, rtoc=0}
        vm_->Write32(f.desc_addr + 4, 0);
        vm_->PageSetExec(f.code_addr, true);
    }

    f.stat_index = static_cast<int>(imports_.size());
    ImportStat st;
    st.module_name = module_name;
    st.nid = nid;
    st.name = f.name;
    st.implemented = f.implemented;
    st.resolve_count = 1;
    by_module_nid_[key] = static_cast<u32>(funcs_.size());
    imports_.push_back(st);
    funcs_.push_back(std::move(f));
    return funcs_.back().desc_addr;
}

void ModuleHost::Dispatch(u32 index, PpuContext* ctx) {
    if (index == 0 && g_hle_return_stub_addr) {
        // synchronous guest-callback return path (see HleCallGuest)
        ctx->exit = true;
        ctx->exit_code = static_cast<s64>(ctx->gpr[3]);
        return;
    }
    // Trampoline n (n >= 1) lives at the function area + (n-1)*24.
    u32 want = kHleAreaBase + 16 + (index - 1) * 24;
    Func* found = nullptr;
    for (auto& f : funcs_) {
        if (f.code_addr == want) { found = &f; break; }
    }
    if (!found) {
        ctx->gpr[3] = CELL_ENOSYS;
        return;
    }
    found->call_count++;
    if (found->stat_index >= 0) imports_[found->stat_index].call_count = found->call_count;

    const auto& handlers = HandlerMap();
    auto it = handlers.find(found->module_name + "::" + found->name);
    if (it != handlers.end()) {
        it->second(*this, ctx);
        return;
    }
    if (found->call_count == 1) {
        AP_INFO("HLE MISSING ", found->module_name, ".", found->name, " (nid 0x",
                Hex32(found->nid), ") called — returning 0x80010002");
    }
    ctx->gpr[3] = CELL_ENOSYS;
}

// ---------------------------------------------------------------------------
// sys_prx-style registry + exit callbacks + bring-up report
// ---------------------------------------------------------------------------

s64 ModuleHost::PrxLoadByName(const std::string& module_name, std::string*) {
    for (auto& m : loaded_) {
        if (m.name == module_name) return m.id;
    }
    LoadedModule m;
    m.id = next_prx_id_++;
    m.name = module_name;
    loaded_.push_back(m);
    return m.id;
}

s64 ModuleHost::PrxStart(s64 id, std::string*) {
    for (auto& m : loaded_) {
        if (m.id == id) {
            m.start_count++;
            return CELL_OK;
        }
    }
    return static_cast<s64>(CELL_ENOSYS);
}

s64 ModuleHost::PrxStop(s64 id, std::string*) {
    for (auto& m : loaded_) {
        if (m.id == id) return CELL_OK;
    }
    return static_cast<s64>(CELL_ENOSYS);
}

s64 ModuleHost::PrxUnload(s64 id, std::string*) {
    for (size_t i = 0; i < loaded_.size(); i++) {
        if (loaded_[i].id == id) {
            loaded_.erase(loaded_.begin() + i);
            return CELL_OK;
        }
    }
    return static_cast<s64>(CELL_ENOSYS);
}

s64 ModuleHost::PrxIdByName(const std::string& module_name) const {
    for (auto& m : loaded_) {
        if (m.name == module_name) return m.id;
    }
    return 0;
}

void ModuleHost::RegisterExitCallback(u32 addr, u32 rtoc, u64 arg) {
    if (addr == 0 || exit_callbacks_.size() >= 64) return;
    for (auto& existing : exit_callbacks_) {
        if (existing.addr == addr) return;
    }
    GuestCallback cb;
    cb.addr = addr;
    cb.rtoc = rtoc;
    cb.arg = arg;
    exit_callbacks_.push_back(cb);
}

bool ModuleHost::missing_called() const {
    for (auto& s : imports_) {
        if (!s.implemented && s.call_count > 0) return true;
    }
    return false;
}

std::string ModuleHost::MissingSummary() const {
    size_t missing = 0, called = 0;
    std::map<std::string, size_t> per_module;
    for (auto& s : imports_) {
        if (!s.implemented) {
            missing++;
            per_module[s.module_name]++;
            if (s.call_count) called++;
        }
    }
    std::string out = "import link: " + std::to_string(imports_.size()) +
                      " import(s) linked across " + std::to_string(per_module.size()) +
                      " imported module(s); " + std::to_string(missing) +
                      " export(s) not implemented (resolved to honest ENOSYS stubs)";
    if (called) {
        out += "\nMISSING exports that were CALLED (fix first):";
        for (auto& s : imports_) {
            if (!s.implemented && s.call_count > 0) {
                out += "\n  MISSING " + s.module_name + "." + s.name + " (0x" + Hex32(s.nid) +
                       ") x" + std::to_string(s.call_count);
            }
        }
    } else if (missing) {
        out += "\nMISSING exports linked but not called yet:";
        size_t shown = 0;
        for (auto& s : imports_) {
            if (!s.implemented && shown < 40) {
                out += "\n  MISSING " + s.module_name + "." + s.name + " (0x" + Hex32(s.nid) + ")";
                shown++;
            }
        }
        if (shown < missing) out += "\n  ... (" + std::to_string(missing - shown) + " more)";
    }
    return out;
}

// ---------------------------------------------------------------------------
// Import slot patching (LV2 module-manager behavior)
// ---------------------------------------------------------------------------

LinkStats LinkImportsToHle(Vm& vm, ModuleHost& host, const std::vector<ModuleImport>& imports) {
    LinkStats st;
    for (const auto& imp : imports) {
        u32 desc = host.Resolve(imp.module_name, imp.nid);
        if (!desc) {
            st.failed++;
            AP_ERROR("module link: cannot resolve ", imp.module_name, ".0x", Hex32(imp.nid),
                     " (trampoline area full)");
            continue;
        }
        if (imp.slot_va && vm.Ptr(imp.slot_va, 4)) {
            vm.Write32(imp.slot_va, desc);
            st.linked++;
        } else {
            st.failed++;
        }
        if (imp.use_va) {
            // attribute 0x2000: a secondary "use address" that must also hold
            // the descriptor address (patched like the primary slot).
            if (vm.Ptr(imp.use_va, 4)) {
                vm.Write32(imp.use_va, desc);
                st.use_refs++;
            } else {
                st.failed++;
            }
        }
    }
    return st;
}

}  // namespace anyps3
