/*---
    EmitStmtSwitch.cpp — switch 多路分发发射。
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

//Phase 12 Step 1: typed switch equality. The resolver family-gated the
//discriminant; pick the compare shape per family. Scalar discriminants
//ride the kind-immediate OP_Cmp Equal (same (lhs, rhs) layout, bool
//result to the lhs slot, so the case-clause emission is family-agnostic).
//0.7.5: the compare kind comes from the registry — float rows compare
//at their own width (double joins with Task 7), 8-byte integer rows
//(long/ulong) compare the full slot, and narrow integer rows
//(byte..uint) compare as int32 (the value-extension convention keeps
//narrow values sign/zero-extended in the slot's low bytes, so an
//int32-width read is exact); enum discriminants are int32 values.
//Labels that do not arrive at compare.kind normalize in
//EmitSwitchLabelNormalize.
VmBackend::SwitchCompare VmBackend::SwitchCompareOf(
    SnSwitchStmt& switchStmt) {
    if (auto* pCondType = switchStmt.Cond()->EvalDataType()) {
        NodeKind k = pCondType->Kind();
        if (k == NK_String)
            return {NK_Int32, true};
        if (k == NK_EnumDecl)
            return {NK_Int32, false};
        int pi = ScalarPrimIndexOf(k);
        if (pi >= 0)
        {
            const auto& row = kScalarPrims[pi];
            if (row.category == PC_Float || row.slotWidth == 8)
                return {k, false};
            //0.7.5 string bridge: char discriminants compare at their
            //own kind (labels normalize through EmitScalarSlotCast,
            //char↔int cross-forms included).
            if (k == NK_Char)
                return {NK_Char, false};
            return {NK_Int32, false};
        }
    }
    return {NK_Int32, false};
}

void VmBackend::Access(SnSwitchStmt& stmt) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
        auto& switchStmt = static_cast<SnSwitchStmt&>(stmt);
        //1. Dedicated slot for the switch value — must not be overwritten
        //by case condition compilation.
        uint16_t switchSlot = m_currFunc->nextOffset;
        m_currFunc->nextOffset += kFrameSlotBytes;
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
        SwitchCompare compare = SwitchCompareOf(switchStmt);
        for (auto* pCase : switchStmt.Cases()) {
            caseStartOffsets.push_back(emitter.CurrentOffset());
            std::vector<size_t> clauseExits;
            size_t bodyExitJump = 0;
            EmitSwitchCaseClause(*pCase, switchSlot, compare,
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
        uint16_t switchSlot, const SwitchCompare& compare,
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
    EmitSwitchLabelCompares(clause, switchSlot, compare, clauseExits,
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
        uint16_t switchSlot, const SwitchCompare& compare,
        std::vector<size_t>& clauseExits, BytecodeEmitter& emitter) {
    const auto& labels = clause.Labels();
    std::vector<size_t> labelStarts;      //start of each label's compare
    std::vector<size_t> missPositions;    //intermediate labels' JumpIfNot
    std::vector<size_t> hitPositions;     //intermediate labels' Jump
    for (size_t li = 0; li < labels.size(); ++li) {
        labelStarts.push_back(emitter.CurrentOffset());
        size_t condJumpPos = EmitSwitchOneLabelCompare(*labels[li],
            switchSlot, compare, emitter);
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

//0.7.5: in-place OP_PrimCast of a staged scalar slot from one primitive
//kind to another. Shared by the switch-label normalize (labels stage
//into the cond slot) and the default-argument fill (EmitBinding: a
//double-typed literal default staged into a float formal would leave
//the double's low bytes in the slot). No-op for equal kinds and for
//any non-scalar kind — callers pass their own domain guards.
void VmBackend::EmitScalarSlotCast(NodeKind from, NodeKind to,
        uint16_t slot, BytecodeEmitter& emitter) {
    if (from == to || ScalarPrimIndexOf(from) < 0
        || ScalarPrimIndexOf(to) < 0)
        return;
    EmitPResultRefresh(emitter, slot);
    EmitPrimCast(emitter, from, to);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(slot);
}

//0.7.5: normalize the staged label to the compare kind. The Int family
//admits cross-width labels (an int literal on a long switch, a long
//literal on an int switch) and the compare's two operands must read the
//same slot width — the in-place PrimCast re-widens/truncates the staged
//label. The wrap lives HERE, not in the resolver: the label list is
//SnCaseClause's separate m_upLabels vector, which a FixupExprType child
//replacement would desync. (Enum labels compare as their int32 value.)
void VmBackend::EmitSwitchLabelNormalize(SnExpression& label,
        const SwitchCompare& compare, uint16_t condSlot,
        BytecodeEmitter& emitter) {
    auto* pLabelType = label.EvalDataType();
    if (!pLabelType || compare.isString)
        return;
    NodeKind lk = pLabelType->Kind();
    if (lk == NK_EnumDecl) lk = NK_Int32;
    EmitScalarSlotCast(lk, compare.kind, condSlot, emitter);
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
        uint16_t switchSlot, const SwitchCompare& compare,
        BytecodeEmitter& emitter) {
    size_t condJumpPos;
    {
        EvalAreaClaim condClaim(*this, 1);
        uint16_t condSlot = condClaim.base();
        EmitExpression(label, emitter, condSlot);
        EmitSwitchLabelNormalize(label, compare, condSlot, emitter);
        //Load switch value from dedicated slot to tempSlot2. Reloaded
        //for EVERY label: the previous compare's result occupies
        //tempSlot2 and must not feed the next compare.
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(switchSlot);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot2);
        //Compare: tempSlot2 == condSlot → bool result in tempSlot2.
        //String keeps the dedicated opcode; scalars ride OP_Cmp Equal.
        if (compare.isString) {
            emitter.Emit(OpCode::OP_Eq_str);
            emitter.EmitUint16(m_currFunc->tempSlot2);
            emitter.EmitUint16(condSlot);
        } else {
            EmitCmp(emitter, compare.kind, kCmpEqual,
                    m_currFunc->tempSlot2, condSlot);
        }
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

} //namespace nlang
