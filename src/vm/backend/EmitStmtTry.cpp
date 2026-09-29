/*---
    EmitStmtTry.cpp — 异常发射：try/catch/finally、throw、super()。
    从 EmitStmtSwitchTry.cpp 抽取（2026-09-29 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
#include "EmitPrimOps.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/compiler/SnExtraTypes.h>
#include <nlang/compiler/ScriptLocation.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>
#include <map>
#include <unordered_set>

namespace nlang {

    //Phase 9d: try { body } catch (Type var) { handler } ...
    //Phase 9d-2: optional finally clause (full Java semantics).
    //
    //Codegen pattern without finally:
    //   tryStart:
    //     <body bytecode>
    //     OP_Jump postTry      ; body completed normally — skip all catches
    //   tryEnd:                 ; (also each handler's startPc for tryBlocks table)
    //   handler1:
    //     <handler1 bytecode>
    //     OP_PopHandler
    //     OP_Jump postTry
    //   handler2:
    //     ...
    //   postTry:
    //
    //Codegen pattern with finally:
    //   tryStart:
    //     <body bytecode>
    //     OP_Jump finallyNormal
    //   tryEnd:
    //   handler1:
    //     <handler1 bytecode>
    //     OP_PopHandler
    //     OP_Jump finallyNormal  ; catch completion also runs finally
    //   handler2:
    //     ...
    //   rangeEnd:                ; finally entry's covered range extends here
    //                             ; so exceptions in catch bodies reach it
    //   finallyHandler:          ; tryBlocks entry (catch-all 0xFFFF), pushed
    //     <finally body copy>    ; LAST so typed catches win first
    //     OP_Rethrow             ; re-raise the in-flight exception
    //   finallyNormal:
    //     <finally body copy>    ; normal/catch-completion path
    //     OP_Jump postTry
    //   postTry:
    //
    //All finally body copies live OUTSIDE the covered range [tryStart,
    //rangeEnd), so an exception thrown inside a finally body propagates
    //outward directly (no double execution by the same handler).
    //break/continue/return leaving the region execute their own inline
    //copies (see NK_BreakStmt/NK_ContinueStmt/NK_ReturnStmt).
    //
    //tryBlocks entries are pushed in declaration order; the runtime scans
    //linearly and the first range+type match wins.
void VmBackend::Access(SnTryStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& ts = static_cast<SnTryStmt&>(stmt);
        SnStatement* pFinally = ts.FinallyBody();
        uint16_t tryStart = static_cast<uint16_t>(emitter.CurrentOffset());
        //Register the finally region BEFORE emitting the try body and
        //catch bodies — break/continue/return sites inside them consult
        //m_finallyStack to emit inline copies.
        if (pFinally)
            m_finallyStack.push_back(pFinally);
        if (ts.TryBody())
            EmitStatement(*ts.TryBody(), emitter);
        //Body completed normally — skip catch handlers (to finallyNormal
        //when a finally clause exists, else postTry).
        emitter.Emit(OpCode::OP_Jump);
        size_t tryEndJumpPatch = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder, patched below
        uint16_t tryEnd = static_cast<uint16_t>(emitter.CurrentOffset());
        std::vector<size_t> catchEndJumpPatches =
            EmitTryCatchClauses(ts, tryStart, tryEnd, emitter);
        if (!pFinally) {
            //No finally: normal and catch-completion jumps go to postTry.
            uint16_t postTry = static_cast<uint16_t>(emitter.CurrentOffset());
            emitter.PatchUint16(tryEndJumpPatch, postTry);
            for (size_t p : catchEndJumpPatches)
                emitter.PatchUint16(p, postTry);
            return;
        }
        EmitTryFinallyTail(pFinally, tryStart, tryEndJumpPatch,
            catchEndJumpPatches, emitter);
        return;
}

//Catch clause's exception class index, resolved from CatchType's
//EvalDataType (a SnClassDecl*). If unresolved (earlier resolver error),
//0xFFFF — at runtime a catch-all, but unresolved types only occur
//after a reported compile error.
uint16_t VmBackend::FindCatchExceptionClassIdx(SnCatchClause& pCatch) {
    uint16_t excClassIdx = 0xFFFF;
    if (pCatch.CatchType()->IsResolved()
        && pCatch.CatchType()->Field()) {
        auto* pType = pCatch.CatchType()->Field();
        if (pType && pType->Kind() == NK_ClassDecl) {
            auto& ccName = pType->Name();
            auto found = std::find_if(
                m_compiledModule.classes.begin(),
                m_compiledModule.classes.end(),
                [&](const CompiledClass& c) { return c.name == ccName; });
            if (found != m_compiledModule.classes.end()) {
                excClassIdx = static_cast<uint16_t>(
                    std::distance(m_compiledModule.classes.begin(),
                                  found));
            }
        }
    }
    return excClassIdx;
}

//Per catch clause: register the tryBlocks entry ([tryStart, tryEnd) →
//handlerPc, typed by the exception class, catch-var slot), emit the
//handler body bracketed by catch-body depth tracking, then PopHandler +
//the completion jump. Returns the completion jumps' patch offsets (all
//target postTry, or finallyNormal when a finally clause exists).
std::vector<size_t> VmBackend::EmitTryCatchClauses(SnTryStmt& ts,
        uint16_t tryStart, uint16_t tryEnd, BytecodeEmitter& emitter) {
    std::vector<size_t> catchEndJumpPatches;
    for (auto* pCatch : ts.Catches()) {
        uint16_t handlerPc = static_cast<uint16_t>(emitter.CurrentOffset());
        uint16_t excClassIdx = FindCatchExceptionClassIdx(*pCatch);
        //catchLocalOff: allocate a local slot for the catch var. This
        //is where the runtime writes the caught Exception heap idx on
        //handler entry, and where the body's IdentifierExpr resolves.
        uint16_t typeKind = RTK_Class;
        uint16_t catchOff = AllocLocal(pCatch->VarName(), kFrameSlotBytes,
                                       typeKind, false);
        m_currFunc->func->tryBlocks.push_back(
            {tryStart, tryEnd, handlerPc, excClassIdx, catchOff});
        if (pCatch->Body()) {
            ++m_catchBodyDepth;
            EmitStatement(*pCatch->Body(), emitter);
            --m_catchBodyDepth;
        }
        emitter.Emit(OpCode::OP_PopHandler);
        emitter.Emit(OpCode::OP_Jump);
        catchEndJumpPatches.push_back(emitter.CurrentOffset());
        emitter.EmitUint16(0);  //placeholder
    }
    return catchEndJumpPatches;
}

//Finally layout. The finally entry's covered range extends past the
//catch handlers so exceptions raised inside a catch body also run the
//finally body (then rethrow outward — a sibling catch must NOT
//intercept it, which the linear scan guarantees because each catch
//entry's endPc is still tryEnd). The handler copy runs the body then
//rethrows; the normal copy (try/catch completion) runs the body then
//jumps past the region. finallyExcOff must be a real slot: the runtime
//writes the caught exception heap idx there on handler entry
//unconditionally — a hidden shared scratch local (name-deduped per
//function) is dead storage; slot 0 would clobber `this` in methods.
void VmBackend::EmitTryFinallyTail(SnStatement* pFinally,
        uint16_t tryStart, size_t tryEndJumpPatch,
        const std::vector<size_t>& catchEndJumpPatches,
        BytecodeEmitter& emitter) {
    uint16_t rangeEnd = static_cast<uint16_t>(emitter.CurrentOffset());
    uint16_t finallyHandlerPc = rangeEnd;
    //finallyHandler: exception path — run body copy, then rethrow.
    uint16_t finallyExcOff = AllocLocal("$finally_exc", kFrameSlotBytes,
                                        RTK_Class, false);
    m_currFunc->func->tryBlocks.push_back(
        {tryStart, rangeEnd, finallyHandlerPc, 0xFFFF, finallyExcOff});
    EmitStatement(*pFinally, emitter);
    emitter.Emit(OpCode::OP_Rethrow);
    //finallyNormal: normal and catch-completion path — body copy, then
    //jump past the region.
    uint16_t finallyNormal = static_cast<uint16_t>(emitter.CurrentOffset());
    EmitStatement(*pFinally, emitter);
    emitter.Emit(OpCode::OP_Jump);
    size_t finallyEndJumpPatch = emitter.CurrentOffset();
    emitter.EmitUint16(0);  //placeholder, patched to postTry
    uint16_t postTry = static_cast<uint16_t>(emitter.CurrentOffset());
    emitter.PatchUint16(tryEndJumpPatch, finallyNormal);
    for (size_t p : catchEndJumpPatches)
        emitter.PatchUint16(p, finallyNormal);
    emitter.PatchUint16(finallyEndJumpPatch, postTry);
    m_finallyStack.pop_back();
}

void VmBackend::Access(SnThrowStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& th = static_cast<SnThrowStmt&>(stmt);
        if (th.IsRethrow()) {
            emitter.Emit(OpCode::OP_Rethrow);
        } else {
            //Expression staging: EvalAreaClaim, never tempSlot (round-9 —
            //uniform statement-staging rule; class-typed exprs write their
            //result last today, but the claim removes the reliance on that
            //luck. StmtPeakDepth's ThrowStmt case tracks the claim=1).
            EvalAreaClaim exprClaim(*this, 1);
            uint16_t exprSlot = exprClaim.base();
            EmitExpression(*th.Expr(), emitter, exprSlot);
            emitter.Emit(OpCode::OP_Throw);
            emitter.EmitUint16(exprSlot);
        }
        return;
}

    //Phase 9d-2: super(args); — forward to the direct parent constructor.
    //Mirrors NK_NewExpr's ctor-call pattern: evalArea claim [0]=this,
    //[1..N]=args, bulk copy to callParamBase, OP_CallMethodDirect on the
    //parent's ctor (works for both user ctors and built-in Exception
    //family stubs via the intrinsic shortcut). No parent ctor (0xFFFF) is
    //a legal no-op — the resolver guarantees no args in that case.
void VmBackend::Access(SnSuperCallStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& sc = static_cast<SnSuperCallStmt&>(stmt);
        if (!m_pCurrClass || !m_pCurrClass->SuperClass())
            return;  //resolver already reported; emit nothing
        auto* pParent = m_pCurrClass->SuperClass();
        int parentClassIdx = m_compiledModule.FindClass(pParent->Name());
        if (parentClassIdx < 0)
            return;
        uint16_t ctorIdx =
            m_compiledModule.classes[parentClassIdx].constructorIdx;
        if (ctorIdx == 0xFFFF)
            return;  //parent has no ctor; args were rejected by resolver

        uint16_t n = static_cast<uint16_t>(1 + sc.Args().size());
        EvalAreaClaim claim(*this, n);
        uint16_t claimBase = claim.base();

        //args → claim[1..N] (positional only; resolver rejected named).
        uint16_t paramIdx = 1;
        for (auto* pArg : sc.Args()) {
            uint16_t paramOffset = claimBase + paramIdx * kFrameSlotBytes;
            EmitExpression(*pArg, emitter, paramOffset);
            ++paramIdx;
        }
        //this (local 0 in a method) → claim[0].
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(0);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(claimBase);

        //Bulk-copy claim → callParamBase, then call the parent ctor.
        for (uint16_t i = 0; i < n; ++i) {
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(claimBase + i * kFrameSlotBytes);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(m_currFunc->callParamBase + i * kFrameSlotBytes);
        }
        emitter.Emit(OpCode::OP_CallMethodDirect);
        emitter.EmitUint16(ctorIdx);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_ParaEnd);
        return;
}

} //namespace nlang
