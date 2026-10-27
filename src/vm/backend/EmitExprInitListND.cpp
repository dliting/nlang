/*---
    EmitExprInitListND.cpp - ND (rank >= 2) array initializer emission.
    Static arm (stage-2): a validated rectangular literal lowers to one
    OP_AllocArrayN (constant dims) plus per-leaf OP_StoreElementN at
    constant row-major coordinates. Dynamic arm (stage-3 D3): a literal
    holding expression rows lowers to OP_AssembleArrayN over evaluated
    row handles - the runtime validates and assembles the shape. Split
    from EmitExprInitList.cpp (2026-10-25, source-size governance).
---*/
#include "VmBackend.h"
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnData.h>
#include <vector>
#include <cstdint>

namespace nlang {

//Stage-3 D3: true when any above-leaf entry is not a bracket row -
//a plain expression, or a brace-form row the resolver let through.
//Such literals take the dynamic OP_AssembleArrayN arm (their shapes
//are runtime values). Shared by the emitter dispatch and the walker's
//peak-depth mirror so the two cannot drift. The resolver rejects
//brace rows with its own depth diagnostic before emit sees one;
//counting them here keeps the emitter total - the static arm's dims
//walk cannot follow a brace row, and the dynamic arm emits any
//resolved row literal as a row handle.
bool VmBackend::NDInitListHasExpressionRow(SnInitListExpr &node,
    uint8_t aboveLeaf) {
    for (auto &entry : node.Entries()) {
        if (!entry.pValue)
            continue;
        if (SnInitListExpr::IsBracketForm(entry.pValue)) {
            if (aboveLeaf > 1 && NDInitListHasExpressionRow(
                    *static_cast<SnInitListExpr *>(entry.pValue),
                    static_cast<uint8_t>(aboveLeaf - 1)))
                return true;
        } else {
            return true;
        }
    }
    return false;
}

//Dims walk of the validated ND literal: counts ride the first path —
//the resolver's rectangular gate made every sibling agree with it.
//False = a non-list row where a row belongs (resolver contract
//breach; the caller logs and bails instead of walking through it).
static bool CollectNDInitListDims(SnInitListExpr &node,
    uint8_t remaining, std::vector<int32_t> &dims) {
    dims.push_back(static_cast<int32_t>(node.Entries().size()));
    if (remaining <= 1)
        return true;
    if (node.Entries().empty())
        return true;  //deeper dims are 0-width rows
    auto *pFirst = node.Entries()[0].pValue;
    auto *pRow = SnInitListExpr::IsBracketForm(pFirst)
        ? static_cast<SnInitListExpr*>(pFirst) : nullptr;
    if (!pRow)
        return false;
    return CollectNDInitListDims(*pRow,
        static_cast<uint8_t>(remaining - 1), dims);
}

//Stage-2 ND arm: a rank>=2 array literal. The resolver validated the
//compile-time rectangular shape (nest depth == rank, equal row
//widths), so the dims feed one OP_AllocArrayN and each leaf lands
//through OP_StoreElementN at its constant row-major coordinate.
//The dynamic dispatch runs before the array-type registration: the
//dynamic arm registers the type itself, so a literal never registers
//the same entry twice.
void VmBackend::EmitInitListArrayND(SnInitListExpr &initList,
    SnArrayTypeToken &token, BytecodeEmitter &emitter,
    uint16_t resultOffset) {
    const uint8_t rank = token.Rank();
    //Expression rows mean runtime shapes: the dynamic arm owns them.
    if (NDInitListHasExpressionRow(initList, static_cast<uint8_t>(rank - 1)))
        return EmitInitListArrayNDynamic(initList, token, emitter,
                                         resultOffset);
    uint16_t arrayTypeIdx = RegisterArrayType(token.ElemTypeOf(), rank);
    std::vector<int32_t> dims;
    if (!CollectNDInitListDims(initList, rank, dims)) {
        //Resolver breach: only bracket rows ride the static arm.
        m_pEnv->Log(CLL_Error, initList.Location(),
            "internal error: ND literal lost its bracket row form");
        return;
    }
    {
        //The dims staging claim spans only the alloc emission; the
        //leaf loop below reclaims the same band for its value slot.
        EvalAreaClaim dimsClaim(*this, rank);
        uint16_t dimsBase = dimsClaim.base();
        for (uint8_t d = 0; d < rank; ++d) {
            emitter.Emit(OpCode::OP_ConstInt32);
            emitter.EmitInt32(dims[d]);
            emitter.Emit(OpCode::OP_Assign);
            emitter.EmitUint16(
                static_cast<uint16_t>(dimsBase + d * kFrameSlotBytes));
        }
        emitter.Emit(OpCode::OP_AllocArrayN);
        emitter.EmitUint16(resultOffset);
        emitter.EmitUint16(arrayTypeIdx);
        emitter.EmitUint16(rank);
        emitter.EmitUint16(dimsBase);
    }
    //Leaves in row-major order; the constant coordinates and the
    //staged value share one exclusive claim [coords | value]. The
    //rank-1 entry path parks its single index in callParamBase, but
    //for rank>=2 the decl temp (resultOffset) sits directly above
    //callParamBase — the top coordinate slot would alias the array
    //handle itself. Nested emissions inside a leaf value claim fresh
    //slots above the claim. ExprPeakDepth's InitListExpr case
    //mirrors: dims claim released before the leaf claim, so the band
    //peak is (rank + 1 + deepest leaf).
    EvalAreaClaim leafClaim(*this, rank + 1);
    uint16_t coordBase = leafClaim.base();
    uint16_t valueSlot = coordBase + rank * kFrameSlotBytes;
    std::vector<int32_t> coords;
    EmitNDInitListLeaves(initList, token, coords, coordBase, valueSlot,
        emitter, resultOffset);
}

//Stage-3 D3 dynamic arm: at least one above-leaf entry is an
//expression, so the dims are runtime values. Every entry - bracket
//rows lower through the normal expression path as complete nested
//literals, expression rows by their own emission - evaluates into one
//row-handle claim, then OP_AssembleArrayN validates the shared row
//shape and builds the record at runtime. Keep in lockstep with the
//dynamic case of InitListExprPeakDepth (claim band + entry depths).
void VmBackend::EmitInitListArrayNDynamic(SnInitListExpr &initList,
    SnArrayTypeToken &token, BytecodeEmitter &emitter,
    uint16_t resultOffset) {
    SnField *pElemField = token.ElemTypeOf();
    uint16_t arrayTypeIdx = RegisterArrayType(pElemField, token.Rank());
    const uint16_t rowCount =
        static_cast<uint16_t>(initList.Entries().size());
    //HeapIndex class: every parked slot ends holding an array handle
    //(the opcode re-validates each row before use).
    EvalAreaClaim rowsClaim(*this, rowCount, ESC_HeapIndex);
    const uint16_t rowsBase = rowsClaim.base();
    for (uint16_t i = 0; i < rowCount; ++i) {
        auto &entry = initList.Entries()[i];
        EmitExpression(*entry.pValue, emitter,
            static_cast<uint16_t>(rowsBase + i * kFrameSlotBytes));
    }
    emitter.Emit(OpCode::OP_AssembleArrayN);
    emitter.EmitUint16(resultOffset);
    emitter.EmitUint16(arrayTypeIdx);
    emitter.EmitUint16(rowsBase);
    emitter.EmitUint16(rowCount);
}

//Row-major leaf walk: coords accumulates the constant coordinate
//prefix; at leaf depth the value emits into the claim's value slot
//(struct elements deep-copy first, mirroring the rank-1 entry path)
//and the coordinate constants land in claim coordBase[0..rank-1].
void VmBackend::EmitNDInitListLeaves(SnInitListExpr &node,
    SnArrayTypeToken &token, std::vector<int32_t> &coords,
    uint16_t coordBase, uint16_t valueSlot, BytecodeEmitter &emitter,
    uint16_t resultOffset) {
    const uint8_t rank = token.Rank();
    SnField *pElemField = token.ElemTypeOf();
    for (size_t i = 0; i < node.Entries().size(); ++i) {
        auto &entry = node.Entries()[i];
        if (!entry.pValue)
            continue;
        coords.push_back(static_cast<int32_t>(i));
        if (SnInitListExpr::IsBracketForm(entry.pValue)) {
            EmitNDInitListLeaves(
                *static_cast<SnInitListExpr*>(entry.pValue), token,
                coords, coordBase, valueSlot, emitter, resultOffset);
        } else {
            EmitExpression(*entry.pValue, emitter, valueSlot);
            if (RuntimeTypeKind(pElemField) == RTK_Struct
                && pElemField && pElemField->Kind() == NK_StructDecl) {
                EmitStructDeepCopy(valueSlot, valueSlot,
                    static_cast<SnStructDecl&>(*pElemField), emitter);
            }
            for (uint8_t d = 0; d < rank; ++d) {
                emitter.Emit(OpCode::OP_ConstInt32);
                emitter.EmitInt32(coords[d]);
                emitter.Emit(OpCode::OP_Assign);
                emitter.EmitUint16(static_cast<uint16_t>(
                    coordBase + d * kFrameSlotBytes));
            }
            emitter.Emit(OpCode::OP_StoreElementN);
            emitter.EmitUint16(resultOffset);
            emitter.EmitUint16(coordBase);
            emitter.EmitUint16(valueSlot);
            emitter.EmitUint16(rank);
        }
        coords.pop_back();
    }
}

} //namespace nlang
