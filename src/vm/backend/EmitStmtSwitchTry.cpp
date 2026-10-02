/*---
    EmitStmtSwitchTry.cpp — 多路分发与异常发射：switch、try/catch/finally、throw、super()。
    从 VmBackend.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "VmBackend.h"
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

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

//Phase 12 Step 1: typed switch equality. The resolver family-gated the
//discriminant; pick the compare opcode per family. All three share the
//(lhs, rhs) operand layout and write the int result to the lhs slot, so
//the case-clause emission is family-agnostic. Enum discriminants are
//int32 values — OP_Equal_i32 (default).
static OpCode SwitchCompareOp(SnSwitchStmt& switchStmt) {
    if (auto* pCondType = switchStmt.Cond()->EvalDataType()) {
        if (pCondType->Kind() == NK_Float)
            return OpCode::OP_Equal_f32;
        if (pCondType->Kind() == NK_String)
            return OpCode::OP_Eq_str;
    }
    return OpCode::OP_Equal_i32;
}

void VmBackend::Access(SnSwitchStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& switchStmt = static_cast<SnSwitchStmt&>(stmt);
        //1. Dedicated slot for the switch value — must not be overwritten
        //by case condition compilation.
        uint16_t switchSlot = m_currFunc->nextOffset;
        m_currFunc->nextOffset += VALUE_SIZE;
        //2. Compile the switch expression to switchSlot
        EmitExpression(*switchStmt.Cond(), emitter, switchSlot);
        //3. OP_Switch marker (aids disassembly; no runtime effect beyond
        //reading the operand — could gain jump-table semantics later).
        emitter.Emit(OpCode::OP_Switch);
        emitter.EmitUint16(switchSlot);
        //4. Enter switch context (break jumps out of switch)
        PushLoopContext(true);
        //5. Compile each case clause (per-clause emission in the helpers
        //below: marker + multi-value compares + body + implicit exit).
        std::vector<std::vector<size_t>> exitJumps;   //per clause: OP_Case placeholder + last label's miss
        std::vector<size_t> bodyExitJumps;            //implicit no-fallthrough jumps, one per clause body
        std::vector<size_t> caseStartOffsets;
        OpCode compareOp = SwitchCompareOp(switchStmt);
        for (auto* pCase : switchStmt.Cases()) {
            caseStartOffsets.push_back(emitter.CurrentOffset());
            std::vector<size_t> clauseExits;
            size_t bodyExitJump = 0;
            EmitSwitchCaseClause(*pCase, switchSlot, compareOp,
                clauseExits, bodyExitJump, emitter);
            exitJumps.push_back(std::move(clauseExits));
            bodyExitJumps.push_back(bodyExitJump);
        }
        //6. Mark locCaseEnd (after all cases, before default)
        size_t locCaseEnd = emitter.CurrentOffset();
        //7. Compile default clause
        if (switchStmt.Default())
            EmitStatement(*switchStmt.Default(), emitter);
        //8. Mark locEnd (after default)
        size_t locEnd = emitter.CurrentOffset();
        //9. Clause-exit fixup
        EmitSwitchClauseExits(caseStartOffsets, exitJumps,
            switchStmt.Default() != nullptr, locCaseEnd, locEnd, emitter);
        //10. Fix break jumps (jump to switch end)
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(locEnd));
        //11. Patch implicit clause exits (no-fallthrough) to switch end
        for (size_t pos : bodyExitJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(locEnd));
        m_loopStack.pop_back();
        return;
}

//One case clause: OP_Case marker, the label-compare cascade, the clause
//body, and the implicit no-fallthrough exit. clauseExits collects the
//jumps the clause-exit fixup must patch (the OP_Case placeholder and
//the LAST label's miss jump); bodyExitJump is the implicit exit.
void VmBackend::EmitSwitchCaseClause(SnCaseClause& clause,
        uint16_t switchSlot, OpCode compareOp,
        std::vector<size_t>& clauseExits, size_t& bodyExitJump,
        BytecodeEmitter& emitter) {
    //OP_Case with jump-to-next-handler placeholder. OP_Case is a marker
    //opcode: its uint16 operand is patched by the clause-exit fixup but
    //never used at runtime (branching is done by OP_JumpIfNot). A future
    //optimization could merge OP_Case with the condition check.
    emitter.Emit(OpCode::OP_Case);
    size_t jumpToNext = emitter.CurrentOffset();
    emitter.EmitUint16(0);  //placeholder, patched by the clause-exit fixup
    clauseExits.push_back(jumpToNext);
    EmitSwitchLabelCompares(clause, switchSlot, compareOp, clauseExits,
        emitter);
    //Compile case body
    EmitStatement(*clause.Body(), emitter);
    //Implicit clause exit (Java/C# style — no C fallthrough, no `break`
    //needed). Pre-Phase-12 this jump was missing: a body fell into the
    //NEXT clause's compares and, with a duplicate label there,
    //re-matched and ran that body too (probed: case 1 / case 1 with x=1
    //summed both bodies). Distinct labels masked the gap because the
    //compare cascade merely drained to the switch exit; multi-value
    //labels widen the duplicate-collision surface, so the exit is now
    //explicit. Dead but harmless after terminal statements
    //(return/break/continue).
    emitter.Emit(OpCode::OP_Jump);
    bodyExitJump = emitter.CurrentOffset();
    emitter.EmitUint16(0);  //placeholder → locEnd
}

//Phase 12 multi-value labels: a clause holds N labels (`case 1, 2:`)
//and ANY match enters the body. The opcode set has no jump-if-true, so
//an INTERMEDIATE label emits a dual jump (JumpIfNot → the next label's
//compare on miss, unconditional Jump → the clause body on hit); only
//the LAST label's JumpIfNot targets the clause exit (appended to
//clauseExits). A single-label clause degenerates to the pre-Phase-12
//shape.
void VmBackend::EmitSwitchLabelCompares(SnCaseClause& clause,
        uint16_t switchSlot, OpCode compareOp,
        std::vector<size_t>& clauseExits, BytecodeEmitter& emitter) {
    const auto& labels = clause.Labels();
    std::vector<size_t> labelStarts;      //start of each label's compare
    std::vector<size_t> missPositions;    //intermediate labels' JumpIfNot
    std::vector<size_t> hitPositions;     //intermediate labels' Jump
    for (size_t li = 0; li < labels.size(); ++li) {
        labelStarts.push_back(emitter.CurrentOffset());
        size_t condJumpPos = EmitSwitchOneLabelCompare(*labels[li],
            switchSlot, compareOp, emitter);
        if (li + 1 == labels.size()) {
            clauseExits.push_back(condJumpPos);
        } else {
            //If equal, jump straight into the clause body.
            emitter.Emit(OpCode::OP_Jump);
            hitPositions.push_back(emitter.CurrentOffset());
            emitter.EmitUint16(0);  //placeholder → body start
            missPositions.push_back(condJumpPos);
        }
    }
    //Every hit-jump of this clause targets the body start (the next
    //offset), and each intermediate miss targets the NEXT label's
    //compare. missPositions/hitPositions are pushed in pairs per
    //intermediate label, so indexing both by the same bound is safe by
    //construction.
    size_t bodyStart = emitter.CurrentOffset();
    for (size_t mi = 0; mi < missPositions.size(); ++mi) {
        emitter.PatchUint16(missPositions[mi],
            static_cast<uint16_t>(labelStarts[mi + 1]));
        emitter.PatchUint16(hitPositions[mi],
            static_cast<uint16_t>(bodyStart));
    }
}

//One label's compare: switch_value == case_constant, then the miss-jump
//placeholder. Returns the placeholder's offset; the caller decides
//next-label vs clause-exit targeting. Case-cond staging: EvalAreaClaim,
//never tempSlot (round-9 — same binary-LEFT vs. struct deep-copy
//scratch family; a corrupted cond silently fell through to default).
//The switch value load is deliberately emitted AFTER the label so it is
//never parked in a temp across a nested emission. The claim releases
//per label (labels are sequential; one claim's worth suffices —
//StmtPeakDepth's SwitchStmt case tracks claim=1).
size_t VmBackend::EmitSwitchOneLabelCompare(SnExpression& label,
        uint16_t switchSlot, OpCode compareOp, BytecodeEmitter& emitter) {
    size_t condJumpPos;
    {
        EvalAreaClaim condClaim(*this, 1);
        uint16_t condSlot = condClaim.base();
        EmitExpression(label, emitter, condSlot);
        //Load switch value from dedicated slot to tempSlot2. Reloaded
        //for EVERY label: the previous compare's result occupies
        //tempSlot2 and must not feed the next compare.
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(switchSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot2);
        //Compare: tempSlot2 == condSlot → result in tempSlot2
        emitter.Emit(compareOp);
        emitter.EmitUint16(m_currFunc->tempSlot2);
        emitter.EmitUint16(condSlot);
        //If not equal: intermediate labels try the next label, the last
        //label exits to the next case handler.
        emitter.Emit(OpCode::OP_JumpIfNot);
        condJumpPos = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        emitter.EmitUint16(m_currFunc->tempSlot2);
    }
    return condJumpPos;
}

//Clause-exit fixup: patch each clause's exit jumps so they point to the
//next clause's start. The last clause's jumps point to default (if
//present) or the switch end.
void VmBackend::EmitSwitchClauseExits(
        const std::vector<size_t>& caseStartOffsets,
        const std::vector<std::vector<size_t>>& exitJumps,
        bool hasDefault, size_t locCaseEnd, size_t locEnd,
        BytecodeEmitter& emitter) {
    size_t caseCount = caseStartOffsets.size();
    for (size_t i = 0; i < caseCount; ++i) {
        uint16_t target;
        if (i + 1 < caseCount)
            target = static_cast<uint16_t>(caseStartOffsets[i + 1]);
        else
            target = static_cast<uint16_t>(hasDefault ? locCaseEnd
                                                      : locEnd);
        //Variable entry count per clause: the OP_Case placeholder plus
        //the last label's miss jump (pre-Phase-12: exactly 2).
        for (size_t pos : exitJumps[i])
            emitter.PatchUint16(pos, target);
    }
}

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
            //The catch scan is a hand-rolled FindClass (invisible to the
            //Find* grep): the key must come from KeyOf like the writer.
            const std::string ccName = KeyOf(*pType);
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
        uint16_t catchOff = AllocLocal(pCatch->VarName(), VALUE_SIZE,
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
    uint16_t finallyExcOff = AllocLocal("$finally_exc", VALUE_SIZE,
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
        //Per-unit: a cross-unit parent slots as an import placeholder
        //(ClassSlotFor); its ctor is discovered in the AST (CtorSlotFor —
        //placeholders carry no constructorIdx).
        uint16_t parentClassIdx = static_cast<uint16_t>(
            ClassSlotFor(*pParent));
        uint16_t ctorIdx = CtorSlotFor(*pParent, parentClassIdx);
        if (ctorIdx == 0xFFFF)
            return;  //parent has no ctor; args were rejected by resolver

        uint16_t n = static_cast<uint16_t>(1 + sc.Args().size());
        EvalAreaClaim claim(*this, n);
        uint16_t claimBase = claim.base();

        //args → claim[1..N] (positional only; resolver rejected named).
        uint16_t paramIdx = 1;
        for (auto* pArg : sc.Args()) {
            uint16_t paramOffset = claimBase + paramIdx * VALUE_SIZE;
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
            emitter.EmitUint16(claimBase + i * VALUE_SIZE);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(m_currFunc->callParamBase + i * VALUE_SIZE);
        }
        emitter.Emit(OpCode::OP_CallMethodDirect);
        emitter.EmitUint16(ctorIdx);
        emitter.EmitUint16(m_currFunc->callParamBase);
        emitter.Emit(OpCode::OP_ParaEnd);
        return;
}

//Round-13: symmetric to EmitExpression's unhandled-kind throw. Codegen
//only runs when the front-end saw no errors, so an unhandled statement
//kind reaching here is an internal invariant break — silently skipping
//it would emit wrong-but-compiling code (statement simply vanishes).

} //namespace nlang
