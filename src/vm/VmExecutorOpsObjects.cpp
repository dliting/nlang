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

void VmExecutor::OpAllocArray(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arrayTypeIdx = reader.ReadUint16();
    uint16_t sizeSlot = reader.ReadUint16();
    int32_t size;
    std::memcpy(&size, locals + sizeSlot, sizeof(size));
    if (size < 0)
        RaiseNlangException(m_oobExcClassIdx,
                            "NLang VM: negative array size");
    int32_t heapIdx = AllocArrayOnHeap(arrayTypeIdx, size);
    std::memcpy(locals + dst, &heapIdx, sizeof(heapIdx));
    m_gcPending = true;
}

void VmExecutor::OpLoadElement(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t arr = reader.ReadUint16();
    uint16_t index = reader.ReadUint16();
    int32_t heapIdx, idx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    std::memcpy(&idx, locals + index, sizeof(idx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    //C-period hole 3: element opcodes are only emitted for
    //array-typed bases; any other slot kind here is a compiler
    //invariant violation (canonically a dangling handle after
    //sweep cleared its kind to 0). Uncatchable by design --
    //same family as "invalid array type index". Order: after
    //the range check (protects the m_slotKinds index AND keeps
    //the catchable null-array-access path unchanged), before
    //the length read (slot[2] on a cleared record would be a
    //vector OOB read -- kind must gate it first).
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_LoadElement expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    int32_t length = slot[2];
    if (idx < 0 || idx >= length)
        RaiseNlangException(m_oobExcClassIdx,
                            "NLang VM: array index out of bounds");
    //0.7.5: element stride is registry-driven (slot[1] holds the array
    //type idx recorded at alloc). A 2-cell element copies 8 bytes —
    //the destination frame slot is the uniform 8-byte cell.
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_LoadElement on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    std::memcpy(locals + dst,
        slot.data() + 3 + static_cast<size_t>(idx) * cells,
        static_cast<size_t>(cells) * sizeof(int32_t));
}

void VmExecutor::OpStoreElement(BytecodeReader& reader, uint8_t* locals) {
    uint16_t arr = reader.ReadUint16();
    uint16_t index = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    int32_t heapIdx, idx;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    std::memcpy(&idx, locals + index, sizeof(idx));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    //Same kind gate as OP_LoadElement (hole 3), same ordering:
    //range check first, then kind, then the length read.
    if (m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: OP_StoreElement expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    int32_t length = slot[2];
    if (idx < 0 || idx >= length)
        RaiseNlangException(m_oobExcClassIdx,
                            "NLang VM: array index out of bounds");
    //0.7.5: mirror of OP_LoadElement's stride read — 2-cell elements
    //copy the full 8 bytes from the source frame slot.
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_StoreElement on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    std::memcpy(slot.data() + 3 + static_cast<size_t>(idx) * cells,
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
    int32_t len = m_structHeap[static_cast<size_t>(heapIdx)][2];
    std::memcpy(locals + dst, &len, sizeof(len));
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
        throw std::runtime_error(std::string(
            "NLang VM: invalid cast - expected `")
            + LeafNameOfKey(tgt.name) + "`, got `"
            + LeafNameOfKey(act.name) + "`");
    }
    //Result: same heap idx, unchanged.
}

void VmExecutor::OpThrow(BytecodeReader& reader, uint8_t* locals) {
    //Phase 9d: user `throw <expr>;` — read heap idx from src slot,
    //raise NLangThrow carrying the heap idx. Catch block above will
    //dispatch via func.tryBlocks.
    uint16_t src = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, locals + src, sizeof(heapIdx));
    //Debugger checkpoint at the throw site, before unwinding
    //starts (user throws do not pass through
    //RaiseNlangException — same checkpoint, shared helper).
    FireOnThrow();
    throw NLangThrow(heapIdx, "user throw");
}

void VmExecutor::OpRethrow() {
    //Phase 9d: `throw;` — re-raise the currently-caught exception.
    //Pops nothing; the matching OP_PopHandler at catch-block exit
    //(normal path) handles that.
    auto& s = m_callStack.back().handlerExcStack;
    if (s.empty())
        throw std::runtime_error(
            "NLang VM: rethrow outside catch handler");
    int32_t h = s.back();
    //Debugger checkpoint at the rethrow site (same contract as
    //OP_Throw / RaiseNlangException).
    FireOnThrow();
    throw NLangThrow(h, "rethrow");
}

void VmExecutor::OpPopHandler() {
    //Phase 9d: emitted at the end of every catch body. Pops the
    //exception bound on catch entry so a later throw; in a different
    //scope doesn't accidentally use this frame's exception.
    auto& s = m_callStack.back().handlerExcStack;
    if (s.empty())
        throw std::runtime_error(
            "NLang VM: PopHandler underflow");
    s.pop_back();
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
