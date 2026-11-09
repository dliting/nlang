/*---
    EmitExprSubscript.cpp — all subscript expression emission: the plain
    array element read (OP_LoadElement family), the v0.12.7 slice
    view mint (OP_MakeArrayView), the List/Dict container-get() sugar,
    and the Access(SnSubscriptExpr) dispatch. The array paths split
    from EmitExprCast.cpp when it reached the source-size budget; the
    container arms joined them in 2026-11-09 so the whole subscript
    family lives in one file.
---*/
#include "VmBackend.h"
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/runtime/Config.h>
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/NodeConsts.h>
#include "EmitPrimOps.h"
#include <array>

namespace nlang {

//v0.12.7: true when the base's static type carries no rank annotation
//(the Array<E> spelling) — the record rank verifies at run time via the
//Dyn element pair; the token spelling keeps the static ops (whose
//rank-mismatch guard stays a compiler invariant).
bool VmBackend::SubscriptNeedsDynOps(const SnExpression& arrExpr) {
    auto* pType = arrExpr.EvalDataType();
    return pType && pType->Kind() != NK_ArrayTypeToken;
}

//v0.12.7 Task 11: any slice item in the index list re-routes the read
//from an element load to a view mint (the resolver has already typed
//the result as an array token of the kept axes).
bool VmBackend::HasSliceAxis(const SnSubscriptExpr& sub) {
    for (auto *pIndex : sub.Indices())
        if (pIndex->Kind() == NK_SliceItemExpr)
            return true;
    return false;
}

void VmBackend::EmitArraySubscriptRead(SnSubscriptExpr& sub,
                                       BytecodeEmitter& emitter,
                                       uint16_t resultOffset) {
    //Stage-2: N indices stage after the receiver in one exclusive
    //claim (rank-1 subscripts hold exactly one — same shape as before).
    const size_t rank = sub.Indices().size();
    EvalAreaClaim claim(*this, rank + 1);
    uint16_t claimBase = claim.base();
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    for (size_t d = 0; d < rank; ++d)
        EmitExpression(*sub.Indices()[d], emitter,
            claimBase + (d + 1) * kFrameSlotBytes);
    OpCode loadOp = SubscriptNeedsDynOps(*sub.Array())
        ? OpCode::OP_LoadElementDyn : OpCode::OP_LoadElementN;
    emitter.Emit(loadOp);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(claimBase + kFrameSlotBytes);  //idxBase
    emitter.EmitUint16(static_cast<uint16_t>(rank));
}

namespace {
//Presence-mask layout shared with the runtime: 4 bits per axis
//(start/stop/step written + axis-is-scalar), little-endian across
//ceil(4*rank/32) int32 slots. INT32_MIN stages as the default sentinel
//for unwritten parts.
constexpr int32_t kSliceDefaultSentinel = INT32_MIN;
constexpr uint32_t kSliceBitStart = 0x1;
constexpr uint32_t kSliceBitStop = 0x2;
constexpr uint32_t kSliceBitStep = 0x4;
constexpr uint32_t kSliceBitScalar = 0x8;
constexpr size_t kSliceMaskWords = (4 * kMaxArrayRank + 31) / 32;
static_assert(4 * kMaxArrayRank <= kSliceMaskWords * 32,
    "the slice mask word array must cover every axis's bits");

//Stage the default sentinel into one parameter slot (const push +
//assign — the same shape the boxing plans use to park a pResult).
void EmitSentinel(BytecodeEmitter& emitter, uint16_t slotOffset) {
    emitter.Emit(OpCode::OP_ConstInt32);
    emitter.EmitInt32(kSliceDefaultSentinel);
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(slotOffset);
}
}  //namespace

//v0.12.7 Task 11: stage ONE subscript axis into its parameter slots
//and OR its presence bits into the mask. A scalar axis rides its start
//slot with the scalar bit; a slice axis stages each written part and
//sentinels each absent one (a part that EVALUATES to INT32_MIN still
//carries its presence bit and is rejected at run time).
void VmBackend::EmitViewAxisParams(SnSubscriptExpr& sub, size_t d,
    uint16_t paramsBase, BytecodeEmitter& emitter, uint32_t* mask) {
    SnExpression* pIndex = sub.Indices()[d];
    const uint16_t axisBase = static_cast<uint16_t>(
        paramsBase + 3 * d * kFrameSlotBytes);
    const uint32_t axisShift = 4 * (d % 8);
    if (pIndex->Kind() != NK_SliceItemExpr)
    {
        //Scalar axis: the index rides the start slot; the unused
        //stop/step slots still stage sentinels (readable slots).
        EmitExpression(*pIndex, emitter, axisBase);
        EmitSentinel(emitter, static_cast<uint16_t>(
            axisBase + kFrameSlotBytes));
        EmitSentinel(emitter, static_cast<uint16_t>(
            axisBase + 2 * kFrameSlotBytes));
        mask[d / 8] |= kSliceBitScalar << axisShift;
        return;
    }
    auto& item = static_cast<SnSliceItemExpr&>(*pIndex);
    SnExpression* parts[] = { item.Head(), item.Stop(), item.Step() };
    const uint32_t partBits[] = {
        kSliceBitStart, kSliceBitStop, kSliceBitStep };
    for (size_t p = 0; p < 3; ++p)
    {
        const uint16_t slot = static_cast<uint16_t>(
            axisBase + p * kFrameSlotBytes);
        if (parts[p])
        {
            EmitExpression(*parts[p], emitter, slot);
            mask[d / 8] |= partBits[p] << axisShift;
        }
        else
            EmitSentinel(emitter, slot);
    }
}

//v0.12.7 Task 11: the slice-subscript emitter. One exclusive claim
//holds the receiver, 3 int32 parameter slots per axis (start/stop/
//step) and the mask words; the mask is a compile-time constant
//(syntactic part presence) so it stages as consts. The view's own type
//row: the resolver typed the result as a token of (element, kept
//axes) — register it exactly as minted.
void VmBackend::EmitArrayViewRead(SnSubscriptExpr& sub,
                                  BytecodeEmitter& emitter,
                                  uint16_t resultOffset) {
    const size_t rank = sub.Indices().size();
    const size_t maskWords = (4 * rank + 31) / 32;
    EvalAreaClaim claim(*this, 1 + 3 * rank + maskWords);
    const uint16_t claimBase = claim.base();
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    const uint16_t paramsBase = claimBase + kFrameSlotBytes;
    std::array<uint32_t, kSliceMaskWords> mask{};
    for (size_t d = 0; d < rank; ++d)
        EmitViewAxisParams(sub, d, paramsBase, emitter,
            mask.data());
    for (size_t w = 0; w < maskWords; ++w)
    {
        emitter.Emit(OpCode::OP_ConstInt32);
        emitter.EmitInt32(static_cast<int32_t>(mask[w]));
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(static_cast<uint16_t>(
            paramsBase + (3 * rank + w) * kFrameSlotBytes));
    }
    auto* pResultType = sub.EvalDataType();
    NLANG_ASSERT(pResultType
        && pResultType->Kind() == NK_ArrayTypeToken);
    auto& token = static_cast<SnArrayTypeToken&>(*pResultType);
    const uint16_t arrayTypeIdx = RegisterArrayType(
        token.ElemTypeOf(), token.Rank());
    emitter.Emit(OpCode::OP_MakeArrayView);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(claimBase);
    emitter.EmitUint16(arrayTypeIdx);
    emitter.EmitUint16(static_cast<uint16_t>(rank));
    emitter.EmitUint16(paramsBase);
}

//Subscript arm: List<T>/Dict<K,V> get() sugar — classify the generic
//instantiation (element/key/value boxing tags), then emit the call.
void VmBackend::EmitContainerSubscriptGet(SnSubscriptExpr& sub,
                                          BytecodeEmitter& emitter,
                                          uint16_t resultOffset) {
    auto* pGenClass = static_cast<SnClassDecl*>(
        sub.Array()->EvalDataType());
    const auto& baseName = pGenClass->BaseName();
    const auto& typeArgs = pGenClass->GenericTypeArgs();
    bool isList = (baseName == kBuiltinListTypeName && !typeArgs.empty());
    bool isDict = (baseName == kBuiltinDictTypeName && typeArgs.size() > 1);
    //Array-typed slots are interned tokens — raw
    //handles, no box/unbox (BoxingTagFor default).
    //Dict keys box when primitive; List's index is int.
    auto keyBox = isDict
        ? BoxingTagFor(typeArgs[0])
        : BoxingTagResult{0, false};
    SnField* pElem = isList ? typeArgs[0]
        : (typeArgs.size() > 1 ? typeArgs[1] : nullptr);
    auto valBox = BoxingTagFor(pElem);
    EmitContainerGetCall(sub, keyBox, valBox, emitter, resultOffset);
}

//Subscript arm: the get() call itself — emits through EvalAreaClaim so
//nested calls (e.g. foo(li[j])) cannot clobber an outer call's
//callParamBase slice. Unboxes the get() return for primitive T/V,
//mirroring the foreach element load and the member-call boxing plans.
void VmBackend::EmitContainerGetCall(SnSubscriptExpr& sub,
                                     BoxingTagResult keyBox,
                                     BoxingTagResult valBox,
                                     BytecodeEmitter& emitter,
                                     uint16_t resultOffset) {
    EvalAreaClaim claim(*this, 2);
    uint16_t claimBase = claim.base();
    //Receiver → claim[0] directly (round-4: NOT via
    //resultOffset — a self-referential read `i = li[i]`
    //would overwrite the index's source slot with the
    //List handle before the index is emitted). Same
    //shape as the array path below; resultOffset is
    //written only by the final get() store.
    EmitExpression(*sub.Array(), emitter, claimBase);
    emitter.Emit(OpCode::OP_NullCheck);
    emitter.EmitUint16(claimBase);
    //arg0 = index (box primitive Dict keys).
    uint16_t keyOffset = claimBase + kFrameSlotBytes;
    EmitExpression(*sub.Index(), emitter, keyOffset);
    if (keyBox.isPrimitive) {
        EmitPResultRefresh(emitter, keyOffset);
        emitter.Emit(OpCode::OP_Box);
        emitter.EmitByte(keyBox.tag);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(keyOffset);
    }
    //Bulk-copy claim → callParamBase (raw 4-byte moves
    //preserve tagged representations).
    for (uint16_t i = 0; i < 2; ++i) {
        emitter.Emit(OpCode::OP_VarLocal);
        emitter.EmitUint16(claimBase + i * kFrameSlotBytes);
        emitter.Emit(OpCode::OP_Assign);
        emitter.EmitUint16(
            m_currFunc->callParamBase + i * kFrameSlotBytes);
    }
    uint16_t nameIdx = AddStringConstant("get");
    emitter.Emit(OpCode::OP_CallMethod);
    emitter.EmitUint16(nameIdx);
    emitter.EmitUint16(m_currFunc->callParamBase);
    if (valBox.isPrimitive) {
        emitter.Emit(OpCode::OP_Unbox);
        emitter.EmitByte(valBox.tag);
    }
    emitter.Emit(OpCode::OP_Assign);
    emitter.EmitUint16(resultOffset);
    emitter.Emit(OpCode::OP_ParaEnd);
}

//Subscript dispatch: type-base sized-array creation, string byte read,
//container get() sugar, slice view mint, then the plain element read.
void VmBackend::Access(SnSubscriptExpr& expr) {
    BytecodeEmitter& emitter = *m_pCurrEmitter;
    uint16_t resultOffset = m_resultOffset;
        auto& sub = static_cast<SnSubscriptExpr&>(expr);
        //0.9.0: a type-base subscript is the sized array creation twin
        //(`int[n]` / `Foo[n]`); it emits through the shared new-array
        //path — the base names a type and evaluates to nothing.
        if (sub.NewArrayElem()) {
            //Stage-2: ND creation — the index list IS the rank; each dim
            //stages into a consecutive eval-area slot for OP_AllocArrayN.
            EmitNewArrayND(sub.NewArrayElem(), sub.Indices(),
                resultOffset, emitter);
            return;
        }
        //0.7.5 string subscript: byte read on a string base.
        if (sub.Array()->IsResolved() && sub.Array()->EvalDataType()
            && sub.Array()->EvalDataType()->Kind() == NK_String) {
            EmitStringByteAt(sub, emitter, resultOffset);
            return;
        }
        //List<T>/Dict<K,V> subscript sugar: li[i] == li.get(i),
        //d[k] == d.get(k). Dispatch on the base's resolved type being a
        //generic instantiation (arrays take the OP_LoadElement path below).
        if (IsContainerSubscript(*sub.Array())) {
            EmitContainerSubscriptGet(sub, emitter, resultOffset);
            return;
        }
        //v0.12.7 Task 11: a slice axis mints a view instead of loading
        //an element (the resolver typed the result as the view token).
        if (HasSliceAxis(sub)) {
            EmitArrayViewRead(sub, emitter, resultOffset);
            return;
        }
        EmitArraySubscriptRead(sub, emitter, resultOffset);
}

} //namespace nlang
