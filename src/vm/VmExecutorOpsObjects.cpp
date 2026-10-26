/*---
    VmExecutorOpsObjects.cpp — 堆对象/字段/元素/装箱/转型/异常抛出操作码
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "VmExecutor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {

void VmExecutor::OpAllocStruct(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t structIdx = reader.ReadUint16();
    uint16_t fieldCount = reader.ReadUint16();
    int32_t heapIdx = AllocStructOnHeap(structIdx);
    std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
    m_gcPending = true;
}

void VmExecutor::OpLoadField(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t obj = reader.ReadUint16();
    uint16_t fieldOff = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + obj, sizeof(heapIdx));
    //Every struct/class data field is a uniform 2-cell (8-byte) heap
    //slot (0.7.5 Step A2); fieldOff is the emission-side byte offset.
    //The 8-byte copy fills the whole frame slot — narrow kinds read the
    //low half, so the upper bytes ride along harmlessly.
    int32_t fieldIdx = static_cast<int32_t>(fieldOff / kHeapCellBytes);
    const size_t kFieldCells = kHeapFieldStrideBytes / kHeapCellBytes;
    if (heapIdx < 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size()
        || fieldIdx < 0
        || static_cast<size_t>(fieldIdx) + kFieldCells
            > m_structHeap[static_cast<size_t>(heapIdx)].size())
        throw std::runtime_error("NLang VM: struct field access out of bounds");
    std::memcpy(locals + dst,
                &m_structHeap[static_cast<size_t>(heapIdx)][static_cast<size_t>(fieldIdx)],
                kHeapFieldStrideBytes);
}

void VmExecutor::OpStoreField(BytecodeReader& reader, uint8_t* locals) {
    uint16_t obj = reader.ReadUint16();
    uint16_t fieldOff = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + obj, sizeof(heapIdx));
    int32_t fieldIdx = static_cast<int32_t>(fieldOff / kHeapCellBytes);
    const size_t kFieldCells = kHeapFieldStrideBytes / kHeapCellBytes;
    if (heapIdx < 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size()
        || fieldIdx < 0
        || static_cast<size_t>(fieldIdx) + kFieldCells
            > m_structHeap[static_cast<size_t>(heapIdx)].size())
        throw std::runtime_error("NLang VM: struct field store out of bounds");
    std::memcpy(&m_structHeap[static_cast<size_t>(heapIdx)][static_cast<size_t>(fieldIdx)],
                locals + src, kHeapFieldStrideBytes);
}

void VmExecutor::OpNew(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t classIdx = reader.ReadUint16();
    int32_t heapIdx = AllocClassOnHeap(classIdx);
    std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
    m_gcPending = true;
}

void VmExecutor::OpNullCheck(BytecodeReader& reader, uint8_t* locals) {
    uint16_t obj = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + obj, sizeof(heapIdx));
    if (heapIdx <= 0)
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null reference error");
}

void VmExecutor::OpCopyStruct(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    uint16_t structIdx = reader.ReadUint16();
    int32_t srcHeapIdx;
    std::memcpy(&srcHeapIdx, locals + src, sizeof(srcHeapIdx));
    int32_t newHeapIdx = DeepCopyStruct(srcHeapIdx, structIdx);
    std::memcpy(locals + dst, &newHeapIdx, sizeof(newHeapIdx));
}

void VmExecutor::OpAllocArrayN(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arrayTypeIdx = reader.ReadUint16();
    uint16_t rank = reader.ReadUint16();
    uint16_t dimsBase = reader.ReadUint16();
    //The dims are rank consecutive int32 slots. Length validation
    //(per-dim negative/cap and the elemCount product) lives in
    //AllocArrayOnHeap — the single choke point shared with native
    //minting.
    int32_t dims[kMaxArrayRank];
    if (rank < 1 || rank > kMaxArrayRank)
        throw std::runtime_error(
            "NLang VM: OP_AllocArrayN rank out of range");
    for (uint16_t d = 0; d < rank; ++d)
        std::memcpy(&dims[d], locals + dimsBase + d * kFrameSlotBytes,
                    sizeof(int32_t));
    int32_t heapIdx = AllocArrayOnHeap(arrayTypeIdx,
        static_cast<uint8_t>(rank), dims);
    std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
    m_gcPending = true;
}

void VmExecutor::OpLoadElementN(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arr = reader.ReadUint16();
    uint16_t idxBase = reader.ReadUint16();
    uint16_t rank = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    //Element opcodes are only emitted for array-typed bases; any other
    //slot kind is a compiler invariant violation (uncatchable).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_LoadElementN expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t recRank = slot[2];
    if (static_cast<uint16_t>(recRank) != rank)
        throw std::runtime_error(
            "NLang VM: OP_LoadElementN rank mismatch");
    const int32_t* dims = slot.data() + 4;
    //Per-dimension bounds check with the row-major flat index; a named
    //dimension in the message keeps ND debugging humane.
    int64_t flat = 0;
    for (uint16_t d = 0; d < rank; ++d) {
        int32_t idx;
        std::memcpy(&idx, locals + idxBase + d * kFrameSlotBytes,
                    sizeof(idx));
        if (idx < 0 || idx >= dims[d])
            RaiseNlangException(m_oobExcClassIdx,
                "NLang VM: array index out of bounds (dim "
                + std::to_string(d) + ")");
        flat = flat * dims[d] + idx;
    }
    //0.7.5: element stride is registry-driven (slot[1] holds the array
    //type idx recorded at alloc). A 2-cell element copies 8 bytes —
    //the destination frame slot is the uniform 8-byte cell.
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_LoadElementN on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    std::memcpy(locals + dst,
        slot.data() + ArrayHeaderCells(static_cast<uint8_t>(rank))
            + static_cast<size_t>(flat) * cells,
        static_cast<size_t>(cells) * sizeof(int32_t));
}

void VmExecutor::OpStoreElementN(BytecodeReader& reader, uint8_t* locals) {
    uint16_t arr = reader.ReadUint16();
    uint16_t idxBase = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    uint16_t rank = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    //Same kind gate as OP_LoadElementN (invariant violation, uncatchable).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_StoreElementN expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t recRank = slot[2];
    if (static_cast<uint16_t>(recRank) != rank)
        throw std::runtime_error(
            "NLang VM: OP_StoreElementN rank mismatch");
    const int32_t* dims = slot.data() + 4;
    int64_t flat = 0;
    for (uint16_t d = 0; d < rank; ++d) {
        int32_t idx;
        std::memcpy(&idx, locals + idxBase + d * kFrameSlotBytes,
                    sizeof(idx));
        if (idx < 0 || idx >= dims[d])
            RaiseNlangException(m_oobExcClassIdx,
                "NLang VM: array index out of bounds (dim "
                + std::to_string(d) + ")");
        flat = flat * dims[d] + idx;
    }
    //0.7.5: mirror of OP_LoadElementN's stride read — 2-cell elements
    //copy the full 8 bytes from the source frame slot.
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_StoreElementN on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    std::memcpy(slot.data() + ArrayHeaderCells(static_cast<uint8_t>(rank))
            + static_cast<size_t>(flat) * cells,
        locals + src, static_cast<size_t>(cells) * sizeof(int32_t));
}

void VmExecutor::OpArrayLength(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arr = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    //Same kind gate as OP_LoadElement (hole 3). No index
    //operand here, so only the range check precedes it; the
    //kind must still gate the slot[2] length read.
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_ArrayLength expects an array slot");
    //Stage-2 header: elemCount moved to slot[3].
    int32_t len = m_structHeap[static_cast<size_t>(heapIdx)][3];
    std::memcpy(locals + dst, &len, sizeof(len));
}

//Stage-2: flat element read — the payload is contiguous row-major, so a
//linear index addresses rank-1 and rank-N arrays alike (foreach walks
//elemCount elements with this). Bounds: flat < elemCount.
void VmExecutor::OpLoadElementFlat(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arr = reader.ReadUint16();
    uint16_t flatSlot = reader.ReadUint16();
    int32_t heapIdx, flat;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    std::memcpy(&flat, locals + flatSlot, sizeof(flat));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_LoadElementFlat expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t elemCount = slot[3];
    if (flat < 0 || flat >= elemCount)
        RaiseNlangException(m_oobExcClassIdx,
                            "NLang VM: array index out of bounds");
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_LoadElementFlat on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    const int32_t rank = slot[2];
    std::memcpy(locals + dst,
        slot.data() + ArrayHeaderCells(static_cast<uint8_t>(rank))
            + static_cast<size_t>(flat) * cells,
        static_cast<size_t>(cells) * sizeof(int32_t));
}

//Stage-2: mint the shape array int[rank] and copy the dims in.
void VmExecutor::OpArrayShape(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arr = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_ArrayShape expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t rank = slot[2];
    if (rank < 1 || rank > kMaxArrayRank)
        throw std::runtime_error(
            "NLang VM: OP_ArrayShape on corrupt record rank");
    //Snapshot the dims BEFORE allocating: AllocArrayOnHeap grows
    //m_structHeap and would invalidate the source-record reference
    //(choke-point contract at AllocArrayOnHeap).
    int32_t dimsSnapshot[kMaxArrayRank];
    for (int32_t d = 0; d < rank; ++d)
        dimsSnapshot[d] = slot[4 + d];
    //The shape array is int[rank] — one dim cell per rank; the scalar
    //int[] table entry comes from the mint filter.
    const uint16_t shapeTypeIdx = FindScalarArrayTypeIdx(RTK_Int32, 1);
    int32_t shapeDim = rank;
    int32_t shapeIdx = AllocArrayOnHeap(shapeTypeIdx, 1, &shapeDim);
    m_gcPending = true;
    auto& shape = m_structHeap[static_cast<size_t>(shapeIdx)];
    const int32_t base =
        static_cast<int32_t>(ArrayPayloadBase(shape));
    for (int32_t d = 0; d < rank; ++d)
        shape[base + d] = dimsSnapshot[d];
    std::memcpy(locals + dst, &shapeIdx, sizeof(shapeIdx));
}

void VmExecutor::OpBox(BytecodeReader& reader, uint8_t* pResult) {
    uint8_t typeTag = reader.ReadByte();
    //0.7.5: payload width from the registry — 8-byte rows (long/ulong)
    //read the full slot; ≤4-byte tags keep the legacy 4-byte read (the
    //upper half of the slot can be stale for narrow writers).
    int pi = ScalarPrimIndexOfRtk(typeTag);
    bool wide = pi >= 0 && kScalarPrims[pi].slotWidth == 8;
    int64_t val = 0;
    std::memcpy(&val, pResult, wide ? sizeof(val) : sizeof(int32_t));
    //Allocation (incl. the always-allocate invariant for val==0)
    //lives in AllocBoxedValue — shared with IntrinsicsString.cpp.
    int32_t heapIdx = AllocBoxedValue(typeTag, val);
    std::memcpy(pResult, &heapIdx, sizeof(heapIdx));
}

void VmExecutor::OpUnbox(BytecodeReader& reader, uint8_t* pResult) {
    uint8_t expectedTag = reader.ReadByte();
    int32_t heapIdx;
    std::memcpy(&heapIdx, pResult, sizeof(heapIdx));
    if (heapIdx <= 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: unbox on null/invalid reference");
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Boxed)
        throw std::runtime_error(
            "NLang VM: unbox target is not a boxed primitive");
    int32_t actualTag = m_structHeap[static_cast<size_t>(heapIdx)][0];
    if (static_cast<uint8_t>(actualTag) != expectedTag)
    {
        //0.7.5: names come from the scalar registry so every primitive
        //kind (bool today, the full family in P4) reports correctly.
        auto tagName = [](uint8_t rtk) {
            int i = ScalarPrimIndexOfRtk(rtk);
            return i < 0 && rtk == RTK_String ? "string"
                 : i < 0 ? "unknown" : kScalarPrims[i].name;
        };
        throw std::runtime_error(std::string(
            "NLang VM: invalid unbox - expected ") + tagName(expectedTag) +
            ", got " + tagName(static_cast<uint8_t>(actualTag)));
    }
    //0.7.5: payload width from the registry (mirror of OpBox) — 8-byte
    //rows rebuild the value from cells [1..2]; ≤4-byte tags read
    //cell[1].
    int pi = ScalarPrimIndexOfRtk(expectedTag);
    if (pi >= 0 && kScalarPrims[pi].slotWidth == 8)
    {
        auto& rec = m_structHeap[static_cast<size_t>(heapIdx)];
        uint64_t bits = static_cast<uint32_t>(rec[1])
            | (static_cast<uint64_t>(
                static_cast<uint32_t>(rec[2])) << 32);
        std::memcpy(pResult, &bits, sizeof(bits));
    }
    else
    {
        int32_t val = m_structHeap[static_cast<size_t>(heapIdx)][1];
        std::memcpy(pResult, &val, sizeof(val));
    }
}

void VmExecutor::OpCheckCast(BytecodeReader& reader, uint8_t* pResult) {
    uint16_t targetClassIdx = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, pResult, sizeof(heapIdx));
    if (heapIdx <= 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: cast on null/invalid reference");
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Class)
        throw std::runtime_error(
            "NLang VM: CheckCast target is not a class object");
    int32_t actualClassIdx =
        m_structHeap[static_cast<size_t>(heapIdx)][0];
    //Walk the actual class's super chain; accept if target is found.
    bool ok = (actualClassIdx == static_cast<int32_t>(targetClassIdx));
    int32_t cur = actualClassIdx;
    while (!ok && cur > 0)
    {
        const auto& cc = m_currModule->classes[
            static_cast<size_t>(cur)];
        int16_t sup = cc.superClassIdx;
        if (sup < 0 || static_cast<size_t>(sup) >=
            m_currModule->classes.size())
            break;
        if (static_cast<uint16_t>(sup) == targetClassIdx)
        {
            ok = true;
            break;
        }
        cur = static_cast<int32_t>(sup);
    }
    if (!ok)
    {
        const auto& tgt = m_currModule->classes[
            static_cast<size_t>(targetClassIdx)];
        const auto& act = m_currModule->classes[
            static_cast<size_t>(actualClassIdx)];
        //User-visible failure message: leaf names. The class table keys
        //are package-qualified link identities; the leaf-name spelling
        //for user-facing rendering is the shared rule (CompiledModule.h).
        //v0.8.5: raise the catchable built-in InvalidCastException (NLang
        //catch reads e.message); the malformed-state guards above stay
        //std::runtime_error by design.
        RaiseNlangException(m_invalidCastExcClassIdx, std::string(
            "NLang VM: invalid cast - expected `")
            + LeafNameOfKey(tgt.name) + "`, got `"
            + LeafNameOfKey(act.name) + "`");
    }
    //Result: same heap idx, unchanged.
}

//Phase 9d: walk superClassIdx chain starting from the actual class at
//heapIdx's slot[0]. Returns true if targetClassIdx appears anywhere in
//the chain (i.e., the object is an instance of target or a subclass).
bool VmExecutor::IsInstanceOrSubclass(int32_t heapIdx, uint16_t targetClassIdx) {
    if (heapIdx <= 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        return false;
    int16_t cur = static_cast<int16_t>(m_structHeap[static_cast<size_t>(heapIdx)][0]);
    while (cur >= 0) {
        if (cur == static_cast<int16_t>(targetClassIdx))
            return true;
        if (static_cast<size_t>(cur) >= m_currModule->classes.size())
            return false;
        cur = m_currModule->classes[static_cast<size_t>(cur)].superClassIdx;
    }
    return false;
}

//Phase 8d — polymorphism check for class-typed deserialization.
//Walks the superClassIdx chain from actualIdx upward. Returns true if
//actualIdx is declaredIdx or a subclass thereof. Used by DeserializeClassFields
//and the top-level ReadObject intrinsics to accept stream objects whose
//runtime class is a subclass of the declared (expected) class.
bool VmExecutor::IsSubclassOf(uint16_t actualIdx, uint16_t declaredIdx) {
    int16_t cur = static_cast<int16_t>(actualIdx);
    while (cur >= 0) {
        if (cur == static_cast<int16_t>(declaredIdx)) return true;
        cur = m_currModule->classes[static_cast<size_t>(cur)].superClassIdx;
    }
    return false;
}

int32_t VmExecutor::ReceiverClassIndex(int32_t heapIdx) const {
    if (m_slotKinds[static_cast<size_t>(heapIdx)] == RTK_Boxed) {
        const auto& classes = m_currModule->classes;
        //Bare-name comparison = builtin: only the ownerless synthesized
        //Object keeps a bare "Object" key after phase 5; a user pkg.Object
        //is keyed with its package and must not be treated as the
        //boxed-primitive root.
        for (size_t i = 0; i < classes.size(); ++i)
            if (classes[i].name == "Object")
                return static_cast<int32_t>(i);
        return -1;
    }
    return m_structHeap[static_cast<size_t>(heapIdx)][0];
}

} // namespace nlang
