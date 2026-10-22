// Any_Ps3 — LLVM AOT backend (AOT-IR -> LLVM IR -> native code via LLVM-C).
//
// ⚠ STATUS: COMPILE-GUARDED AND *UNTESTED* — the CI/dev environment for this
// repository has no LLVM development packages (see PHASE_REPORTS.md). Written
// against the LLVM-C stable C API, LLVM >= 15 (opaque pointers, LLVMBuild*2,
// ORC LLJIT). Enable with `-DANYPS3_LLVM=ON`; then tests/test_aot.cpp runs the
// same GNU-assembled fixtures through this backend. Expect small API-drift
// fixes for your exact LLVM version.
//
// Production role: this is the backend that covers **arm64 (Android/Snapdragon)
// and optimized x64**. The contract matches MiniAOT exactly: compile a block to
// `void block(PpuContext*)` (ctx as i8* with the same field offsets); on any
// unsupported IR op return nullptr and the runtime falls back.
#ifdef ANYPS3_WITH_LLVM

#include <llvm-c/Core.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>
#include <llvm-c/Analysis.h>

#include <cstdio>
#include <mutex>

#include "anyps3/cpu/ppu/aot.h"
#include "anyps3/cpu/ppu/state.h"

namespace anyps3 {
namespace mini_aot {

namespace {

struct LlvmGlobals {
    LLVMContextRef ctx = nullptr;
    LLVMOrcLLJITRef jit = nullptr;
    LLVMTypeRef i8 = nullptr, i32 = nullptr, i64 = nullptr, i8p = nullptr, void_t = nullptr;
    std::once_flag once;
    bool ok = false;
};
LlvmGlobals g;

LLVMValueRef CtxFieldPtr(LLVMBuilderRef b, LLVMValueRef ctx, size_t off, LLVMTypeRef ty) {
    // ctx is i8*; byte-pointer GEP then cast to ty*
    LLVMValueRef idx = LLVMConstInt(g.i64, off, 0);
    LLVMValueRef gep = LLVMBuildGEP2(b, g.i8p, ctx, &idx, 1, "f");
    return LLVMBuildPointerCast(b, gep, ty, "fp");
}

}  // namespace

static void InitLlvm() {
    std::call_once(g.once, [] {
        LLVMInitializeNativeTarget();
        LLVMInitializeNativeAsmPrinter();
        g.ctx = LLVMContextCreate();
        g.i8 = LLVMInt8TypeInContext(g.ctx);
        g.i32 = LLVMInt32TypeInContext(g.ctx);
        g.i64 = LLVMInt64TypeInContext(g.ctx);
        g.i8p = LLVMPointerTypeInContext(g.ctx, 0);
        g.void_t = LLVMVoidTypeInContext(g.ctx);
        LLVMOrcCreateLLJITOptions opts = LLVMOrcCreateLLJITOptions();
        if (LLVMOrcCreateLLJIT(&g.jit, opts) == LLVORC_ERR_SUCCESS) g.ok = true;
    });
}

// Field offsets shared with the interpreter/MiniAOT (recomputed here).
static size_t GprOff(int r) { return offsetof(PpuContext, gpr) + r * 8; }

BlockFn CompileLlvmBlock(u32 block_pc, const std::vector<AIROp>& ir) {
    InitLlvm();
    if (!g.ok) return nullptr;

    char mname[40], fname[40];
    std::snprintf(mname, sizeof(mname), "ap3_%08x", block_pc);
    std::snprintf(fname, sizeof(fname), "%s", mname);

    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext(mname, g.ctx);
    LLVMTypeRef fty = LLVMFunctionType(g.void_t, &g.i8p, 1, 0);
    LLVMValueRef fn = LLVMAddFunction(mod, fname, fty);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(g.ctx, fn, "entry");
    LLVMBuilderRef b = LLVMCreateBuilderInContext(g.ctx);
    LLVMPositionBuilderAtEnd(b, entry);

    LLVMTypeRef i64p = LLVMPointerType(g.i64, 0);
    LLVMValueRef ctx = LLVMGetParam(fn, 0);
    auto gprp = [&](int r) { return CtxFieldPtr(b, ctx, GprOff(r), i64p); };
    auto i32p = [&](size_t off) { return CtxFieldPtr(b, ctx, off, LLVMPointerType(g.i32, 0)); };
    auto i8p_fld = [&](size_t off) { return CtxFieldPtr(b, ctx, off, LLVMPointerType(g.i8, 0)); };
    auto i64p_fld = [&](size_t off) { return CtxFieldPtr(b, ctx, off, i64p); };

    auto set_error_and_ret = [&](u32 fault_pc) {
        LLVMValueRef one = LLVMConstInt(g.i8, 1, 0);
        LLVMBuildStore(b, one, i8p_fld(offsetof(PpuContext, error)));
        LLVMBuildStore(b, LLVMConstInt(g.i32, fault_pc, 0),
                       i32p(offsetof(PpuContext, error_pc)));
        LLVMBuildRetVoid(b);
    };
    auto write_pc = [&](u32 target) {
        LLVMBuildStore(b, LLVMConstInt(g.i32, target, 0), i32p(offsetof(PpuContext, pc)));
    };
    // guest pointer with bounds check; returns {ok_block_taken}
    auto bounds_ok = [&](LLVMValueRef addr32, u32 size) {
        LLVMValueRef rel = LLVMBuildSub(b, addr32, LLVMConstInt(g.i32, 0x10000, 0), "rel");
        LLVMValueRef max = LLVMConstInt(g.i32, 0x1FFF0000u - size, 0);
        return LLVMBuildICmp(b, LLVMIntULE, rel, max, "in_range");
    };

    for (const AIROp& op : ir) {
        switch (op.kind) {
            case IRKind::GprImm:
                LLVMBuildStore(b, LLVMConstInt(g.i64, op.imm, 1), gprp(op.dst));
                break;
            case IRKind::GprAddImm: {
                LLVMValueRef v = LLVMBuildLoad2(b, g.i64, gprp(op.a), "");
                v = LLVMBuildAdd(b, v, LLVMConstInt(g.i64, static_cast<s64>(op.imm), 1), "");
                LLVMBuildStore(b, v, gprp(op.dst));
                break;
            }
            case IRKind::AluAdd: case IRKind::AluOr: case IRKind::AluXor: case IRKind::AluAnd: {
                LLVMValueRef a = LLVMBuildLoad2(b, g.i64, gprp(op.a), "");
                LLVMValueRef v2 = LLVMBuildLoad2(b, g.i64, gprp(op.b), "");
                LLVMValueRef r = op.kind == IRKind::AluAdd ? LLVMBuildAdd(b, a, v2, "")
                            : op.kind == IRKind::AluOr ? LLVMBuildOr(b, a, v2, "")
                            : op.kind == IRKind::AluXor ? LLVMBuildXor(b, a, v2, "")
                                                        : LLVMBuildAnd(b, a, v2, "");
                LLVMBuildStore(b, r, gprp(op.dst));
                break;
            }
            case IRKind::AluSub: {
                LLVMValueRef x = LLVMBuildLoad2(b, g.i64, gprp(op.b), "");
                LLVMValueRef y = LLVMBuildLoad2(b, g.i64, gprp(op.a), "");
                LLVMBuildStore(b, LLVMBuildSub(b, x, y, ""), gprp(op.dst));
                break;
            }
            case IRKind::AluOrImm: {
                LLVMValueRef v = LLVMBuildLoad2(b, g.i64, gprp(op.a), "");
                v = LLVMBuildOr(b, v, LLVMConstInt(g.i64, static_cast<s64>(op.imm), 1), "");
                LLVMBuildStore(b, v, gprp(op.dst));
                break;
            }
            case IRKind::AluSar64Imm: {
                LLVMValueRef v = LLVMBuildLoad2(b, g.i64, gprp(op.a), "");
                v = LLVMBuildAShr(b, v, LLVMConstInt(g.i64, op.b, 0), "");
                LLVMBuildStore(b, v, gprp(op.dst));
                break;
            }
            case IRKind::MtCtr: {
                LLVMValueRef v = LLVMBuildLoad2(b, g.i64, gprp(op.dst), "");
                LLVMBuildStore(b, v, i64p_fld(offsetof(PpuContext, ctr)));
                break;
            }
            case IRKind::MtLr: {
                LLVMValueRef v = LLVMBuildLoad2(b, g.i64, gprp(op.dst), "");
                LLVMBuildStore(b, v, i64p_fld(offsetof(PpuContext, lr)));
                break;
            }
            case IRKind::Syscall: {
                LLVMTypeRef fptr_ty = LLVMPointerType(g.void_t, 0);
                LLVMValueRef fptr = LLVMBuildLoad2(
                    b, fptr_ty, CtxFieldPtr(b, ctx, offsetof(PpuContext, syscall_fn), fptr_ty),
                    "");
                LLVMValueRef args[1] = {ctx};
                LLVMBuildCall2(b, fty, fptr, args, 1, "");
                LLVMValueRef exited = LLVMBuildLoad2(
                    b, g.i8, i8p_fld(offsetof(PpuContext, exit)), "");
                LLVMValueRef done = LLVMBuildICmp(b, LLVMIntNE, exited,
                                                  LLVMConstInt(g.i8, 0, 0), "done");
                LLVMBasicBlockRef cont = LLVMAppendBasicBlockInContext(g.ctx, fn, "cont");
                LLVMBasicBlockRef stop = LLVMAppendBasicBlockInContext(g.ctx, fn, "stop");
                LLVMBuildCondBr(b, done, stop, cont);
                LLVMPositionBuilderAtEnd(b, stop);
                LLVMBuildRetVoid(b);
                LLVMPositionBuilderAtEnd(b, cont);
                break;
            }
            case IRKind::BranchAlways:
                write_pc(op.target);
                LLVMBuildRetVoid(b);
                goto done_ir;
            default:
                // Conservative fallback: unknown op -> caller uses MiniAOT/interpreter.
                LLVMDisposeBuilder(b);
                LLVMDisposeModule(mod);
                return nullptr;
        }
    }
    LLVMBuildRetVoid(b);  // no terminator op: fall through with pc advanced later
done_ir:
    LLVMDisposeBuilder(b);

    if (LLVMVerifyModule(mod, LLVMReturnStatus, nullptr)) {
        LLVMDisposeModule(mod);
        return nullptr;
    }

    // ORC: add IR module to the main dylib and look the function up.
    LLVMOrcJITDylibRef lib = LLVMOrcLLJITGetMainJITDylib(g.jit);
    LLVMErrorRef err = LLVMOrcLLJITAddLLVMIRModule(
        g.jit, lib,
        LLVMOrcCreateNewThreadSafeModule(mod, LLVMOrcCreateNewThreadSafeContext()));
    if (err) {
        LLVMDisposeMessage(reinterpret_cast<char*>(err));
        return nullptr;
    }
    LLVMOrcJITTargetAddress addr = 0;
    if (LLVMOrcLLJITLookup(g.jit, &addr, fname) != LLVORC_ERR_SUCCESS || addr == 0) {
        return nullptr;
    }
    return reinterpret_cast<BlockFn>(addr);
}

}  // namespace mini_aot
}  // namespace anyps3

#endif  // ANYPS3_WITH_LLVM
