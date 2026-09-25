/*---
    VmBackendEmitStmtFlow.cpp — 简单语句与循环发射：行锚点、return/调用/段落、if/while/do/for、break/continue、语句兜底。
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

void VmBackend::EmitStatementAnchor(SnStatement& stmt, BytecodeEmitter& emitter)
{
    if (auto* pLoc = stmt.Location()) {
        if (auto* pScript = dynamic_cast<const ScriptLocation*>(pLoc)) {
            uint16_t line = static_cast<uint16_t>(
                pScript->m_nStartLine & 0xFFFF);
            emitter.Emit(OpCode::OP_DebugInfo);
            emitter.EmitUint16(line);
        }
}
}

void VmBackend::Access(SnReturnStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& ret = static_cast<SnReturnStmt&>(stmt);
        if (ret.Result()) {
            //Phase 9d-2: evaluate the return expression BEFORE running
            //finally copies (Java semantics: expr first, finally second).
            EmitExpression(*ret.Result(), emitter, m_currFunc->returnSlot);
        }
        //Phase 9d-2: a return leaving try/finally regions runs each
        //finally body inline (innermost first). returnSlot is a reserved
        //slot so the copies cannot clobber the result.
        for (size_t i = m_finallyStack.size(); i > 0; --i)
            EmitStatement(*m_finallyStack[i - 1], emitter);
        if (ret.Result()) {
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(m_currFunc->returnSlot);
        }
        emitter.Emit(OpCode::OP_Return);
        return;
}

void VmBackend::Access(SnInvokeStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& invoke = static_cast<SnInvokeStmt&>(stmt);
        //Result staging: EvalAreaClaim, never tempSlot (round-9 — uniform
        //statement-staging rule; the result is discarded, but intermediates
        //must not park in a temp either. StmtPeakDepth tracks the claim=1).
        EvalAreaClaim resultClaim(*this, 1);
        EmitExpression(*invoke.Expr(), emitter, resultClaim.base());
        return;
}

void VmBackend::Access(SnParagraph& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& para = static_cast<SnParagraph&>(stmt);
        for (auto& child : para.Statements())
            EmitStatement(child, emitter);
        return;
}

    //Local variable declaration.
    //After the decomposition pattern, initializers are handled by
    //AssignStmts inserted after this declaration. We only allocate
    //the local variable slot here.
void VmBackend::Access(SnIfStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& ifStmt = static_cast<SnIfStmt&>(stmt);
        //Condition staging: EvalAreaClaim, never tempSlot (round-9 —
        //binary cond LEFT operand vs. EmitBinding struct deep-copy scratch;
        //StmtPeakDepth's IfStmt case tracks the claim=1). The claim's
        //scope ends at the JumpIfNot operand (its last read): the branch
        //bodies re-claim fresh slots, so nested statements never stack.
        size_t jumpToElse;
        {
            EvalAreaClaim condClaim(*this, 1);
            uint16_t condSlot = condClaim.base();
            EmitExpression(*ifStmt.Cond(), emitter, condSlot);
            //JumpIfNot to else/endif
            emitter.Emit(OpCode::OP_JumpIfNot);
            jumpToElse = emitter.CurrentOffset();
            emitter.EmitUint16(0);  //placeholder for target
            emitter.EmitUint16(condSlot);  //local offset to check
        }
        //Then branch
        EmitStatement(*ifStmt.ThenStmt(), emitter);
        //Jump to endif (skip else branch)
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder for target
        //Else branch
        size_t elseStart = emitter.CurrentOffset();
        if (ifStmt.ElseStmt())
            EmitStatement(*ifStmt.ElseStmt(), emitter);
        //Fixup jumps
        size_t endPos = emitter.CurrentOffset();
        emitter.PatchUint16(jumpToElse, static_cast<uint16_t>(elseStart));
        emitter.PatchUint16(jumpToEnd, static_cast<uint16_t>(endPos));
        return;
}

    //While loop statement.
void VmBackend::Access(SnWhileStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& whileStmt = static_cast<SnWhileStmt&>(stmt);
        size_t loopStart = emitter.CurrentOffset();
        //Back-edge landing: the re-jump and `continue` both target
        //loopStart, so the while line re-fires every iteration.
        EmitStatementAnchor(whileStmt, emitter);

        PushLoopContext();

        //Condition staging: EvalAreaClaim, never tempSlot (round-9 —
        //same binary-LEFT vs. struct deep-copy scratch family; the loop
        //bound was silently corrupted. StmtPeakDepth tracks the claim=1).
        //Scope ends at the JumpIfNot operand — the loop body re-claims.
        size_t jumpToEnd;
        {
            EvalAreaClaim condClaim(*this, 1);
            uint16_t condSlot = condClaim.base();
            EmitExpression(*whileStmt.Cond(), emitter, condSlot);
            //JumpIfNot to end of loop
            emitter.Emit(OpCode::OP_JumpIfNot);
            jumpToEnd = emitter.CurrentOffset();
            emitter.EmitUint16(0);  //placeholder for target
            emitter.EmitUint16(condSlot);  //local offset to check
        }
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //Loop body
        EmitStatement(*whileStmt.Body(), emitter);

        //Jump back to loop start
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //Fixup jumps
        size_t loopEnd = emitter.CurrentOffset();
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
        //continue jumps back to locStart (condition check)
        for (size_t pos : ctx.continueJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopStart));

        m_loopStack.pop_back();
        return;
}

    //Do-while loop statement.
void VmBackend::Access(SnDoStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& doStmt = static_cast<SnDoStmt&>(stmt);

        PushLoopContext();

        //1. Loop body (executed at least once)
        size_t loopStart = emitter.CurrentOffset();
        EmitStatement(*doStmt.Body(), emitter);

        //2. Continue target: condition check
        size_t continueTarget = emitter.CurrentOffset();
        //Anchor at the tail condition re-check — the per-iteration pass
        //point for do-while (first iteration falls through from entry;
        //see EmitStatementAnchor).
        EmitStatementAnchor(doStmt, emitter);

        //3. Condition check (claim staging — see WhileStmt above, round-9;
        //scope ends at the JumpIfNot operand)
        size_t jumpToEnd;
        {
            EvalAreaClaim condClaim(*this, 1);
            uint16_t condSlot = condClaim.base();
            EmitExpression(*doStmt.Cond(), emitter, condSlot);
            emitter.Emit(OpCode::OP_JumpIfNot);
            jumpToEnd = emitter.CurrentOffset();
            emitter.EmitUint16(0);  //placeholder
            emitter.EmitUint16(condSlot);
        }
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //4. Jump back to loop start
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //5. Loop end, fixup jumps
        size_t loopEnd = emitter.CurrentOffset();
        auto& ctx = m_loopStack.back();
        for (size_t pos : ctx.breakJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(loopEnd));
        //continue jumps to locCondition
        for (size_t pos : ctx.continueJumps)
            emitter.PatchUint16(pos, static_cast<uint16_t>(continueTarget));

        m_loopStack.pop_back();
        return;
}

    //For loop statement.
void VmBackend::Access(SnForStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    auto& forStmt = static_cast<SnForStmt&>(stmt);

        //1. Compile init part (before loop context)
        if (forStmt.Init())
            EmitStatement(*forStmt.Init(), emitter);
        //Compile decomposed init AssignStmts
        for (auto* pExtra : forStmt.InitExtras())
            EmitStatement(*pExtra, emitter);

        //2. Loop start
        size_t loopStart = emitter.CurrentOffset();
        //Back-edge landing: the re-jump reaches here after fini, so the
        //for line re-fires every iteration (see WhileStmt).
        EmitStatementAnchor(forStmt, emitter);

        //3. Enter loop context
        PushLoopContext();

        //4. Condition check
        size_t jumpToEnd = EmitForCondition(forStmt, emitter);
        m_loopStack.back().breakJumps.push_back(jumpToEnd);

        //5. Loop body
        EmitStatement(*forStmt.Body(), emitter);

        //6. Continue target: fini part
        size_t continueTarget = emitter.CurrentOffset();

        //7. Compile fini part
        if (forStmt.Fini())
            EmitStatement(*forStmt.Fini(), emitter);

        //8. Jump back to loop start
        emitter.Emit(OpCode::OP_Jump);
        emitter.EmitUint16(static_cast<uint16_t>(loopStart));

        //9. Loop end
        size_t loopEnd = emitter.CurrentOffset();

        //10. Fixup jumps
        EmitLoopExitFixups(loopEnd, continueTarget, emitter);

        m_loopStack.pop_back();
        return;
}

//For-loop condition check: claim-staged evaluation and the JumpIfNot
//placeholder (claim staging — see WhileStmt above, round-9; the claim's
//scope ends at the JumpIfNot operand so the body re-claims). Returns the
//placeholder offset to patch with the loop-end address.
size_t VmBackend::EmitForCondition(SnForStmt& forStmt, BytecodeEmitter& emitter) {
    size_t jumpToEnd;
    {
        EvalAreaClaim condClaim(*this, 1);
        uint16_t condSlot = condClaim.base();
        EmitExpression(*forStmt.Cond(), emitter, condSlot);
        emitter.Emit(OpCode::OP_JumpIfNot);
        jumpToEnd = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        emitter.EmitUint16(condSlot);
    }
    return jumpToEnd;
}

//Patch the innermost loop context's break/continue placeholders
//(break → breakTarget, continue → continueTarget).
void VmBackend::EmitLoopExitFixups(size_t breakTarget, size_t continueTarget,
                                   BytecodeEmitter& emitter) {
    auto& ctx = m_loopStack.back();
    for (size_t pos : ctx.breakJumps)
        emitter.PatchUint16(pos, static_cast<uint16_t>(breakTarget));
    for (size_t pos : ctx.continueJumps)
        emitter.PatchUint16(pos, static_cast<uint16_t>(continueTarget));
}

    //Foreach loop statement (Phase 8e-5).
    //Index-based expansion reusing Length()/Get() (List), arr.length + arr[i] (Array).
    //No new opcode. Dict path is Phase D.
void VmBackend::Access(SnBreakStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        if (m_loopStack.empty()) {
            //This should be caught by an earlier validation pass.
            assert(!"break statement not in loop or switch");
            return;
        }
        //Phase 9d follow-up: if break exits a loop/switch that sits
        //outside one or more catch bodies (relative to where the break
        //is lexically), emit OP_PopHandler for each such catch body.
        //This balances handlerExcStack — otherwise a subsequent `throw;`
        //in this function would rethrow a stale exception.
        int pops = m_catchBodyDepth - m_loopStack.back().catchBodyDepthAtEntry;
        for (int i = 0; i < pops; ++i)
            emitter.Emit(OpCode::OP_PopHandler);
        //Phase 9d-2: run inline copies of the finally bodies this break
        //passes through (regions entered after the target loop), innermost
        //first, then jump to the loop's break target.
        for (size_t i = m_finallyStack.size();
             i > static_cast<size_t>(m_loopStack.back().finallyDepthAtEntry);
             --i) {
            EmitStatement(*m_finallyStack[i - 1], emitter);
        }
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpPos = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        m_loopStack.back().breakJumps.push_back(jumpPos);
        return;
}

    //Continue statement.
    //Continue targets the innermost enclosing *loop*, not switch.
void VmBackend::Access(SnContinueStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        //Walk the stack to find the nearest actual loop (not switch).
        //continue skips switch contexts.
        auto it = m_loopStack.rbegin();
        while (it != m_loopStack.rend() && it->isSwitch)
            ++it;
        if (it == m_loopStack.rend()) {
            //This should be caught by an earlier validation pass.
            assert(!"continue statement not in loop");
            return;
        }
        //Phase 9d follow-up: same handler-balancing as break — see above.
        int pops = m_catchBodyDepth - it->catchBodyDepthAtEntry;
        for (int i = 0; i < pops; ++i)
            emitter.Emit(OpCode::OP_PopHandler);
        //Phase 9d-2: same finally trampolines as break, targeting this
        //loop's continue target.
        for (size_t i = m_finallyStack.size();
             i > static_cast<size_t>(it->finallyDepthAtEntry);
             --i) {
            EmitStatement(*m_finallyStack[i - 1], emitter);
        }
        emitter.Emit(OpCode::OP_Jump);
        size_t jumpPos = emitter.CurrentOffset();
        emitter.EmitUint16(0);  //placeholder
        it->continueJumps.push_back(jumpPos);
        return;
}

    //Switch statement.
void VmBackend::Access(SnStatement& stmt) {
    NodeKind kind = stmt.Kind();
    throw std::runtime_error(
        "NLang backend: unhandled statement kind in codegen: "
        + std::to_string(static_cast<int>(kind)) + " at "
        + (stmt.Location() ? stmt.Location()->ToString()
                           : std::string("?")));
}

//Non-emissible node kinds (declarations, formal params, case clauses,
//array-type tokens...) never reach codegen — emission visits only
//value- and statement-position nodes. Reaching this universal fallback
//is an internal invariant break.

} //namespace nlang
