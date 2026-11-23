/*---
    EmitExprRef.cpp — v0.13.9 phase 3 Tasks 5/6: the MakeRef emission
    family. One (base record, cell) pair builder serves the consumption
    sites of the shared judgment (RefBindJudgment.cpp): a &-formal
    argument at a call site and a &-binding initializer. Identifier,
    member-chain, member-call, invoke, subscript (array and container
    cores) and rvalue arms — the rvalue contract binds a fresh
    temporary record and writes to it are lost (spec §3.9). Task 6:
    &-returning callees pass the raw pair through the call boundary.
    Split from EmitStmtRef.cpp per the source-size guard.
---*/
#include "VmBackend.h"
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnData.h>
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/NodeConsts.h>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

namespace nlang {

//Identifier arm. The resolver has already judged the target (position
//family, exact type, promotion marking), so the arms below are closed
//over the accepted shapes:
//  - NF_Ref target: the slot already holds a pair — a raw 8-byte copy
//    preserves the cell offset (pass-through, no rebinding).
//  - NF_Promoted target: RefOfPromoted names the stable box's payload.
//  - plain struct/class local: RefOfLocal names the record itself.
//A non-promoted scalar here means the promotion marking was skipped —
//an internal inconsistency, failed loudly (the raw value would corrupt
//the heap as a "handle").
void VmBackend::EmitMakeRefOfIdentifier(uint16_t dstOffset,
    SnIdentifierExpr& idExpr, BytecodeEmitter& emitter)
{
    auto* pTarget = idExpr.Field();
    NLANG_ASSERT(pTarget != nullptr);
    auto target = ResolveBareIdentifier(pTarget);
    if (target.kind == BareIdTarget::Local) {
        EmitLocalMakeRef(dstOffset, idExpr, emitter);
        return;
    }
    if (target.kind == BareIdTarget::DisplayField
        || target.kind == BareIdTarget::CaptureField) {
        //0.13.10 Task 6: chased display storage (EmitCaptureStorage.cpp).
        EmitCapturedMakeRef(dstOffset, idExpr, target, emitter);
        return;
    }
    if (target.kind == BareIdTarget::ThisField) {
        //Bare class/struct field inside a method: the __this record
        //hosts the field cells (slot 0, allocated first). Task 13: the
        //pair shape follows the field's type through the shared throat;
        //the shared temp parks the struct deref (no eval-area growth —
        //the walkers' identifier mirror stays zero).
        EmitFieldRefBind(idExpr.Field()->EvalDataType(), dstOffset, 0,
            target.fieldOff / kHeapCellBytes, m_currFunc->tempSlot,
            emitter);
        return;
    }
    throw std::runtime_error(
        "NLang backend: reference binding to a static or unresolved "
        "slot reached emission (resolver gate missed)");
}

//Local arm of EmitMakeRefOfIdentifier: the reference to a frame local.
//A reference local passes its pair through (an 8-byte copy preserves
//the alias); a promoted local pairs the stable box's payload; a scalar
//means the promotion marking was skipped — failed loudly (the raw
//value would corrupt the heap as a "handle"); a struct/class/array
//local pairs the record itself.
void VmBackend::EmitLocalMakeRef(uint16_t dstOffset,
    SnIdentifierExpr& idExpr, BytecodeEmitter& emitter)
{
    auto* pTarget = idExpr.Field();
    auto target = ResolveBareIdentifier(pTarget);
    if (pTarget->ContainFlags(NF_Ref)) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(target.localOffset);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(dstOffset);
        return;
    }
    if (pTarget->ContainFlags(NF_Promoted)) {
        emitter.Emit(OpCode::OP_RefOfPromoted);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(target.localOffset);
        return;
    }
    auto* pType = pTarget->EvalDataType();
    NLANG_ASSERT(pType != nullptr);
    if (IsPrimitiveType(pType->Kind()) || pType->Kind() == NK_EnumDecl)
        throw std::runtime_error(
            "NLang backend: scalar reference argument on an "
            "un-promoted local (promotion marking missed): " +
            idExpr.Name() + " in " + m_currFunc->func->name);
    emitter.Emit(OpCode::OP_RefOfLocal);
    emitter.EmitUint16(dstOffset);
    emitter.EmitUint16(target.localOffset);
}

//Compile-time cell offset of one field within its owner record: the
//field ops' byte offsets (FindFieldOffset/FindClassFieldOffset) mapped
//to heap cells — byref pairs address cells directly.
int VmBackend::RefFieldCellOffset(SyntaxNode& ownerDecl, SnField& field)
{
    const auto kind = ownerDecl.Kind();
    if (kind == NK_StructDecl)
        return FindFieldOffset(static_cast<SnStructDecl&>(ownerDecl),
                   field.Name()) / kHeapCellBytes;
    if (kind == NK_ClassDecl)
        return FindClassFieldOffset(static_cast<SnClassDecl&>(ownerDecl),
                   field.Name()) / kHeapCellBytes;
    throw std::runtime_error(
        "NLang backend: reference member chain through a non-record "
        "owner (resolver gate missed)");
}

//Task 13: classify one field bind's pair shape from the final hop's
//use-site type node. Scalar/enum/string storage pairs its cell (the
//deref ops address the cell); a struct field's cell names a record —
//the bind derefs it and pairs the record itself (the offset-0 struct
//invariant, the container-get core's shape); identity types take the
//equivalent-notation ruling — the plain handle read, no pair; a
//type-parameter placeholder defers to the runtime kind
//(OP_RefOfField_T, the OP_LoadRef_T/OP_StoreRef_T family).
VmBackend::FieldRefShape VmBackend::ShapeOfFieldBind(
    const SyntaxNode* pType)
{
    if (!pType)
        return FieldRefShape::Cell;
    auto* pField = static_cast<const SnField*>(pType);
    if (TypeParamOrdinalOfType(*const_cast<SnField*>(pField))
            != SnTypeNameExpr::kNoTypeParamOrdinal)
        return FieldRefShape::Descriptor;
    const NodeKind kind = pField->Kind();
    if (IsPrimitiveType(kind) || kind == NK_EnumDecl)
        return FieldRefShape::Cell;
    if (kind == NK_StructDecl)
        return FieldRefShape::Record;
    return FieldRefShape::Handle;
}

//Task 13: one emission per shape — the throat both field-bind sites
//route through (one ruling, one implementation; the member-chain arm
//of Tasks 5/6 paired every kind as a cell, which corrupted struct and
//identity owners — an 8-byte store_field through the wrong pair
//overwrote the owner's classIdx and field cells). cellOff is in heap
//cells; the LoadField derefs convert to byte offsets. scratchSlot is
//the Record arm's deref landing (the chain reuses its walk slot, the
//ThisField arm the shared temp — two adjacent ops, nothing between,
//and the walkers' claim mirrors stay untouched).
void VmBackend::EmitFieldRefBind(const SyntaxNode* pType,
    uint16_t dstOffset, uint16_t baseSlot, int cellOff,
    uint16_t scratchSlot, BytecodeEmitter& emitter)
{
    switch (ShapeOfFieldBind(pType)) {
    case FieldRefShape::Cell:
        emitter.Emit(OpCode::OP_RefOfField);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(baseSlot);
        emitter.EmitUint16(static_cast<uint16_t>(cellOff));
        return;
    case FieldRefShape::Descriptor: {
        auto* pField = static_cast<const SnField*>(pType);
        emitter.Emit(OpCode::OP_RefOfField_T);
        emitter.EmitByte(TypeParamOrdinalOfType(
            *const_cast<SnField*>(pField)));
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(baseSlot);
        emitter.EmitUint16(static_cast<uint16_t>(cellOff));
        return;
    }
    case FieldRefShape::Record:
        //Deref the cell, then pair the record it names.
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(scratchSlot);
        emitter.EmitUint16(baseSlot);
        emitter.EmitUint16(static_cast<uint16_t>(
            cellOff * kHeapCellBytes));
        emitter.Emit(OpCode::OP_RefOfLocal);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(scratchSlot);
        return;
    case FieldRefShape::Handle:
        //Equivalent notation: the binding names the object — the plain
        //field read puts the handle in the destination slot.
        emitter.Emit(OpCode::OP_LoadField);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(baseSlot);
        emitter.EmitUint16(static_cast<uint16_t>(
            cellOff * kHeapCellBytes));
        return;
    }
}

//Collect a member chain final-hop-first (member.Field() is the resolved
//descriptor of the member-name side; Outer() is the object expression
//the walk descends into) and return the chain's root expression.
SnExpression* VmBackend::CollectMemberChain(SnMemberExpr& member,
    std::vector<SnField*>& chain)
{
    SnExpression* pRoot = &member;
    while (pRoot->Kind() == NK_MemberExpr) {
        auto& hop = static_cast<SnMemberExpr&>(*pRoot);
        chain.push_back(hop.Field());
        pRoot = hop.Outer();
    }
    NLANG_ASSERT(!chain.empty() && chain.back() != nullptr);
    return pRoot;
}

//Member-chain arm: walk record-to-record from the root expression's
//handle, the final hop naming the pair. The root may be any expression
//evaluating to a record handle — a struct/class local, another field,
//an element record, or an rvalue result (its temp record absorbs the
//writes, lost — the documented contract).
void VmBackend::EmitMakeRefOfMemberChain(uint16_t dstOffset,
    SnMemberExpr& member, BytecodeEmitter& emitter)
{
    std::vector<SnField*> chain;
    SnExpression* pRoot = CollectMemberChain(member, chain);
    EvalAreaClaim claim(*this, 1);
    uint16_t walkSlot = claim.base();
    EmitExpression(*pRoot, emitter, walkSlot);
    //A class-typed root rides a heap handle that can be null — the
    //same guard the member reads take before their first field walk.
    auto* pRootType = pRoot->EvalDataType();
    NLANG_ASSERT(pRootType != nullptr);
    if (pRootType->Kind() == NK_ClassDecl) {
        emitter.Emit(OpCode::OP_NullCheck);
        emitter.EmitUint16(walkSlot);
    }
    //Each hop's owner comes from the field's declaring decl (Parent),
    //not EvalDataType — immune to the array-element dispatch trap.
    //Task 13: the final hop's pair shape follows the MEMBER EXPR's own
    //use-site type (the resolver substitutes instantiations, so a
    //generic owner's T field binds with the concrete kind outside the
    //body; inside the body the placeholder routes to the descriptor
    //op, whose executing frame carries the kind).
    for (size_t i = chain.size(); i-- > 0; ) {
        auto* pField = chain[i];
        NLANG_ASSERT(pField != nullptr);
        auto* pOwnerDecl = OwningTypeOfMemberField(pField);
        NLANG_ASSERT(pOwnerDecl != nullptr);
        const int cellOff = RefFieldCellOffset(*pOwnerDecl, *pField);
        if (i == 0) {
            EmitFieldRefBind(member.EvalDataType(), dstOffset, walkSlot,
                cellOff, walkSlot, emitter);
        } else {
            //Intermediate hop: the field cell holds the sub-record's
            //handle; keep walking from it.
            emitter.Emit(OpCode::OP_LoadField);
            emitter.EmitUint16(walkSlot);
            emitter.EmitUint16(walkSlot);
            emitter.EmitUint16(static_cast<uint16_t>(
                cellOff * kHeapCellBytes));
        }
    }
}

//Container & read (Task 6): subscript sugar `pts[0]` and the explicit
//`pts.get(0)` spelling share this core. The get() call stages through
//the shared helper (result in the accumulator, raw — no return unbox),
//then the & tail pair-builds: a primitive element's boxed record holds
//the payload at cell 1 (OP_RefOfPromoted); a struct/class element IS a
//record (OP_RefOfLocal, cell 0). A Dict miss raises the same runtime
//error as the value read.
void VmBackend::EmitMakeRefOfContainerGet(uint16_t dstOffset,
    SnExpression& receiver, SnExpression& index, BytecodeEmitter& emitter)
{
    ArgBoxPlan keyBox;
    ArgBoxPlan valBox;
    ClassifyContainerGet(receiver, keyBox, valBox);
    EvalAreaClaim claim(*this, 2);
    StageContainerGetCall(receiver, index, keyBox, claim.base(), emitter);
    //The accumulator holds the raw get() result: park it in claim[0]
    //(the receiver slot — consumed by the call) and pair-build.
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(claim.base());
    if (valBox.needsBox) {
        emitter.Emit(OpCode::OP_RefOfPromoted);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(claim.base());
    } else {
        emitter.Emit(OpCode::OP_RefOfLocal);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(claim.base());
    }
    emitter.Emit(OpCode::OP_ParaEnd);
}

//Subscript arm: containers route to the get()-based core — the array
//ref opcodes are invalid on a container record. Plain arrays keep the
//read path's staging shape (array + indices in one exclusive claim),
//then the ref opcodes. Rank-1 binds through OP_RefOfElement (the single
//index IS the flat address); rank-N binds through OP_RefOfElementN —
//the runtime computes the flat address from the staged indices, so
//static tokens and dynamic shapes bind alike. Slices never reach here
//(the resolver rejects them).
void VmBackend::EmitMakeRefOfSubscript(uint16_t dstOffset,
    SnSubscriptExpr& sub, BytecodeEmitter& emitter)
{
    if (IsContainerSubscript(*sub.Array())) {
        EmitMakeRefOfContainerGet(dstOffset, *sub.Array(), *sub.Index(),
            emitter);
        return;
    }
    const size_t rank = sub.Indices().size();
    EvalAreaClaim claim(*this, rank + 1);
    uint16_t claimBase = claim.base();
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    for (size_t d = 0; d < rank; ++d)
        EmitExpression(*sub.Indices()[d], emitter,
            claimBase + (d + 1) * kFrameSlotBytes);
    if (rank == 1) {
        emitter.Emit(OpCode::OP_RefOfElement);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(claimBase);
        emitter.EmitUint16(claimBase + kFrameSlotBytes);
        return;
    }
    emitter.Emit(OpCode::OP_RefOfElementN);
    emitter.EmitUint16(dstOffset);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(claimBase + kFrameSlotBytes);
    emitter.EmitUint16(static_cast<uint16_t>(rank));
}

//&-returning invoke: the callee's return slot holds the raw
//(record, cell) pair — stage the call straight into the destination
//(bRawRefResult suppresses the value-side normalization). Everything
//else is an rvalue: the documented temporary-record contract.
void VmBackend::EmitMakeRefOfInvoke(uint16_t dstOffset,
    SnInvokeExpr& invoke, BytecodeEmitter& emitter)
{
    auto* pCallee = invoke.Callee();
    if (pCallee && pCallee->ReturnType()
        && pCallee->ReturnType()->ContainFlags(NF_Ref)) {
        EmitBareInvokeShape(invoke, dstOffset, /*bRawRefResult=*/true);
        return;
    }
    if (!pCallee && DelegateKeyHasRefReturn(invoke)) {
        //Task 8: a delegate key with a & return — the dispatch parks
        //the raw pair (same MakeRef contract as a &-returning direct
        //callee; argument-side staging runs in EmitUnresolvedInvokeArgs).
        EmitBareInvokeShape(invoke, dstOffset, /*bRawRefResult=*/true);
        return;
    }
    EmitMakeRefOfRvalue(dstOffset, invoke, emitter);
}

//Member-call shape in a & context (Task 6): three routes.
//  - a &-returning class method: the ladder threads bRawRefResult so
//    the raw pair lands in the destination (write-through);
//  - the builtin container get: the same core as the subscript sugar;
//  - anything else: rvalue (temporary record, writes lost).
//Field-read chains fall through to the member-chain arm below.
void VmBackend::EmitMakeRefOfMemberCall(uint16_t dstOffset,
    SnMemberExpr& member, BytecodeEmitter& emitter)
{
    auto* pInner = member.Inner();
    if (pInner && pInner->Kind() == NK_InvokeExpr) {
        auto& invoke = static_cast<SnInvokeExpr&>(*pInner);
        //Container get first: the receiver shape identifies the route —
        //the callee resolves to the containers.n declaration whose
        //instantiation does not carry the & mark (generic synthesis,
        //phase 3 Task 8), so the mark is no discriminator here.
        if (invoke.CalleeName() == kBuiltinGetMethodName
            && IsContainerSubscript(*member.Outer())) {
            SnExpression* pIndex = nullptr;
            for (auto& param : invoke.Params()) {
                pIndex = &param;
                break;
            }
            if (pIndex) {
                EmitMakeRefOfContainerGet(dstOffset, *member.Outer(),
                    *pIndex, emitter);
                return;
            }
        }
        auto* pCallee = invoke.Callee();
        //Task 8: a direct &-returning callee and a delegate key with
        //a & return (Callee() null) share the pass-through arm — the
        //resolver's MemberCallBindsStorage gate admits exactly these
        //two shapes.
        const bool bRawRefPair = pCallee
            ? (pCallee->ReturnType()
                && pCallee->ReturnType()->ContainFlags(NF_Ref))
            : DelegateKeyHasRefReturn(invoke);
        if (bRawRefPair) {
            auto* pOuterType = member.Outer()->EvalDataType();
            NLANG_ASSERT(pOuterType != nullptr);
            if (pOuterType->Kind() != NK_ClassDecl)
                throw std::runtime_error(
                    "NLang backend: reference-returning call on a "
                    "non-class receiver (declaration gate missed)");
            EmitMemberTypedReceiverDispatch(member,
                static_cast<SnClassDecl*>(pOuterType), emitter,
                dstOffset, /*bRawRefResult=*/true);
            return;
        }
        EmitMakeRefOfRvalue(dstOffset, member, emitter);
        return;
    }
    EmitMakeRefOfMemberChain(dstOffset, member, emitter);
}

//Rvalue arm: everything the position family does not name — literals,
//binary results, creation/invoke results, identity `as` — binds a
//fresh temporary record. Scalars/strings/enum rvalues box (the same
//stable-record shape the promotion machinery uses); struct rvalues
//bind the created record directly. Writes land in the temp and are
//lost; reads see the fresh value (spec §3.9, documented).
void VmBackend::EmitMakeRefOfRvalue(uint16_t dstOffset,
    SnExpression& expr, BytecodeEmitter& emitter)
{
    EvalAreaClaim claim(*this, 1);
    uint16_t tempSlot = claim.base();
    auto* pType = expr.EvalDataType();
    NLANG_ASSERT(pType != nullptr);
    EmitExpression(expr, emitter, tempSlot);
    const NodeKind kind = pType->Kind();
    if (IsPrimitiveType(kind) || kind == NK_EnumDecl || kind == NK_String) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(tempSlot);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(BoxTypeTagOfKind(kind));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(tempSlot);
        emitter.Emit(OpCode::OP_RefOfPromoted);
        emitter.EmitUint16(dstOffset);
        emitter.EmitUint16(tempSlot);
        return;
    }
    emitter.Emit(OpCode::OP_RefOfLocal);
    emitter.EmitUint16(dstOffset);
    emitter.EmitUint16(tempSlot);
}

//Dispatcher: the pair builder every & consumption site shares.
void VmBackend::EmitMakeRef(uint16_t dstOffset, SnExpression& expr,
    BytecodeEmitter& emitter)
{
    switch (expr.Kind()) {
    case NK_IdentifierExpr:
        EmitMakeRefOfIdentifier(dstOffset,
            static_cast<SnIdentifierExpr&>(expr), emitter);
        return;
    case NK_MemberExpr:
        EmitMakeRefOfMemberCall(dstOffset,
            static_cast<SnMemberExpr&>(expr), emitter);
        return;
    case NK_InvokeExpr:
        EmitMakeRefOfInvoke(dstOffset,
            static_cast<SnInvokeExpr&>(expr), emitter);
        return;
    case NK_SubscriptExpr:
        EmitMakeRefOfSubscript(dstOffset,
            static_cast<SnSubscriptExpr&>(expr), emitter);
        return;
    default:
        EmitMakeRefOfRvalue(dstOffset, expr, emitter);
        return;
    }
}

} //namespace nlang
