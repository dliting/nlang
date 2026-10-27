/*---
    VmExecutorOpsObjects.cpp — 堆对象/字段/元素/装箱/转型/异常抛出操作码
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
    K8: the field/element/box/cast family runs lock-free under the sharing
    memory model — field and element writes are benign value races on
    shared records, allocation goes through the per-carrier shard free
    lists, and error paths re-acquire inside RaiseNlangException. The
    throw family keeps the store lock (debug checkpoint).
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
    NoteGcPressure();
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
    NoteGcPressure();
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
        throw std::runtime_error("NLang VM: cast on null/invalid reference");
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
        //Bare-name scan = the prelude root: prelude top-level members key
        //bare "Object" in every image (source-of-truth design §6.7); a
        //user pkg.Object is keyed with its package and must not be treated
        //as the boxed-primitive root.
        for (size_t i = 0; i < classes.size(); ++i)
            if (classes[i].name == "Object")
                return static_cast<int32_t>(i);
        return -1;
    }
    return m_structHeap[static_cast<size_t>(heapIdx)][0];
}

} // namespace nlang
