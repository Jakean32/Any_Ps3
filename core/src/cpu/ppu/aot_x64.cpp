// Any_Ps3 — MiniAOT x86-64 emitter (see aot.h).
//
// ABI: BlockFn(ctx) receives the PpuContext* in RDI and keeps it there for the
// whole block (guest GPR slots are [rdi + off]). Guest memory accesses go
// through ctx->mem_base with inline bounds checks; faults set ctx->error and
// end the block. Syscalls: push rdi; call ctx->syscall_fn; pop rdi (stack
// stays 16-byte aligned per SysV). All used host regs are caller-saved.
//
// On non-x86-64 hosts this file compiles to fallback stubs (BlockCache always
// falls back to the interpreter; the LLVM backend is the production path there).
#include "anyps3/cpu/ppu/aot.h"

#include <sys/mman.h>

#include <cstddef>
#include <cstring>

#include "anyps3/log.h"

#if defined(__x86_64__) || defined(_M_X64)

namespace anyps3 {
namespace mini_aot {

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------
struct Emitter {
    std::vector<u8> c;

    void Emit8(u8 b) { c.push_back(b); }
    void Emit32(u32 v) {
        for (int i = 0; i < 4; i++) c.push_back(static_cast<u8>(v >> (i * 8)));
    }
    void Emit64(u64 v) {
        for (int i = 0; i < 8; i++) c.push_back(static_cast<u8>(v >> (i * 8)));
    }

    // rel32 label fixups
    struct Fixup {
        size_t at;      // position of the rel32 operand
        u32 label;
    };
    std::vector<Fixup> fixups;
    std::vector<std::pair<u32, size_t>> labels;  // label id -> code position

    u32 next_label = 1;
    u32 NewLabel() { return next_label++; }
    void Bind(u32 label) { labels.push_back({label, c.size()}); }
    void Jmp32ToLabel(u32 label) { Emit8(0xE9); fixups.push_back({c.size(), label}); Emit32(0); }
    void Jcc32ToLabel(u8 op, u32 label) {
        Emit8(0x0F);
        Emit8(op);
        fixups.push_back({c.size(), label});
        Emit32(0);
    }
    void Patch() {
        for (const auto& f : fixups) {
            u32 target = 0;
            for (const auto& l : labels) {
                if (l.first == f.label) target = static_cast<u32>(l.second);
            }
            u32 rel = target - static_cast<u32>(f.at + 4);
            for (int i = 0; i < 4; i++) c[f.at + i] = static_cast<u8>(rel >> (i * 8));
        }
    }
};

// Context field offsets (recomputed per compile so layout changes are safe).
struct Offsets {
    size_t gpr[32];
    size_t pc, ctr, lr, exit, error, error_pc, syscall_fn, mem_base, cond;
    Offsets() {
        for (int i = 0; i < 32; i++) gpr[i] = offsetof(PpuContext, gpr) + i * 8;
        pc = offsetof(PpuContext, pc);
        ctr = offsetof(PpuContext, ctr);
        lr = offsetof(PpuContext, lr);
        exit = offsetof(PpuContext, exit);
        error = offsetof(PpuContext, error);
        error_pc = offsetof(PpuContext, error_pc);
        syscall_fn = offsetof(PpuContext, syscall_fn);
        mem_base = offsetof(PpuContext, mem_base);
        cond = offsetof(PpuContext, cond);
    }
};

static bool FitsDisp8(size_t off) { return off < 128; }

// mov rax, [rdi+off]  : 48 8B 47 xx | 48 8B 87 id
static void MovRaxFromCtx(Emitter* e, size_t off) {
    e->Emit8(0x48); e->Emit8(0x8B);
    if (FitsDisp8(off)) { e->Emit8(0x47); e->Emit8(static_cast<u8>(off)); }
    else { e->Emit8(0x87); e->Emit32(static_cast<u32>(off)); }
}
// mov [rdi+off], rax  : 48 89 47 xx | 48 89 87 id
static void MovCtxFromRax(Emitter* e, size_t off) {
    e->Emit8(0x48); e->Emit8(0x89);
    if (FitsDisp8(off)) { e->Emit8(0x47); e->Emit8(static_cast<u8>(off)); }
    else { e->Emit8(0x87); e->Emit32(static_cast<u32>(off)); }
}
// mov rcx, [rdi+off]
static void MovRcxFromCtx(Emitter* e, size_t off) {
    e->Emit8(0x48); e->Emit8(0x8B);
    if (FitsDisp8(off)) { e->Emit8(0x4F); e->Emit8(static_cast<u8>(off)); }
    else { e->Emit8(0x8F); e->Emit32(static_cast<u32>(off)); }
}
// mov rdx, [rdi+off]
static void MovRdxFromCtx(Emitter* e, size_t off) {
    e->Emit8(0x48); e->Emit8(0x8B);
    if (FitsDisp8(off)) { e->Emit8(0x57); e->Emit8(static_cast<u8>(off)); }
    else { e->Emit8(0x97); e->Emit32(static_cast<u32>(off)); }
}
// mov dword [rdi+off], imm32 : C7 47 xx id | C7 87 id id id id id
static void MovCtxImm32(Emitter* e, size_t off, u32 imm) {
    e->Emit8(0xC7);
    if (FitsDisp8(off)) { e->Emit8(0x47); e->Emit8(static_cast<u8>(off)); }
    else { e->Emit8(0x87); e->Emit32(static_cast<u32>(off)); }
    e->Emit32(imm);
}
// mov qword [rdi+off], imm32 (sign-extended) : 48 C7 47 xx id | 48 C7 87 ...
static void MovCtxImm64From32(Emitter* e, size_t off, s32 imm) {
    e->Emit8(0x48); e->Emit8(0xC7);
    if (FitsDisp8(off)) { e->Emit8(0x47); e->Emit8(static_cast<u8>(off)); }
    else { e->Emit8(0x87); e->Emit32(static_cast<u32>(off)); }
    e->Emit32(static_cast<u32>(imm));
}
// add rax, imm32 : 48 81 C0 id   | imm8: 48 83 C0 ib
static void AddRaxImm(Emitter* e, s32 imm) {
    if (imm >= -128 && imm <= 127) {
        e->Emit8(0x48); e->Emit8(0x83); e->Emit8(0xC0); e->Emit8(static_cast<u8>(imm));
    } else {
        e->Emit8(0x48); e->Emit8(0x81); e->Emit8(0xC0); e->Emit32(static_cast<u32>(imm));
    }
}
// add rcx, imm32 : 48 81 C1 id
static void AddRcxImm(Emitter* e, s32 imm) {
    e->Emit8(0x48); e->Emit8(0x81); e->Emit8(0xC1); e->Emit32(static_cast<u32>(imm));
}
// add/or/and/xor rax, [rdi+off]: 48 03/0B/23/33 47 xx
static void AluRaxCtx(u8 op, Emitter* e, size_t off) {
    e->Emit8(0x48); e->Emit8(op);
    if (FitsDisp8(off)) { e->Emit8(0x47); e->Emit8(static_cast<u8>(off)); }
    else { e->Emit8(0x87); e->Emit32(static_cast<u32>(off)); }
}
static void MovRaxImm64(Emitter* e, u64 imm) {
    e->Emit8(0x48); e->Emit8(0xB8); e->Emit64(imm);
}
static void WritePc(Emitter* e, const Offsets& o, u32 target) { MovCtxImm32(e, o.pc, target); }

// Fault: set error, error_pc, jump to end label.
static void EmitFault(Emitter* e, const Offsets& o, u32 fault_pc, u32 end_label) {
    e->Emit8(0xC6);  // mov byte [rdi+off], 1
    e->Emit8(FitsDisp8(o.error) ? 0x47 : 0x87);
    if (FitsDisp8(o.error)) { e->Emit8(static_cast<u8>(o.error)); }
    else { e->Emit32(static_cast<u32>(o.error)); }
    e->Emit8(1);
    MovCtxImm32(e, o.error_pc, fault_pc);
    e->Jmp32ToLabel(end_label);
}

// (Bounds-check sequences are emitted inline in Compile().)

// ---------------------------------------------------------------------------
// Compiler
// ---------------------------------------------------------------------------
// mov eax, [rdi+off] (zero-extends to rax)
static void MovEaxFromCtx(Emitter* e, size_t off) {
    e->Emit8(0x8B);
    if (FitsDisp8(off)) { e->Emit8(0x47); e->Emit8(static_cast<u8>(off)); }
    else { e->Emit8(0x87); e->Emit32(static_cast<u32>(off)); }
}
// mov ecx, [rdi+off] (zero-extends to rcx)
static void MovEcxFromCtx(Emitter* e, size_t off) {
    e->Emit8(0x8B);
    if (FitsDisp8(off)) { e->Emit8(0x4F); e->Emit8(static_cast<u8>(off)); }
    else { e->Emit8(0x8F); e->Emit32(static_cast<u32>(off)); }
}
// mov [rdi+off], eax (zero-extends the 64-bit slot)
static void MovCtxFromEax(Emitter* e, size_t off) {
    e->Emit8(0x89);
    if (FitsDisp8(off)) { e->Emit8(0x47); e->Emit8(static_cast<u8>(off)); }
    else { e->Emit8(0x87); e->Emit32(static_cast<u32>(off)); }
}

Code Compile(const std::vector<AIROp>& ir) {
    Code out;
    if (ir.empty()) return out;
    Offsets o;
    Emitter e;
    u32 end_label = e.NewLabel();

    // SysV: incoming rsp%16==8. We push rdi exactly once before calls; that is
    // done per-syscall so rsp stays 16-aligned at each call site.

    auto cmpSetcc = [&e, &o](const AIROp& op) {
        // cond = compare(guest r[op.a] (32 or 64 bit, signed) vs op.imm)
        if (op.cmp64) {
            MovRaxFromCtx(&e, o.gpr[op.a]);
        } else {
            // mov eax, [rdi+off] (zero-extends), then movsxd rax, eax
            e.Emit8(0x8B);
            if (FitsDisp8(o.gpr[op.a])) { e.Emit8(0x47); e.Emit8(static_cast<u8>(o.gpr[op.a])); }
            else { e.Emit8(0x87); e.Emit32(static_cast<u32>(o.gpr[op.a])); }
            e.Emit8(0x48); e.Emit8(0x63); e.Emit8(0xC0);  // movsxd rax, eax
        }
        s32 imm = static_cast<s32>(static_cast<s64>(op.imm));
        e.Emit8(0x48); e.Emit8(0x81); e.Emit8(0xF8); e.Emit32(static_cast<u32>(imm));  // cmp rax, imm32
        u8 setop = 0x94;  // sete
        switch (op.cmp) {
            case CmpKind::Eq: setop = 0x94; break;
            case CmpKind::Ne: setop = 0x95; break;
            case CmpKind::Lt: setop = 0x9C; break;
            case CmpKind::Gt: setop = 0x9F; break;
            case CmpKind::Le: setop = 0x9E; break;
            case CmpKind::Ge: setop = 0x9D; break;
        }
        e.Emit8(0x0F); e.Emit8(setop); e.Emit8(0xC0);  // setcc al
        e.Emit8(0x88);                                  // mov [rdi+cond], al
        e.Emit8(FitsDisp8(o.cond) ? 0x47 : 0x87);
        if (FitsDisp8(o.cond)) { e.Emit8(static_cast<u8>(o.cond)); }
        else { e.Emit32(static_cast<u32>(o.cond)); }
    };

    for (const AIROp& op : ir) {
        switch (op.kind) {
            case IRKind::GprImm: {
                if (static_cast<s64>(op.imm) >= -0x80000000LL &&
                    static_cast<s64>(op.imm) <= 0x7FFFFFFFLL) {
                    MovCtxImm64From32(&e, o.gpr[op.dst], static_cast<s32>(op.imm));
                } else {
                    MovRaxImm64(&e, op.imm);
                    MovCtxFromRax(&e, o.gpr[op.dst]);
                }
                break;
            }
            case IRKind::GprAddImm: {
                MovRaxFromCtx(&e, o.gpr[op.a]);
                AddRaxImm(&e, static_cast<s32>(static_cast<s64>(op.imm)));
                MovCtxFromRax(&e, o.gpr[op.dst]);
                break;
            }
            case IRKind::AluAdd: {
                MovRaxFromCtx(&e, o.gpr[op.a]);
                AluRaxCtx(0x03, &e, o.gpr[op.b]);
                MovCtxFromRax(&e, o.gpr[op.dst]);
                break;
            }
            case IRKind::AluOr: {
                MovRaxFromCtx(&e, o.gpr[op.a]);
                AluRaxCtx(0x0B, &e, o.gpr[op.b]);
                MovCtxFromRax(&e, o.gpr[op.dst]);
                break;
            }
            case IRKind::AluAnd: {
                MovRaxFromCtx(&e, o.gpr[op.a]);
                AluRaxCtx(0x23, &e, o.gpr[op.b]);
                MovCtxFromRax(&e, o.gpr[op.dst]);
                break;
            }
            case IRKind::AluXor: {
                MovRaxFromCtx(&e, o.gpr[op.a]);
                AluRaxCtx(0x33, &e, o.gpr[op.b]);
                MovCtxFromRax(&e, o.gpr[op.dst]);
                break;
            }
            case IRKind::AluSub: {
                // dst = b - a
                MovRaxFromCtx(&e, o.gpr[op.b]);
                MovRcxFromCtx(&e, o.gpr[op.a]);
                e.Emit8(0x48); e.Emit8(0x2B); e.Emit8(0xC1);  // sub rax, rcx
                MovCtxFromRax(&e, o.gpr[op.dst]);
                break;
            }
            case IRKind::Load32: case IRKind::Load32s: case IRKind::Load64: {
                bool is64 = op.kind == IRKind::Load64;
                // address in rcx: mov rcx, [rdi+a]; add rcx, imm
                if (is64) {
                    MovRcxFromCtx(&e, o.gpr[op.a]);
                } else {
                    e.Emit8(0x8B);  // mov ecx, [rdi+off] (zero-extends)
                    if (FitsDisp8(o.gpr[op.a])) { e.Emit8(0x4F); e.Emit8(static_cast<u8>(o.gpr[op.a])); }
                    else { e.Emit8(0x8F); e.Emit32(static_cast<u32>(o.gpr[op.a])); }
                }
                if (op.imm != 0) AddRcxImm(&e, static_cast<s32>(static_cast<s64>(op.imm)));
                // bounds: lea edx,[rcx-0x10000]; cmp edx, max; ja fault
                e.Emit8(0x8D); e.Emit8(0x91); e.Emit32(0xFFFF0000u);  // lea edx,[rcx-0x10000]
                e.Emit8(0x81); e.Emit8(0xFA);
                e.Emit32(0x1FFF0000u - (is64 ? 8u : 4u));            // cmp edx, max
                u32 fault_label = e.NewLabel();
                e.Jcc32ToLabel(0x87, fault_label);                   // ja fault
                // ok path
                u32 done_label = e.NewLabel();
                MovRdxFromCtx(&e, o.mem_base);
                if (is64) {
                    e.Emit8(0x48); e.Emit8(0x8B); e.Emit8(0x04); e.Emit8(0x0A);  // mov rax,[rdx+rcx]
                    e.Emit8(0x48); e.Emit8(0x0F); e.Emit8(0xC8);                  // bswap rax
                    MovCtxFromRax(&e, o.gpr[op.dst]);
                } else {
                    e.Emit8(0x8B); e.Emit8(0x04); e.Emit8(0x0A);                  // mov eax,[rdx+rcx]
                    e.Emit8(0x0F); e.Emit8(0xC8);                                 // bswap eax
                    if (op.kind == IRKind::Load32s) {
                        e.Emit8(0x48); e.Emit8(0x63); e.Emit8(0xC0);              // movsxd rax, eax
                        MovCtxFromRax(&e, o.gpr[op.dst]);
                    } else {
                        // mov [rdi+dst], eax (zero-extends to 64)
                        e.Emit8(0x89);
                        if (FitsDisp8(o.gpr[op.dst])) { e.Emit8(0x47); e.Emit8(static_cast<u8>(o.gpr[op.dst])); }
                        else { e.Emit8(0x87); e.Emit32(static_cast<u32>(o.gpr[op.dst])); }
                    }
                }
                e.Jmp32ToLabel(done_label);
                e.Bind(fault_label);
                EmitFault(&e, o, op.pc, end_label);
                e.Bind(done_label);
                break;
            }
            case IRKind::Store32: case IRKind::Store64: {
                bool is64 = op.kind == IRKind::Store64;
                if (is64) {
                    MovRaxFromCtx(&e, o.gpr[op.dst]);      // value
                    MovRcxFromCtx(&e, o.gpr[op.a]);        // address
                } else {
                    e.Emit8(0x8B);  // mov eax, [rdi+dst] (value, low 32)
                    if (FitsDisp8(o.gpr[op.dst])) { e.Emit8(0x47); e.Emit8(static_cast<u8>(o.gpr[op.dst])); }
                    else { e.Emit8(0x87); e.Emit32(static_cast<u32>(o.gpr[op.dst])); }
                    e.Emit8(0x8B);  // mov ecx, [rdi+a]
                    if (FitsDisp8(o.gpr[op.a])) { e.Emit8(0x4F); e.Emit8(static_cast<u8>(o.gpr[op.a])); }
                    else { e.Emit8(0x8F); e.Emit32(static_cast<u32>(o.gpr[op.a])); }
                }
                if (op.imm != 0) AddRcxImm(&e, static_cast<s32>(static_cast<s64>(op.imm)));
                e.Emit8(0x8D); e.Emit8(0x91); e.Emit32(0xFFFF0000u);  // lea edx,[rcx-0x10000]
                e.Emit8(0x81); e.Emit8(0xFA);
                e.Emit32(0x1FFF0000u - (is64 ? 8u : 4u));
                u32 fault_label = e.NewLabel();
                e.Jcc32ToLabel(0x87, fault_label);
                u32 done_label = e.NewLabel();
                MovRdxFromCtx(&e, o.mem_base);
                if (is64) {
                    e.Emit8(0x48); e.Emit8(0x0F); e.Emit8(0xC8);                  // bswap rax
                    e.Emit8(0x48); e.Emit8(0x89); e.Emit8(0x04); e.Emit8(0x0A);    // mov [rdx+rcx], rax
                } else {
                    e.Emit8(0x0F); e.Emit8(0xC8);                                   // bswap eax
                    e.Emit8(0x89); e.Emit8(0x04); e.Emit8(0x0A);                   // mov [rdx+rcx], eax
                }
                e.Jmp32ToLabel(done_label);
                e.Bind(fault_label);
                EmitFault(&e, o, op.pc, end_label);
                e.Bind(done_label);
                break;
            }
            case IRKind::AluMulW: {
                MovEaxFromCtx(&e, o.gpr[op.a]);
                e.Emit8(0x48); e.Emit8(0x63); e.Emit8(0xC0);  // movsxd rax, eax
                MovEcxFromCtx(&e, o.gpr[op.b]);
                e.Emit8(0x48); e.Emit8(0x63); e.Emit8(0xC9);  // movsxd rcx, ecx
                e.Emit8(0x48); e.Emit8(0x0F); e.Emit8(0xAF); e.Emit8(0xC1);  // imul rax, rcx
                MovCtxFromRax(&e, o.gpr[op.dst]);
                break;
            }
            case IRKind::AluSar64Imm: {
                MovRaxFromCtx(&e, o.gpr[op.a]);
                e.Emit8(0x48); e.Emit8(0xC1); e.Emit8(0xF8);  // sar rax, imm8
                e.Emit8(static_cast<u8>(op.b));
                MovCtxFromRax(&e, o.gpr[op.dst]);
                break;
            }
            case IRKind::Rlwinm: {
                MovEaxFromCtx(&e, o.gpr[op.a]);
                e.Emit8(0xC1); e.Emit8(0xC0);  // rol eax, imm8
                e.Emit8(static_cast<u8>(op.b));
                e.Emit8(0x25); e.Emit32(static_cast<u32>(op.imm));  // and eax, mask
                MovCtxFromEax(&e, o.gpr[op.dst]);
                break;
            }
            case IRKind::AluOrImm: {
                MovRaxFromCtx(&e, o.gpr[op.a]);
                e.Emit8(0x48); e.Emit8(0x81); e.Emit8(0xC8);  // or rax, imm32 (sign-ext)
                e.Emit32(static_cast<u32>(op.imm));
                MovCtxFromRax(&e, o.gpr[op.dst]);
                break;
            }
            case IRKind::MtCtr: {
                MovRaxFromCtx(&e, o.gpr[op.dst]);
                MovCtxFromRax(&e, o.ctr);
                break;
            }
            case IRKind::MtLr: {
                MovRaxFromCtx(&e, o.gpr[op.dst]);
                MovCtxFromRax(&e, o.lr);
                break;
            }
            case IRKind::Syscall: {
                e.Emit8(0x57);  // push rdi            (rsp%16: 8 -> 0)
                MovRaxFromCtx(&e, o.syscall_fn);
                e.Emit8(0xFF); e.Emit8(0xD0);  // call rax
                e.Emit8(0x5F);                  // pop rdi
                // if (ctx->exit || ctx->error) end block
                e.Emit8(0x80);  // cmp byte [rdi+exit], 0
                e.Emit8(FitsDisp8(o.exit) ? 0x7F : 0xBF);
                if (FitsDisp8(o.exit)) { e.Emit8(static_cast<u8>(o.exit)); }
                else { e.Emit32(static_cast<u32>(o.exit)); }
                e.Emit8(0x00);
                e.Jcc32ToLabel(0x85, end_label);  // jne end
                e.Emit8(0x80);  // cmp byte [rdi+error], 0
                e.Emit8(FitsDisp8(o.error) ? 0x7F : 0xBF);
                if (FitsDisp8(o.error)) { e.Emit8(static_cast<u8>(o.error)); }
                else { e.Emit32(static_cast<u32>(o.error)); }
                e.Emit8(0x00);
                e.Jcc32ToLabel(0x85, end_label);  // jne end
                break;
            }
            case IRKind::BranchAlways: {
                WritePc(&e, o, op.target);
                e.Jmp32ToLabel(end_label);
                break;
            }
            case IRKind::BranchCond: {
                cmpSetcc(op);
                // test cond byte, branch if == polarity
                e.Emit8(0x80);  // cmp byte [rdi+cond], polarity
                e.Emit8(FitsDisp8(o.cond) ? 0x7F : 0xBF);
                if (FitsDisp8(o.cond)) { e.Emit8(static_cast<u8>(o.cond)); }
                else { e.Emit32(static_cast<u32>(o.cond)); }
                e.Emit8(op.polarity ? 1 : 0);
                u32 taken = e.NewLabel();
                e.Jcc32ToLabel(0x84, taken);  // je taken (cond == polarity)
                WritePc(&e, o, op.fallthrough);
                e.Jmp32ToLabel(end_label);
                e.Bind(taken);
                WritePc(&e, o, op.target);
                e.Jmp32ToLabel(end_label);
                break;
            }
            case IRKind::BranchCtr: {
                MovRaxFromCtx(&e, o.ctr);
                e.Emit8(0x48); e.Emit8(0xFF); e.Emit8(0xC8);  // dec rax
                MovCtxFromRax(&e, o.ctr);
                e.Emit8(0x48); e.Emit8(0x85); e.Emit8(0xC0);  // test rax, rax
                u32 taken = e.NewLabel();
                e.Jcc32ToLabel(op.polarity ? 0x84 : 0x85, taken);  // je (bdz) / jne (bdnz)
                WritePc(&e, o, op.fallthrough);
                e.Jmp32ToLabel(end_label);
                e.Bind(taken);
                WritePc(&e, o, op.target);
                e.Jmp32ToLabel(end_label);
                break;
            }
            default:
                return out;  // unsupported IR -> caller falls back
        }
    }

    e.Bind(end_label);
    e.Emit8(0xC3);  // ret
    e.Patch();

    // Allocate RW, copy, flip to RX (W^X).
    size_t page = 4096;
    size_t sz = (e.c.size() + page - 1) & ~(page - 1);
    void* mem = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return out;
    std::memcpy(mem, e.c.data(), e.c.size());
    if (mprotect(mem, sz, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, sz);
        return out;
    }
    out.fn = reinterpret_cast<BlockFn>(mem);
    out.mem = mem;
    out.size = sz;
    out.emitted = e.c.size();
    return out;
}

void Release(Code* c) {
    if (c->mem) {
        mprotect(c->mem, c->size, PROT_READ | PROT_WRITE);
        munmap(c->mem, c->size);
    }
    *c = Code{};
}

BlockFn BlockCache::GetOrCompile(u32 pc) {
    auto fb = fallbacks_.find(pc);
    if (fb != fallbacks_.end()) return nullptr;  // known-unlift-able
    auto it = blocks_.find(pc);
    if (it != blocks_.end()) return it->second.fn();

    std::vector<AIROp> ir;
    if (!PpuLiftBlock(vm_, pc, 128, &ir)) {
        fallbacks_[pc] = true;
        return nullptr;
    }
    Code c = Compile(ir);
    if (!c.fn) {
        fallbacks_[pc] = true;
        return nullptr;
    }
    CodeHolder holder(c);
    BlockFn fn = holder.fn();
    blocks_.emplace(pc, std::move(holder));
    return fn;
}

void BlockCache::InvalidateRange(u32 lo, u32 hi) {
    for (auto it = blocks_.begin(); it != blocks_.end();) {
        it = (it->first >= lo && it->first < hi) ? blocks_.erase(it) : std::next(it);
    }
    for (auto it = fallbacks_.begin(); it != fallbacks_.end();) {
        it = (it->first >= lo && it->first < hi) ? fallbacks_.erase(it) : std::next(it);
    }
}

int RunAot(PpuContext* ctx, BlockCache& cache) {
    u64 block_budget = 200000000ULL;  // safety valve for runaway guests
    while (!ctx->exit && !ctx->error) {
        if (block_budget-- == 0) {
            PpuRaise(ctx, "AOT block budget exceeded");
            break;
        }
        BlockFn fn = cache.GetOrCompile(ctx->pc);
        if (fn) {
            fn(ctx);
        } else {
            // Interpreter fallback for blocks the lifter can't handle.
            if (!PpuStep(ctx)) break;
        }
    }
    return static_cast<int>(ctx->exit_code);
}

}  // namespace mini_aot
}  // namespace anyps3

#else  // non-x86-64 host: everything falls back to the interpreter

namespace anyps3 {
namespace mini_aot {

Code Compile(const std::vector<AIROp>& ir) {
    (void)ir;
    return Code{};
}
void Release(Code* c) { *c = Code{}; }

BlockFn BlockCache::GetOrCompile(u32 pc) {
    fallbacks_[pc] = true;
    return nullptr;
}
void BlockCache::InvalidateRange(u32, u32) {}

int RunAot(PpuContext* ctx, BlockCache&) {
    return PpuRun(ctx);
}

}  // namespace mini_aot
}  // namespace anyps3

#endif  // __x86_64__ || _M_X64
