// Any_Ps3 — LLVM AOT backend (AOT-IR -> LLVM IR -> native code via LLVM-C).
//
// Delivers the TASK-2 milestone-1 goal "PPC64 -> LLVM IR lifting with AOT
// output (arm64 + x86-64)":
//   * LlvmEmitAot(): builds one LLVM module with one function per guest basic
//     block, writes human-readable LLVM IR (.ll) and emits native OBJECT
//     files for x86-64 AND aarch64 — real AOT artifacts verifiable with
//     llvm-objdump / llvm-nm.
//   * CompileLlvmBlock(): the same IR compiled + executed in-process through
//     ORC LLJIT — used by the tests to prove the lifted code runs natively
//     with identical semantics to the interpreter (ctx->icount stays 0).
//
// Contract identical to MiniAOT: `void block(PpuContext*)` — ctx as i8* with
// the same field offsets; unsupported IR ops make the caller fall back to the
// interpreter (honest AOT + fallback split).
#ifdef ANYPS3_WITH_LLVM

#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "anyps3/cpu/ppu/aot.h"
#include "anyps3/cpu/ppu/state.h"

namespace anyps3 {
namespace mini_aot {

namespace {

struct LlvmGlobals {
    LLVMContextRef ctx = nullptr;  // context for AOT emission modules
    LLVMOrcLLJITRef jit = nullptr;
    std::once_flag once;
    bool ok = false;
};
LlvmGlobals g;

// Per-context type set: the JIT path must build modules inside the
// ThreadSafeContext (mixing contexts is undefined behavior in ORC).
struct Types {
    LLVMTypeRef i8, i32, i64, i8p, void_t, fty;
};
Types MakeTypes(LLVMContextRef c) {
    Types t;
    t.i8 = LLVMInt8TypeInContext(c);
    t.i32 = LLVMInt32TypeInContext(c);
    t.i64 = LLVMInt64TypeInContext(c);
    t.i8p = LLVMPointerTypeInContext(c, 0);
    t.void_t = LLVMVoidTypeInContext(c);
    t.fty = LLVMFunctionType(t.void_t, &t.i8p, 1, 0);
    return t;
}
Types T;  // set per module build (single-threaded build per module)

void InitLlvm() {
    std::call_once(g.once, [] {
        LLVMInitializeNativeTarget();
        LLVMInitializeNativeAsmPrinter();
        // Cross-target AOT object emission (x86-64 + aarch64).
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
        g.ctx = LLVMContextCreate();
        LLVMOrcLLJITBuilderRef builder = LLVMOrcCreateLLJITBuilder();
        if (LLVMOrcCreateLLJIT(&g.jit, builder) == LLVMErrorSuccess) g.ok = true;
    });
}

LLVMValueRef CtxFieldPtr(LLVMBuilderRef b, LLVMValueRef ctx, size_t off, LLVMTypeRef ty) {
    LLVMValueRef idx = LLVMConstInt(T.i64, off, 0);
    LLVMValueRef gep = LLVMBuildGEP2(b, T.i8, ctx, &idx, 1, "f");
    return LLVMBuildPointerCast(b, gep, ty, "fp");
}

LLVMIntPredicate CmpToLLVM(CmpKind k) {
    switch (k) {
        case CmpKind::Eq: return LLVMIntEQ;
        case CmpKind::Ne: return LLVMIntNE;
        case CmpKind::Lt: return LLVMIntSLT;
        case CmpKind::Gt: return LLVMIntSGT;
        case CmpKind::Le: return LLVMIntSLE;
        case CmpKind::Ge: return LLVMIntSGE;
    }
    return LLVMIntEQ;
}

struct BlockBuilder {
    LLVMModuleRef mod;
    LLVMValueRef fn;
    LLVMBuilderRef b;
    LLVMValueRef ctx;

    LLVMTypeRef i64p;
    LLVMTypeRef i32p;

    LLVMValueRef gprp(int r) {
        return CtxFieldPtr(b, ctx, offsetof(PpuContext, gpr) + r * 8, i64p);
    }
    LLVMValueRef fld8p(size_t off) { return CtxFieldPtr(b, ctx, off, LLVMPointerType(T.i8, 0)); }
    LLVMValueRef fld32p(size_t off) { return CtxFieldPtr(b, ctx, off, LLVMPointerType(T.i32, 0)); }
    LLVMValueRef fld64p(size_t off) { return CtxFieldPtr(b, ctx, off, i64p); }
    LLVMValueRef membasep() {
        return CtxFieldPtr(b, ctx, offsetof(PpuContext, mem_base), LLVMPointerType(T.i8p, 0));
    }

    // Declared once per module; returns the bswap intrinsic for 32/64-bit ints.
    LLVMValueRef bswap32_fn;
    LLVMValueRef bswap64_fn;

    void write_pc(u32 v) {
        LLVMBuildStore(b, LLVMConstInt(T.i32, v, 0), fld32p(offsetof(PpuContext, pc)));
    }
};

LLVMValueRef CallBswap32(BlockBuilder& B, LLVMValueRef v) {
    LLVMValueRef args[1] = {v};
    return LLVMBuildCall2(B.b, LLVMFunctionType(T.i32, &T.i32, 1, 0), B.bswap32_fn, args, 1,
                          "bs32");
}
LLVMValueRef CallBswap64(BlockBuilder& B, LLVMValueRef v) {
    LLVMValueRef args[1] = {v};
    return LLVMBuildCall2(B.b, LLVMFunctionType(T.i64, &T.i64, 1, 0), B.bswap64_fn, args, 1,
                          "bs64");
}

// Guest pointer with bounds check. Emits a fault path (sets error/error_pc and
// returns) exactly like MiniAOT: rel = addr - kGuestMemBase; fault if rel >
// (kGuestMemEnd - kGuestMemBase - size). Leaves the builder positioned in the
// continuation block with `gp` = i8* to the guest memory.
LLVMValueRef GuestPtrChecked(BlockBuilder& B, LLVMValueRef addr32, u32 size, u32 fault_pc) {
    LLVMValueRef rel = LLVMBuildSub(B.b, addr32, LLVMConstInt(T.i32, kGuestMemBase, 0), "rel");
    LLVMValueRef max = LLVMConstInt(T.i32, kGuestMemEnd - kGuestMemBase - size, 0);
    LLVMValueRef in_range = LLVMBuildICmp(B.b, LLVMIntULE, rel, max, "in_range");
    LLVMBasicBlockRef cont = LLVMAppendBasicBlockInContext(g.ctx, B.fn, "cont");
    LLVMBasicBlockRef fault = LLVMAppendBasicBlockInContext(g.ctx, B.fn, "fault");
    LLVMBuildCondBr(B.b, in_range, cont, fault);
    LLVMPositionBuilderAtEnd(B.b, fault);
    LLVMBuildStore(B.b, LLVMConstInt(T.i8, 1, 0), B.fld8p(offsetof(PpuContext, error)));
    LLVMBuildStore(B.b, LLVMConstInt(T.i32, fault_pc, 0),
                   B.fld32p(offsetof(PpuContext, error_pc)));
    LLVMBuildRetVoid(B.b);
    LLVMPositionBuilderAtEnd(B.b, cont);
    LLVMValueRef base = LLVMBuildLoad2(B.b, T.i8p, B.membasep(), "mem_base");
    LLVMValueRef a64 = LLVMBuildZExt(B.b, addr32, T.i64, "a64");
    LLVMValueRef gp = LLVMBuildGEP2(B.b, T.i8, base, &a64, 1, "gp");
    return gp;
}

// Emits one IR op. Returns false when the op is unsupported (caller falls back
// to the interpreter for that block).
bool EmitOp(BlockBuilder& B, const AIROp& op) {
    switch (op.kind) {
        case IRKind::GprImm:
            LLVMBuildStore(B.b, LLVMConstInt(T.i64, op.imm, 1), B.gprp(op.dst));
            return true;
        case IRKind::GprMov: {
            LLVMValueRef v = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "");
            LLVMBuildStore(B.b, v, B.gprp(op.dst));
            return true;
        }
        case IRKind::GprAddImm: {
            LLVMValueRef v = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "");
            v = LLVMBuildAdd(B.b, v, LLVMConstInt(T.i64, op.imm, 1), "");
            LLVMBuildStore(B.b, v, B.gprp(op.dst));
            return true;
        }
        case IRKind::AluAdd: case IRKind::AluOr: case IRKind::AluXor:
        case IRKind::AluAnd: {
            LLVMValueRef a = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "");
            LLVMValueRef v2 = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.b), "");
            LLVMValueRef r = op.kind == IRKind::AluAdd ? LLVMBuildAdd(B.b, a, v2, "")
                            : op.kind == IRKind::AluOr ? LLVMBuildOr(B.b, a, v2, "")
                            : op.kind == IRKind::AluXor ? LLVMBuildXor(B.b, a, v2, "")
                                                        : LLVMBuildAnd(B.b, a, v2, "");
            LLVMBuildStore(B.b, r, B.gprp(op.dst));
            return true;
        }
        case IRKind::AluSub: {  // dst = b - a
            LLVMValueRef x = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.b), "");
            LLVMValueRef y = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "");
            LLVMBuildStore(B.b, LLVMBuildSub(B.b, x, y, ""), B.gprp(op.dst));
            return true;
        }
        case IRKind::AluOrImm: {
            LLVMValueRef v = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "");
            v = LLVMBuildOr(B.b, v, LLVMConstInt(T.i64, op.imm, 1), "");
            LLVMBuildStore(B.b, v, B.gprp(op.dst));
            return true;
        }
        case IRKind::AluMulW: {  // dst = (s64)(s32)a * (s32)b
            LLVMValueRef a = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "");
            LLVMValueRef v2 = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.b), "");
            a = LLVMBuildTrunc(B.b, a, T.i32, "a32");
            v2 = LLVMBuildTrunc(B.b, v2, T.i32, "b32");
            LLVMValueRef m = LLVMBuildMul(B.b, a, v2, "m32");
            LLVMBuildStore(B.b, LLVMBuildSExt(B.b, m, T.i64, "m64"), B.gprp(op.dst));
            return true;
        }
        case IRKind::AluSar64Imm: {
            LLVMValueRef v = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "");
            v = LLVMBuildAShr(B.b, v, LLVMConstInt(T.i64, op.b, 0), "");
            LLVMBuildStore(B.b, v, B.gprp(op.dst));
            return true;
        }
        case IRKind::Rlwinm: {  // dst = zext(rol32(a, sh) & mask); sh = op.b
            LLVMValueRef a = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "");
            a = LLVMBuildTrunc(B.b, a, T.i32, "a32");
            u32 sh = static_cast<u32>(op.b) & 31u;
            LLVMValueRef shl = LLVMBuildShl(B.b, a, LLVMConstInt(T.i32, sh, 0), "shl");
            LLVMValueRef lshr = LLVMBuildLShr(B.b, a, LLVMConstInt(T.i32, (32 - sh) & 31, 0), "lhr");
            LLVMValueRef rot = LLVMBuildOr(B.b, shl, lshr, "rot");
            rot = LLVMBuildAnd(B.b, rot, LLVMConstInt(T.i32, static_cast<u32>(op.imm), 0), "masked");
            LLVMBuildStore(B.b, LLVMBuildZExt(B.b, rot, T.i64, "r64"), B.gprp(op.dst));
            return true;
        }
        case IRKind::MtCtr: {
            LLVMValueRef v = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.dst), "");
            LLVMBuildStore(B.b, v, B.fld64p(offsetof(PpuContext, ctr)));
            return true;
        }
        case IRKind::MtLr: {
            LLVMValueRef v = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.dst), "");
            LLVMBuildStore(B.b, v, B.fld64p(offsetof(PpuContext, lr)));
            return true;
        }
        case IRKind::Load32: case IRKind::Load32s: case IRKind::Load64: {
            u32 size = op.kind == IRKind::Load64 ? 8 : 4;
            LLVMValueRef base = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "base");
            LLVMValueRef addr32 = LLVMBuildTrunc(
                B.b, LLVMBuildAdd(B.b, base, LLVMConstInt(T.i64, op.imm, 1), "addr"), T.i32,
                "a32");
            LLVMValueRef gp = GuestPtrChecked(B, addr32, size, op.pc);
            if (op.kind == IRKind::Load64) {
                LLVMValueRef v = LLVMBuildLoad2(B.b, T.i64, gp, "raw");
                v = CallBswap64(B, v);
                LLVMBuildStore(B.b, v, B.gprp(op.dst));
            } else {
                LLVMValueRef v = LLVMBuildLoad2(B.b, T.i32, gp, "raw");
                v = CallBswap32(B, v);
                if (op.kind == IRKind::Load32s) {
                    LLVMBuildStore(B.b, LLVMBuildSExt(B.b, v, T.i64, "s64"), B.gprp(op.dst));
                } else {
                    LLVMBuildStore(B.b, LLVMBuildZExt(B.b, v, T.i64, "z64"), B.gprp(op.dst));
                }
            }
            return true;
        }
        case IRKind::Store32: case IRKind::Store64: {
            u32 size = op.kind == IRKind::Store64 ? 8 : 4;
            LLVMValueRef base = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "base");
            LLVMValueRef addr32 = LLVMBuildTrunc(
                B.b, LLVMBuildAdd(B.b, base, LLVMConstInt(T.i64, op.imm, 1), "addr"), T.i32,
                "a32");
            LLVMValueRef gp = GuestPtrChecked(B, addr32, size, op.pc);
            LLVMValueRef src = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.dst), "src");
            if (op.kind == IRKind::Store64) {
                LLVMBuildStore(B.b, CallBswap64(B, src), gp);
            } else {
                LLVMValueRef s32 = LLVMBuildTrunc(B.b, src, T.i32, "s32");
                LLVMBuildStore(B.b, CallBswap32(B, s32), gp);
            }
            return true;
        }
        case IRKind::Syscall: {
            LLVMTypeRef fptr_ty = LLVMPointerType(T.void_t, 0);
            LLVMValueRef fptr = LLVMBuildLoad2(
                B.b, fptr_ty,
                CtxFieldPtr(B.b, B.ctx, offsetof(PpuContext, syscall_fn), fptr_ty), "syscallfn");
            LLVMValueRef args[1] = {B.ctx};
            LLVMBuildCall2(B.b, T.fty, fptr, args, 1, "");
            LLVMValueRef exited = LLVMBuildLoad2(B.b, T.i8,
                                                 B.fld8p(offsetof(PpuContext, exit)), "exited");
            LLVMValueRef done = LLVMBuildICmp(B.b, LLVMIntNE, exited,
                                              LLVMConstInt(T.i8, 0, 0), "done");
            LLVMBasicBlockRef cont = LLVMAppendBasicBlockInContext(g.ctx, B.fn, "cont");
            LLVMBasicBlockRef stop = LLVMAppendBasicBlockInContext(g.ctx, B.fn, "stop");
            LLVMBuildCondBr(B.b, done, stop, cont);
            LLVMPositionBuilderAtEnd(B.b, stop);
            LLVMBuildRetVoid(B.b);
            LLVMPositionBuilderAtEnd(B.b, cont);
            return true;
        }
        case IRKind::BranchAlways:
            B.write_pc(op.target);
            LLVMBuildRetVoid(B.b);
            return true;
        case IRKind::BranchCond: {
            // The compare is embedded in the op (cmpwi/cmpdi peephole).
            LLVMValueRef a = LLVMBuildLoad2(B.b, T.i64, B.gprp(op.a), "");
            LLVMValueRef imm = LLVMConstInt(T.i64, op.imm, 1);
            if (!op.cmp64) {
                a = LLVMBuildTrunc(B.b, a, T.i32, "a32");
                imm = LLVMBuildTrunc(B.b, imm, T.i32, "i32");
            }
            LLVMValueRef c = LLVMBuildICmp(B.b, CmpToLLVM(op.cmp), a, imm, "cmp");
            if (!op.polarity) c = LLVMBuildNot(B.b, c, "nc");
            LLVMBasicBlockRef tgt = LLVMAppendBasicBlockInContext(g.ctx, B.fn, "taken");
            LLVMBasicBlockRef fall = LLVMAppendBasicBlockInContext(g.ctx, B.fn, "fall");
            LLVMBuildCondBr(B.b, c, tgt, fall);
            LLVMPositionBuilderAtEnd(B.b, tgt);
            B.write_pc(op.target);
            LLVMBuildRetVoid(B.b);
            LLVMPositionBuilderAtEnd(B.b, fall);
            B.write_pc(op.fallthrough);
            LLVMBuildRetVoid(B.b);
            return true;
        }
        case IRKind::BranchCtr: {
            LLVMValueRef ctr = LLVMBuildLoad2(B.b, T.i64, B.fld64p(offsetof(PpuContext, ctr)),
                                              "ctr");
            LLVMValueRef dec = LLVMBuildSub(B.b, ctr, LLVMConstInt(T.i64, 1, 0), "dec");
            LLVMBuildStore(B.b, dec, B.fld64p(offsetof(PpuContext, ctr)));
            // lift.cpp contract: polarity=true is bdz (branch when ctr==0),
            // polarity=false is bdnz (branch when ctr!=0). Matches MiniAOT.
            LLVMValueRef c = LLVMBuildICmp(
                B.b, op.polarity ? LLVMIntEQ : LLVMIntNE, dec,
                LLVMConstInt(T.i64, 0, 0), "cctr");
            LLVMBasicBlockRef tgt = LLVMAppendBasicBlockInContext(g.ctx, B.fn, "taken");
            LLVMBasicBlockRef fall = LLVMAppendBasicBlockInContext(g.ctx, B.fn, "fall");
            LLVMBuildCondBr(B.b, c, tgt, fall);
            LLVMPositionBuilderAtEnd(B.b, tgt);
            B.write_pc(op.target);
            LLVMBuildRetVoid(B.b);
            LLVMPositionBuilderAtEnd(B.b, fall);
            B.write_pc(op.fallthrough);
            LLVMBuildRetVoid(B.b);
            return true;
        }
        default:
            return false;  // CondCmpImm / Fail / future ops: interpreter fallback
    }
}

// Builds the LLVM function for one block. Returns false on unsupported IR.
bool BuildBlockFn(LLVMModuleRef mod, u32 block_pc, const std::vector<AIROp>& ir,
                  const char* name_suffix, std::string* fn_name) {
    LLVMContextRef c = LLVMGetModuleContext(mod);
    T = MakeTypes(c);
    char name[48];
    if (name_suffix && name_suffix[0])
        std::snprintf(name, sizeof(name), "ap3_%08x_%s", block_pc, name_suffix);
    else
        std::snprintf(name, sizeof(name), "ap3_%08x", block_pc);
    *fn_name = name;
    LLVMValueRef fn = LLVMAddFunction(mod, name, T.fty);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(g.ctx, fn, "entry");
    LLVMBuilderRef b = LLVMCreateBuilderInContext(g.ctx);
    LLVMPositionBuilderAtEnd(b, entry);

    BlockBuilder B{};
    B.mod = mod;
    B.fn = fn;
    B.b = b;
    B.ctx = LLVMGetParam(fn, 0);
    B.i64p = LLVMPointerType(T.i64, 0);
    B.i32p = LLVMPointerType(T.i32, 0);
    B.bswap32_fn = LLVMAddFunction(mod, "llvm.bswap.i32", LLVMFunctionType(T.i32, &T.i32, 1, 0));
    B.bswap64_fn = LLVMAddFunction(mod, "llvm.bswap.i64", LLVMFunctionType(T.i64, &T.i64, 1, 0));

    for (const AIROp& op : ir) {
        if (!EmitOp(B, op)) {
            LLVMDisposeBuilder(b);
            return false;
        }
    }
    // No terminator op: fall through with pc advanced past the last insn.
    if (LLVMGetBasicBlockTerminator(LLVMGetEntryBasicBlock(fn)) == nullptr &&
        LLVMCountBasicBlocks(fn) == 1) {
        LLVMBuildRetVoid(b);
    }
    LLVMDisposeBuilder(b);
    return LLVMVerifyFunction(fn, LLVMReturnStatusAction) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API (see aot.h): per-block JIT + module-level AOT emission.
// ---------------------------------------------------------------------------

BlockFn CompileLlvmBlock(u32 block_pc, const std::vector<AIROp>& ir) {
    InitLlvm();
    if (!g.ok) return nullptr;

    // Build the module INSIDE a fresh ThreadSafeContext (required by ORC).
    LLVMOrcThreadSafeContextRef tsc = LLVMOrcCreateNewThreadSafeContext();
    LLVMContextRef c = LLVMOrcThreadSafeContextGetContext(tsc);
    static std::atomic<unsigned> seq{0};
    char mname[48], suffix[16];
    std::snprintf(suffix, sizeof(suffix), "%u", seq.fetch_add(1));
    std::snprintf(mname, sizeof(mname), "ap3mod_%08x_%s", block_pc, suffix);
    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext(mname, c);
    std::string fname;
    if (!BuildBlockFn(mod, block_pc, ir, suffix, &fname) ||
        LLVMVerifyModule(mod, LLVMReturnStatusAction, nullptr)) {
        LLVMDisposeModule(mod);
        LLVMOrcDisposeThreadSafeContext(tsc);
        return nullptr;
    }
    LLVMOrcJITDylibRef lib = LLVMOrcLLJITGetMainJITDylib(g.jit);
    LLVMOrcThreadSafeModuleRef tsm = LLVMOrcCreateNewThreadSafeModule(mod, tsc);
    LLVMErrorRef err = LLVMOrcLLJITAddLLVMIRModule(g.jit, lib, tsm);
    LLVMOrcDisposeThreadSafeContext(tsc);
    if (err) {
        LLVMDisposeMessage(reinterpret_cast<char*>(err));
        return nullptr;
    }
    LLVMOrcJITTargetAddress addr = 0;
    if (LLVMOrcLLJITLookup(g.jit, &addr, fname.c_str()) != LLVMErrorSuccess || addr == 0) {
        return nullptr;
    }
    return reinterpret_cast<BlockFn>(addr);
}

bool LlvmEmitAot(const char* module_name,
                 const std::vector<std::pair<u32, const std::vector<AIROp>*>>& blocks,
                 const char* out_dir, LlvmAotArtifacts* out) {
    InitLlvm();
    if (!g.ok || out == nullptr) return false;
    *out = LlvmAotArtifacts{};

    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext(module_name, g.ctx);
    u32 built = 0;
    std::vector<std::string> names;
    for (const auto& [pc, ir] : blocks) {
        std::string nm;
        if (!ir || ir->empty()) continue;
        if (!BuildBlockFn(mod, pc, *ir, "", &nm)) continue;
        names.push_back(nm);
        built++;
    }
    if (built == 0) {
        LLVMDisposeModule(mod);
        return false;
    }
    out->blocks_built = built;

    // 1) human-readable LLVM IR
    std::string ll_path = std::string(out_dir) + "/" + module_name + ".ll";
    if (LLVMPrintModuleToFile(mod, ll_path.c_str(), nullptr) == 0) {
        out->ll_path = ll_path;
    }

    // 2) native objects for x86-64 and aarch64
    struct TargetSpec {
        const char* triple;
        const char* cpu;
        std::string* path_out;
        bool* ok_out;
    };
    std::string x64_path = std::string(out_dir) + "/" + module_name + ".x64.o";
    std::string arm64_path = std::string(out_dir) + "/" + module_name + ".arm64.o";
    bool x64_ok = false, arm64_ok = false;
    TargetSpec targets[2] = {
        {"x86_64-unknown-linux-gnu", "x86-64", &x64_path, &x64_ok},
        {"aarch64-unknown-linux-gnu", "generic", &arm64_path, &arm64_ok},
    };
    for (const TargetSpec& t : targets) {
        LLVMTargetRef target = nullptr;
        char* err = nullptr;
        if (LLVMGetTargetFromTriple(t.triple, &target, &err)) continue;
        LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
            target, t.triple, t.cpu, "", LLVMCodeGenLevelDefault, LLVMRelocPIC,
            LLVMCodeModelDefault);
        if (!tm) continue;
        // Set the module's triple + data layout for this target before emitting.
        LLVMTargetDataRef td = LLVMCreateTargetDataLayout(tm);
        LLVMSetModuleDataLayout(mod, td);
        LLVMSetTarget(mod, t.triple);
        if (LLVMTargetMachineEmitToFile(tm, mod, const_cast<char*>(t.path_out->c_str()),
                                        LLVMObjectFile, nullptr) == 0) {
            *t.ok_out = true;
        }
        LLVMDisposeTargetMachine(tm);
    }
    out->x64_obj_path = x64_ok ? x64_path : "";
    out->arm64_obj_path = arm64_ok ? arm64_path : "";

    LLVMDisposeModule(mod);
    return built > 0 && (out->ll_path.size() > 0);
}

bool LlvmAvailable() {
    InitLlvm();
    return g.ok;
}

}  // namespace mini_aot
}  // namespace anyps3

#endif  // ANYPS3_WITH_LLVM
