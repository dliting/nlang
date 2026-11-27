/*---
    EmitCapture.cpp — 0.13.10 Task 6: lambda capture emission, frame
    machinery half. The display-instance frame prologue halves, the
    receiver redirect for synthetic capture frames, the chased-cell
    load/store helpers, the captured identifier reads, and the loop
    display activation family (per-iteration freshness, Major-3 lift:
    flavor-dispatched head carry). The storage mutation arms
    (assign / compound / reference binding / declarator) live in
    EmitCaptureStorage.cpp; the synthetic frame's own two moments
    (captured-formal prologue copies, the lambda literal) in
    EmitCaptureFrame.cpp. The analysis
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

//Collect display-bearing paragraphs from the complete statement subtree,
//stopping at the frame boundary. Loop and branch bodies sit below their
//statement nodes rather than directly under the enclosing paragraph, so
//walking only paragraph children misses their display and leaves the
//backend without its local slot. A lambda's body emits in its own
//synthetic frame, so its display paragraphs belong to THAT frame's
//prologue — descending here would allocate and construct them twice.
void CollectDisplayParas(Node &node, std::vector<SnParagraph*> &out)
{
    if (node.Kind() == NK_LambdaExpr)
        return;
    if (auto *pPara = dynamic_cast<SnParagraph*>(&node)) {
        if (pPara->DisplayClass())
            out.push_back(pPara);
    }
    for (auto &child : node.Children())
        CollectDisplayParas(child, out);
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

//Construct one display instance into its frame slot: OP_New plus the
//"$this"/"$outer" fills a synthesized display carries. Shared by the
//frame prologue and the per-iteration loop activation — one recipe
//keeps the two construction sites from drifting apart.
void VmBackend::EmitDisplayConstruct(SnClassDecl& cls,
    uint16_t displaySlot, BytecodeEmitter& emitter)
{
    emitter.Emit(OpCode::OP_New);
    emitter.EmitUint16(displaySlot);
    emitter.EmitUint16(static_cast<uint16_t>(ClassSlotFor(cls)));
    emitter.EmitUint16(DescriptorIndexFor(cls));
    const int thisOff = FindClassFieldOffset(cls, "$this");
    if (thisOff >= 0) {
        EmitReceiverLoad(m_currFunc->tempSlot, emitter);
        emitter.Emit(OpCode::OP_StoreField);
        emitter.EmitUint16(displaySlot);
        emitter.EmitUint16(static_cast<uint16_t>(thisOff));
        emitter.EmitUint16(m_currFunc->tempSlot);
    }
    const int outerOff = FindClassFieldOffset(cls, "$outer");
    if (outerOff >= 0)
        EmitDisplayOuterInit(displaySlot, outerOff, cls, emitter);
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
    std::vector<SnParagraph*> displayParas;
    CollectDisplayParas(body, displayParas);
    for (auto* pPara : displayParas) {
        auto* pClass = pPara->DisplayClass();
        NLANG_ASSERT(pClass && pClass->ContainFlags(NF_Generated));
        //The slot is part of the frame layout for every display (the
        //activation and capture reads route to it by name; a nested
        //display's "$outer" init looks the parent slot up here too).
        const uint16_t displaySlot = AllocLocal(pClass->Name(),
            kFrameSlotBytes, RTK_Class, false);
        //0.13.10 Task 9 (loop-scoped heads): a loop-owned display skips
        //pre-construction — the head is scoped to the loop statement, so
        //nothing before the loop can reference it; the loop's first
        //activation (the for-init hand-off / foreach entry) constructs
        //the instance before any body read. The unconstructed slot
        //reads as the null sentinel until then.
        if (pPara->LoopActivationOwner() == pPara)
            continue;
        //D-1 (0.13.13a): the synthetic method of the lambda at a
        //formal's default-value root ARRIVES with this display already
        //constructed — the call site built the instance and filled the
        //snapshot fields and "$this" there, and local 0 carries that
        //instance into the frame. Re-running OP_New would drop the
        //payload: alias local 0 into the name-keyed slot instead. The
        //identity test is exact — an arrival is always the current
        //generated class's body-root display, and only the default
        //lambda's method frame is hosted on that class.
        if (pPara == &body && pClass == m_pCurrClass) {
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(0);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(displaySlot);
            continue;
        }
        EmitDisplayConstruct(*pClass, displaySlot, emitter);
    }
}

//Activate all displays owned by one loop body. The old loop display is
//copied into claim slots before OP_New so a for-loop's fini update becomes
//the next iteration's starting value without requiring a hidden local for
//the captured declaration (captured locals intentionally own no frame slot).
void VmBackend::EmitLoopDisplayActivation(SnParagraph& loopBody,
    bool linkPrevious, BytecodeEmitter& emitter)
{
    std::vector<SnParagraph*> displayParas;
    CollectDisplayParas(loopBody, displayParas);
    std::vector<SnParagraph*> owned;
    for (auto *pPara : displayParas)
        if (pPara->LoopActivationOwner() == &loopBody)
            owned.push_back(pPara);
    if (owned.empty())
        return;

    //Value carry-over only when this body hosts captured loop heads (the
    //shared predicate keeps the walker's eval-area mirror in lockstep
    //with this claim). The claim spans both halves — the construct sits
    //in between.
    const bool bLink = linkPrevious && loopBody.HasCapturedLoopHeads();
    const auto capturedHeads = loopBody.CapturedLoopHeads();
    EvalAreaClaim linkClaim(*this,
        bLink ? static_cast<uint16_t>(capturedHeads.size()) : 0);
    if (bLink)
        EmitDisplayHeadCarryIn(loopBody, linkClaim.base(), emitter);

    for (auto *pPara : owned) {
        EmitDisplayConstruct(*pPara->DisplayClass(),
            FindLocal(pPara->DisplayClass()->Name()), emitter);
    }

    if (bLink)
        EmitDisplayHeadCarryOut(loopBody, linkClaim.base(), emitter);
}

//Link half 1: stage the outgoing display's head values into the claim
//slots — runs before the OP_New replaces the instance. Only captured
//heads have display fields; the claim indexes follow the same filtered
//list as the walker mirror and half 2 below. A promoted head stages
//the box PAYLOAD (handle deref in place) — the fresh display wraps a
//new box, so the outgoing iteration's closures and & references keep
//the old box frozen at its final value.
void VmBackend::EmitDisplayHeadCarryIn(SnParagraph& loopBody,
    uint16_t claimBase, BytecodeEmitter& emitter)
{
    auto *pRootClass = loopBody.DisplayClass();
    const uint16_t oldSlot = FindLocal(pRootClass->Name());
    const auto loopHeads = loopBody.CapturedLoopHeads();
    for (size_t i = 0; i < loopHeads.size(); ++i) {
        const int fieldOff = FindClassFieldOffset(*pRootClass,
            loopHeads[i]->Name());
        NLANG_ASSERT(fieldOff >= 0);
        const uint16_t claimSlot =
            claimBase + static_cast<uint16_t>(i * kFrameSlotBytes);
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(claimSlot);
        emitter.EmitUint16(oldSlot);
        emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
        auto *pDisplayField = CapturedDisplayFieldOf(loopHeads[i]);
        if (pDisplayField && pDisplayField->ContainFlags(NF_Promoted)) {
            emitter.Emit(OpCode::OP_LoadField);
            emitter.EmitUint16(claimSlot);
            emitter.EmitUint16(claimSlot);
            emitter.EmitUint16(kHeapCellBytes);
        }
    }
}

//Link half 2: store the staged values into the fresh display's fields.
//A promoted head wraps its staged payload into a FRESH box first —
// copying the field as-is would share one box across iterations and
// every closure would observe the tail value (the freshness defect).
// The claim slot is Dual-classed, so a string payload stays rooted
// across OP_Box's allocation window.
void VmBackend::EmitDisplayHeadCarryOut(SnParagraph& loopBody,
    uint16_t claimBase, BytecodeEmitter& emitter)
{
    auto *pRootClass = loopBody.DisplayClass();
    const uint16_t displaySlot = FindLocal(pRootClass->Name());
    const auto loopHeads = loopBody.CapturedLoopHeads();
    for (size_t i = 0; i < loopHeads.size(); ++i) {
        const int fieldOff = FindClassFieldOffset(*pRootClass,
            loopHeads[i]->Name());
        NLANG_ASSERT(fieldOff >= 0);
        const uint16_t claimSlot =
            claimBase + static_cast<uint16_t>(i * kFrameSlotBytes);
        auto *pDisplayField = CapturedDisplayFieldOf(loopHeads[i]);
        if (pDisplayField && pDisplayField->ContainFlags(NF_Promoted)) {
            emitter.Emit(OpCode::OP_VarLocal);
            emitter.EmitUint16(claimSlot);
            emitter.Emit(OpCode::OP_Box);
            emitter.EmitByte(BoxTypeTagOfKind(
                pDisplayField->EvalDataType()->Kind()));
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(claimSlot);
            emitter.Emit(OpCode::OP_StoreField);
            emitter.EmitUint16(displaySlot);
            emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
            emitter.EmitUint16(claimSlot);
            continue;
        }
        //Reference heads keep the plain 8-byte pair copy — the pair
        //intentionally aliases its target cell across iterations.
        //Value-flavor struct heads store through the struct-aware field
        //store — the plain handle copy aliased ONE record across every
        //iteration's display. Promotion never applies to structs (the
        //primitive/enum grant), so the arms are disjoint by construction.
        if (pDisplayField && !pDisplayField->ContainFlags(NF_Ref)) {
            EmitStructAwareStoreField(pDisplayField->EvalDataType(),
                displaySlot, static_cast<uint16_t>(fieldOff), claimSlot,
                emitter);
        } else {
            emitter.Emit(OpCode::OP_StoreField);
            emitter.EmitUint16(displaySlot);
            emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
            emitter.EmitUint16(claimSlot);
        }
    }
}

//Copy a foreach element loaded into its raw loop slot into the active
//display field. The direct slot write in EmitForeachLoadElement bypasses
//identifier capture routing, so this is the activation boundary. Struct
//heads deep-copy into the display's born record — the plain handle copy
//shared the source record (the container's element), so a write through
//the closure would corrupt the container.
void VmBackend::EmitLoopDisplayHeadCopies(SnParagraph& loopBody,
    BytecodeEmitter& emitter)
{
    if (!loopBody.HasCapturedLoopHeads())
        return;
    auto *pClass = loopBody.DisplayClass();
    const uint16_t displaySlot = FindLocal(pClass->Name());
    for (auto *pHead : loopBody.CapturedLoopHeads()) {
        const int fieldOff = FindClassFieldOffset(*pClass, pHead->Name());
        NLANG_ASSERT(fieldOff >= 0);
        auto *pDisplayField = CapturedDisplayFieldOf(pHead);
        //Reference heads keep the plain pair copy (the pair aliases its
        //target cell); value-flavor struct heads deep-copy into the
        //display's born record.
        if (pDisplayField && !pDisplayField->ContainFlags(NF_Ref)) {
            EmitStructAwareStoreField(pDisplayField->EvalDataType(),
                displaySlot, static_cast<uint16_t>(fieldOff),
                DeclSlotOf(pHead, pHead->Name()), emitter);
        } else {
            emitter.Emit(OpCode::OP_StoreField);
            emitter.EmitUint16(displaySlot);
            emitter.EmitUint16(static_cast<uint16_t>(fieldOff));
            emitter.EmitUint16(DeclSlotOf(pHead, pHead->Name()));
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

} //namespace nlang
