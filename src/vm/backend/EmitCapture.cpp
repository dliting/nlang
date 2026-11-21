/*---
    EmitCapture.cpp — 0.13.10 Task 6: lambda capture emission, frame
    machinery half. The display-instance frame prologue halves, the
    receiver redirect for synthetic capture frames, the chased-cell
    load/store helpers, the captured identifier reads, and the lambda
    literal's OP_MakeBoundFunc emission. The storage mutation arms
    (assign / compound / reference binding / declarator) live in
    EmitCaptureStorage.cpp. The analysis
    (builder/LambdaCaptureAnalysis.cpp) synthesized the display classes
    and moved the bodies; this file is everything the back end adds.
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>
#include <vector>

namespace nlang {

namespace {

//Own instance-field scan over a display class (baseless by synthesis, so
//the ancestor walk of the shared helpers is wasted work; the name-keyed
//FindField would also answer methods).
SnClassField* DisplayOwnFieldOf(SnClassDecl& cls, const char* name)
{
    for (auto& member : cls.Members())
        if (member.Kind() == NK_ClassField
            && !member.ContainFlags(NF_Static)
            && member.Name() == name)
            return static_cast<SnClassField*>(&member);
    return nullptr;
}

//Collect the display-bearing paragraphs of a body subtree in document
//order. Document order is the frame layout order: a display's parent is
//always an enclosing paragraph, so the prologue allocates parents before
//children and the "$outer" init finds its target local already present.
void CollectDisplayParas(SnParagraph& para, std::vector<SnParagraph*>& out)
{
    if (para.DisplayClass())
        out.push_back(&para);
    for (auto& child : para.Children())
        if (auto* pInner = dynamic_cast<SnParagraph*>(&child))
            CollectDisplayParas(*pInner, out);
}

} //namespace

//The display field behind a captured descriptor — the resolver stamped
//both halves (NF_Captured on the descriptor, the field pointer beside
//the ownership list). Locals and lambda parameters share the
//NK_FormalParam kind; the lightweight descriptor answers the dynamic
//type first.
SnClassField* VmBackend::CapturedDisplayFieldOf(SnField* field)
{
    if (auto* pLocal = dynamic_cast<SnLocalVar*>(field))
        return pLocal->DisplayField();
    if (auto* pFormal = dynamic_cast<SnFormalParam*>(field))
        return pFormal->DisplayField();
    NLANG_ASSERT(false && "captured descriptor is neither local nor formal");
    return nullptr;
}

//True while emitting inside a $lambda_N synthetic frame: the current
//class is a generated display class (only the capture analysis stamps
//NF_Generated on classes — the $staticInit functions carry it, but they
//are NK_Function and never set m_pCurrClass).
bool VmBackend::InSyntheticCaptureFrame() const
{
    return m_pCurrClass && m_pCurrClass->ContainFlags(NF_Generated);
}

//Load the implicit receiver into a slot. Synthetic capture frames read
//the anchor display's "$this" field (local 0 is the anchor instance, and
//the analysis' propagation stamps the field on every ancestor a capture
//inside needs — the load is only ever emitted when the field exists).
//Real frames keep the implicit-this shape with its null check.
void VmBackend::EmitReceiverLoad(uint16_t dstOffset,
    BytecodeEmitter& emitter)
{
    emitter.Emit(OpCode::OP_VarLocal);
    emitter.EmitUint16(ImplicitThisSlot());
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(dstOffset);
    if (!InSyntheticCaptureFrame()) {
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(dstOffset);
        return;
    }
    NLANG_ASSERT(m_pCurrClass);
    const int thisOff = FindClassFieldOffset(*m_pCurrClass, "$this");
    NLANG_ASSERT(thisOff >= 0 && "receiver read without a \"$this\" field");
    emitter.Emit(OpCode::OP_LoadField);
    emitter.EmitUint16(dstOffset);
    emitter.EmitUint16(dstOffset);
    emitter.EmitUint16(static_cast<uint16_t>(thisOff));
}

//Load the record that physically holds a captured field's storage: the
//display instance for DisplayField, or local 0 chased through
//`outerHops` "$outer" fields for CaptureField. A promoted capture
//chases one more level — its storage is the box payload, and the field
//cell holds the box handle — so every flavor arm downstream addresses
//a single base (the payload at kHeapCellBytes, the same offset the
//local promoted arms use). pField is the captured descriptor or the
//display field (the flavor flags mirror onto both); null only where
//the caller already rejected the flavored captures.
void VmBackend::EmitCaptureCellLoad(uint16_t dstOffset,
    const BareIdTarget& target, SnField* pField, BytecodeEmitter& emitter)
{
    if (target.kind == BareIdTarget::DisplayField) {
        NLANG_ASSERT(target.outerHops == 0);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(target.localOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(dstOffset);
    } else {
        NLANG_ASSERT(target.kind == BareIdTarget::CaptureField);
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(0);   //local 0 = the anchor display instance
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(dstOffset);
        NLANG_ASSERT(m_pCurrClass && m_pCurrClass->ContainFlags(NF_Generated));
        SnClassDecl* pCls = m_pCurrClass;
        for (int hop = 0; hop < target.outerHops; ++hop) {
            const int outerOff = FindClassFieldOffset(*pCls, "$outer");
            NLANG_ASSERT(outerOff >= 0 && "$outer chain does not reach the owner");
            emitter.Emit(OpCode::OP_LoadField);
            emitter.EmitUint16(dstOffset);
            emitter.EmitUint16(dstOffset);
            emitter.EmitUint16(static_cast<uint16_t>(outerOff));
            auto* pOuterField = DisplayOwnFieldOf(*pCls, "$outer");
            NLANG_ASSERT(pOuterField);
            auto* pParent = dynamic_cast<SnClassDecl*>(
                pOuterField->EvalDataType());
            NLANG_ASSERT(pParent && "$outer names a non-class owner");
            pCls = pParent;
        }
    }
    if (pField && pField->ContainFlags(NF_Promoted)) {
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
    }
}

//Store a value (already in valueSlot) into a captured field's cell.
//Struct-typed captures copy in place into the born record (the display
//instance's struct fields are established from birth by OP_New's
//allocation recursion); every other kind is an 8-byte cell store.
void VmBackend::EmitCaptureCellStore(SnField* pDisplayField, int fieldOff,
    uint16_t cellOwnerSlot, uint16_t valueSlot, BytecodeEmitter& emitter)
{
    EmitStructAwareStoreField(pDisplayField->EvalDataType(), cellOwnerSlot,
        static_cast<uint16_t>(fieldOff), valueSlot, emitter);
}

//Identifier read through captured storage. The flavor dispatch mirrors
//EmitRefLocalRead's shapes, re-hosted over a chased cell: the promoted
//payload sits at cell 1 of the box, a reference pair dereferences
//through OP_LoadRef after staging the pair into a frame slot (the deref
//ops read pairs from frame slots, not field cells), an aggregate-typed
//reference keeps the plain pair copy, and a value capture is a plain
//field read.
void VmBackend::EmitCapturedRead(SnField* field, const BareIdTarget& target,
    uint16_t resultOffset, BytecodeEmitter& emitter)
{
    auto* pDisplayField = CapturedDisplayFieldOf(field);
    EvalAreaClaim cellClaim(*this, 1);
    uint16_t cellSlot = cellClaim.base();
    EmitCaptureCellLoad(cellSlot, target, pDisplayField, emitter);
    if (pDisplayField->ContainFlags(NF_Ref)) {
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(cellSlot);   //the pair staged here
        emitter.EmitUint16(cellSlot);
        emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
        if (IsTypeArgRefFormal(pDisplayField)) {
            emitter.Emit(OpCode::OP_LoadRef_T);
            emitter.EmitByte(TypeParamOrdinalOfType(
                *pDisplayField->EvalDataType()));
        } else if (IsDerefRefDescriptor(pDisplayField)) {
            emitter.Emit(OpCode::OP_LoadRef);
            emitter.EmitByte(BoxTypeTagOfKind(
                pDisplayField->EvalDataType()->Kind()));
        } else {
            //Aggregate-typed reference: the pair IS the value — copy
            //the staged pair into resultOffset (the local analog's
            //caller reads the binding's own frame slot; a captured
            //pair lives in the display field, so this arm publishes
            //the copy itself). Downstream field ops read the pair's
            //base half as a handle slot (EmitRefLocalRead's contract).
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(cellSlot);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(resultOffset);
            return;
        }
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(cellSlot);
        return;
    }
    if (pDisplayField->ContainFlags(NF_Promoted)) {
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(cellSlot);
        emitter.EmitUint16(kHeapCellBytes);
        return;
    }
    //Value capture.
    emitter.Emit(OpCode::OP_LoadField);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(cellSlot);
    emitter.EmitUint16(static_cast<uint16_t>(target.fieldOff));
}

//Frame prologue half 1: one display instance per marked paragraph in the
//body subtree (uniform rule — the body root itself included), allocated
//under the display class name (RTK_Class gives the slot its GC heap
//band), constructed by OP_New, then its "$this"/"$outer" fields filled
//when present. Field presence IS the flag: the analysis only synthesizes
//the fields a capture subtree needs.
void VmBackend::EmitCaptureDisplayPrologue(SnParagraph& body,
    BytecodeEmitter& emitter)
{
    std::vector<SnParagraph*> markedParas;
    CollectDisplayParas(body, markedParas);
    for (auto* pPara : markedParas) {
        auto* pClass = pPara->DisplayClass();
        NLANG_ASSERT(pClass && pClass->ContainFlags(NF_Generated));
        const uint16_t displaySlot = AllocLocal(pClass->Name(),
            kFrameSlotBytes, RTK_Class, false);
        emitter.Emit(OpCode::OP_New);
        emitter.EmitUint16(displaySlot);
        emitter.EmitUint16(static_cast<uint16_t>(ClassSlotFor(*pClass)));
        emitter.EmitUint16(DescriptorIndexFor(*pClass));
        const int thisOff = FindClassFieldOffset(*pClass, "$this");
        if (thisOff >= 0) {
            EmitReceiverLoad(m_currFunc->tempSlot, emitter);
            emitter.Emit(OpCode::OP_StoreField);
            emitter.EmitUint16(displaySlot);
            emitter.EmitUint16(static_cast<uint16_t>(thisOff));
            emitter.EmitUint16(m_currFunc->tempSlot);
        }
        const int outerOff = FindClassFieldOffset(*pClass, "$outer");
        if (outerOff >= 0) {
            EmitDisplayOuterInit(displaySlot, outerOff, *pClass, emitter);
        }
    }
}

//"$outer" field init of one display instance: the parent display
//instance slot — a same-frame display is a local under its class name
//(allocated earlier — parents precede children in document order);
//across the frame boundary, local 0 carries the enclosing lambda's
//anchor.
void VmBackend::EmitDisplayOuterInit(uint16_t displaySlot, int outerOff,
    SnClassDecl& cls, BytecodeEmitter& emitter)
{
    auto* pOuterField = DisplayOwnFieldOf(cls, "$outer");
    NLANG_ASSERT(pOuterField);
    auto* pParentClass = dynamic_cast<SnClassDecl*>(
        pOuterField->EvalDataType());
    NLANG_ASSERT(pParentClass && "$outer names a non-class owner");
    auto it = m_currFunc->localOffsets.find(pParentClass->Name());
    uint16_t parentSlot = 0;
    if (it != m_currFunc->localOffsets.end())
        parentSlot = it->second;
    else
        NLANG_ASSERT(InSyntheticCaptureFrame()
            && "$outer target missing from a real frame");
    emitter.Emit(OpCode::OP_StoreField);
    emitter.EmitUint16(displaySlot);
    emitter.EmitUint16(static_cast<uint16_t>(outerOff));
    emitter.EmitUint16(parentSlot);
}

//Frame prologue half 2: captured formals copy from their frame slots
//into the display fields (the analysis mirrored each capture onto the
//rebuilt formal). Flavor dispatch: a promoted formal wraps the raw
//argument into the box the field owns (the frame slot keeps the raw
//value, dead after the copy); a reference formal's pair is an 8-byte
//cell copy; a value formal goes through the struct-aware store (in
//place for struct-typed captures, born records from OP_New).
void VmBackend::EmitCapturedFormalCopies(SnFunction& func,
    BytecodeEmitter& emitter)
{
    for (auto& param : func.Params()) {
        if (!param.ContainFlags(NF_Captured))
            continue;
        EmitCapturedFormalCopy(param, emitter);
    }
}

//One captured formal's prologue copy (EmitCapturedFormalCopies' body —
//one flavor dispatch per parameter).
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
    const uint16_t paramSlot = FindLocal(param.Name());
    if (param.ContainFlags(NF_Promoted)) {
        //VarLocal; Box; park the handle in the shared temp — no
        //nested emission between (the temps' two-op contract); the
        //raw value in the param slot rides its kind's heap GC arm
        //across the Box allocation (over-mark, same argument as
        //EmitPromotedValueWrap).
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(paramSlot);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(BoxTypeTagOfKind(
            param.EvalDataType()->Kind()));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(m_currFunc->tempSlot);
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(displaySlot);
        emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
        emitter.EmitUint16(m_currFunc->tempSlot);
    } else if (param.ContainFlags(NF_Ref)) {
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(displaySlot);
        emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
        emitter.EmitUint16(paramSlot);
    } else {
        EmitStructAwareStoreField(pDisplayField->EvalDataType(),
            displaySlot, static_cast<uint16_t>(fieldOff), paramSlot,
            emitter);
    }
}

//Lambda literal: bind the anchor display instance (a frame local under
//the display class name — the anchor paragraph is always inside the
//literal's own emission frame) and the synthetic method into a delegate
//handle — the EmitMemberFuncHandleRef shape (receiver-first, pResult
//refresh, OP_MakeBoundFunc, assign).
void VmBackend::Access(SnLambdaExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
    auto* pDisplay = expr.DisplayClass();
    auto* pMethod = expr.SyntheticMethod();
    NLANG_ASSERT(pDisplay && pDisplay->ContainFlags(NF_Generated)
        && pMethod && "lambda literal without its synthesis write-backs");
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

} //namespace nlang
