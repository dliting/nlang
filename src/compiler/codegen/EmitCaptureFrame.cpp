/*---
    EmitCaptureFrame.cpp — 0.13.10 lambda capture emission, synthetic
    frame half. The captured-formal prologue copies (each formal's
    argument lands in its display field, flavor-dispatched) and the
    lambda literal's bound-func emission — the two emission moments of
    the frame the analysis synthesized for one lambda. Split from
    EmitCapture.cpp (source-size guard, zero behavior change); the
    display-instance machinery (construct/activate/carry/copy, cell
    access, captured reads) stays there.
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>

namespace nlang {

//Frame prologue half 2: captured formals copy from their frame slots
//into the display fields (the analysis mirrored each capture onto the
//rebuilt formal); the copy itself is the flavor-dispatched store.
void VmBackend::EmitCapturedFormalCopies(SnFunction& func,
    BytecodeEmitter& emitter)
{
    for (auto& param : func.Params()) {
        if (!param.ContainFlags(NF_Captured))
            continue;
        EmitCapturedFormalCopy(param, emitter);
    }
}

//Flavor-dispatched store of one captured value: dstSlot's field at
//fieldOff takes the value living in frame slot srcSlot. The capture
//flavor rides the display field's own flags (SynthCapturedField
//mirrors it there at synthesis): a promoted capture wraps the raw
//value into the box the field owns (the frame slot keeps the raw
//value, dead after the copy); a reference pair copies as the 8-byte
//cell; a value capture goes through the struct-aware store (in place
//for struct-typed values, born records from OP_New). The promoted
//arm parks the fresh box in the shared temp — no nested emission
//between (the temps' two-op contract); the raw value still in
//srcSlot rides its kind's heap GC arm across the Box allocation
//(over-mark, same argument as EmitPromotedValueWrap). Shared by the
//prologue's formal copies and the call-site snapshot fill.
void VmBackend::EmitCapturedFieldStore(SnField& field, uint16_t dstSlot,
    uint16_t fieldOff, uint16_t srcSlot, BytecodeEmitter& emitter)
{
    if (field.ContainFlags(NF_Promoted)) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(srcSlot);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(BoxTypeTagOfKind(field.EvalDataType()->Kind()));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(dstSlot);
        emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
        emitter.EmitUint16(m_currFunc->tempSlot);
    } else if (field.ContainFlags(NF_Ref)) {
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(dstSlot);
        emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
        emitter.EmitUint16(srcSlot);
    } else {
        EmitStructAwareStoreField(field.EvalDataType(), dstSlot,
            static_cast<uint16_t>(fieldOff), srcSlot, emitter);
    }
}

//One captured formal's prologue copy (EmitCapturedFormalCopies' body).
void VmBackend::EmitCapturedFormalCopy(SnFormalParam& param,
    BytecodeEmitter& emitter)
{
    auto* pDisplayField = CapturedDisplayFieldOf(&param);
    auto* pDisplayClass = static_cast<SnClassDecl*>(
        pDisplayField->Parent());
    const uint16_t displaySlot = FindLocal(pDisplayClass->Name());
    const int fieldOff = FindClassFieldOffset(*pDisplayClass,
        pDisplayField->Name());
    NLANG_ASSERT(fieldOff >= 0);
    EmitCapturedFieldStore(*pDisplayField, displaySlot,
        static_cast<uint16_t>(fieldOff), FindLocal(param.Name()),
        emitter);
}

//A captured catch variable's handler-entry copy: the runtime protocol
//lands the caught exception heap idx in the clause's frame slot, and
//every reference (captured or not) reads display storage once the
//descriptor is NF_Captured — so the first handler instruction bridges
//the slot into the display field. The value-flavor struct-aware store
//is the only arm: a catch variable is a class-typed value by
//construction (the flavor dispatch of the formal copy cannot apply).
void VmBackend::EmitCatchVarDisplayCopy(SnCatchClause& catchClause,
    uint16_t catchOff, BytecodeEmitter& emitter)
{
    auto* pDisplayField = CapturedDisplayFieldOf(catchClause.VarLocal());
    auto* pDisplayClass = static_cast<SnClassDecl*>(
        pDisplayField->Parent());
    const uint16_t displaySlot = FindLocal(pDisplayClass->Name());
    const int fieldOff = FindClassFieldOffset(*pDisplayClass,
        pDisplayField->Name());
    NLANG_ASSERT(fieldOff >= 0);
    EmitStructAwareStoreField(pDisplayField->EvalDataType(),
        displaySlot, static_cast<uint16_t>(fieldOff), catchOff, emitter);
}

//Lambda literal: bind the anchor display instance (a frame local under
//the display class name — the anchor paragraph is always inside the
//literal's own emission frame) and the synthetic method into a delegate
//handle — the EmitMemberFuncHandleRef shape (receiver-first, pResult
//refresh, OP_MakeBoundFunc, assign).
//D-1 (0.13.13a): the lambda at a formal's default-value root builds its
//display at the CALL site instead (EmitCallSiteLambdaHandle) — the
//marker is the self-anchor shape: the synthetic method's body IS the
//lambda's block body (moved at synthesis), and an ordinary lambda's
//anchor is always strictly outside its own body, so only a call-site
//display rides its own lambda's body.
void VmBackend::Access(SnLambdaExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
    auto* pDisplay = expr.DisplayClass();
    auto* pMethod = expr.SyntheticMethod();
    NLANG_ASSERT(pDisplay && pDisplay->ContainFlags(NF_Generated)
        && pMethod && "lambda literal without its synthesis write-backs");
    if (pMethod->Body()
        && pMethod->Body()->DisplayClass() == pDisplay) {
        EmitCallSiteLambdaHandle(*pDisplay, *pMethod, emitter,
            resultOffset);
        return;
    }
    const uint16_t displaySlot = FindLocal(pDisplay->Name());
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(displaySlot);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    EmitPResultRefresh(emitter, resultOffset);
    emitter.Emit(OpCode::OP_MakeBoundFunc);
    emitter.EmitUint16(static_cast<uint16_t>(FunctionSlotFor(*pMethod)));
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//D-1 (0.13.13a): the call-site half of the lambda-literal emission —
//the lambda at a formal's default-value root evaluates at the CALL
//site, where no enclosing prologue constructed its display (prologues
//scan the function body; defaults ride the Params() subtree) — this is
//that display's one construction point. The instance is built directly
//into resultOffset and filled from the call site's override bindings
//(earlier formals' claim slots, the staged receiver — EmitDefaultBinding's
//OverrideScope), then the synthetic method binds on. The GC window is
//the ordinary arm's: the fresh handle parks in resultOffset (a GC root
//— the resultCell invariant) and the tail's PResultRefresh survives it.
void VmBackend::EmitCallSiteLambdaHandle(SnClassDecl& display,
    SnFunction& method, BytecodeEmitter& emitter, uint16_t resultOffset)
{
    emitter.Emit(OpCode::OP_New);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(static_cast<uint16_t>(ClassSlotFor(display)));
    emitter.EmitUint16(DescriptorIndexFor(display));
    EmitCallSiteSnapshotMembers(display, resultOffset, emitter);
    EmitPResultRefresh(emitter, resultOffset);
    emitter.Emit(OpCode::OP_MakeBoundFunc);
    emitter.EmitUint16(static_cast<uint16_t>(FunctionSlotFor(method)));
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
}

//One call-site display's member fill, in the field order the prologue's
//recipe uses: "$this" takes the staged call receiver, every other
//member its CaptureSource formal's staged slot (both resolved through
//the OverrideScope). The flavor store mirrors the declaration-side
//arms (EmitCapturedFieldStore — the field flags carry that mirror).
void VmBackend::EmitCallSiteSnapshotMembers(SnClassDecl& display,
    uint16_t resultOffset, BytecodeEmitter& emitter)
{
    for (auto& member : display.Members()) {
        if (member.Kind() != NK_ClassField
            || member.ContainFlags(NF_Static))
            continue;
        auto& snapField = static_cast<SnClassField&>(member);
        NLANG_ASSERT(snapField.Name() != "$outer"
            && "the call-site display is the chain root");
        const int fieldOff = FindClassFieldOffset(display,
            snapField.Name());
        NLANG_ASSERT(fieldOff >= 0);
        uint16_t srcSlot = 0;
        if (snapField.Name() == "$this") {
            auto hit = LookupThisOverride();
            NLANG_ASSERT(hit.first
                && "receiver capture without a staged call receiver");
            srcSlot = hit.second;
        } else {
            NLANG_ASSERT(snapField.CaptureSource()
                && "display field neither capture mirror nor $-slot");
            auto hit = LookupOverride(snapField.CaptureSource()->Name());
            NLANG_ASSERT(hit.first
                && "snapshot capture whose formal is unstaged here");
            srcSlot = hit.second;
        }
        EmitCapturedFieldStore(snapField, resultOffset,
            static_cast<uint16_t>(fieldOff), srcSlot, emitter);
    }
}

} //namespace nlang
