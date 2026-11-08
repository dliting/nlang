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

void VmExecutor::OpLoadStatic(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t slotIdx = reader.ReadUint16();
    //D12: a slot with an initializer reads as uninitialized until its
    //initializer stores it — throw instead of handing out the silent
    //zero. The access-site gate has already run, so the owner is at
    //least Initializing: an unset bit here means the initializer is
    //running deeper on this stack (re-entrant read) or failed; both
    //surface as a catchable InitializationException.
    const CompiledStaticSlot& meta = m_currModule->staticSlots[slotIdx];
    if (meta.hasInitializer && !m_staticSlots.IsSlotInitialized(slotIdx))
        RaiseNlangException(m_initExcClassIdx,
                            "Static field \"" + meta.name
                            + "\" is read before its initializer stored "
                              "a value.");
    //Plain 8-byte cell copy (same uniform-slot semantics as OpLoadField:
    //narrow kinds read the low half, upper bytes ride along harmlessly).
    std::memcpy(locals + dst, m_staticSlots.Cell(slotIdx),
                kHeapFieldStrideBytes);
}

void VmExecutor::OpStoreStatic(BytecodeReader& reader, uint8_t* locals) {
    uint16_t slotIdx = reader.ReadUint16();
    uint16_t src = reader.ReadUint16();
    const CompiledStaticSlot& meta = m_currModule->staticSlots[slotIdx];
    //v0.12.6 D14: a struct-typed slot is a value boundary — the slot
    //owns an independent record, not the source's handle. Deep-copy at
    //this single choke point so every producer (user stores, $staticInit
    //assignments, static-local guard stores, cross-module consumers)
    //shares the semantics. The slot metadata's typeIdx carries the
    //struct table index, link-remapped by nlink like every TypeDesc.
    //A zero handle passes through unchanged: null is not a struct
    //value, and no legal program produces it here.
    if (meta.type.kind == RTK_Struct) {
        int32_t srcHeapIdx;
        std::memcpy(&srcHeapIdx, locals + src, sizeof(srcHeapIdx));
        if (srcHeapIdx > 0) {
            const int32_t copyIdx =
                DeepCopyStruct(srcHeapIdx, meta.type.typeIdx);
            std::memcpy(m_staticSlots.Cell(slotIdx), &copyIdx,
                        sizeof(copyIdx));
            if (meta.hasInitializer)
                m_staticSlots.MarkSlotInitialized(slotIdx);
            return;
        }
    }
    std::memcpy(m_staticSlots.Cell(slotIdx), locals + src,
                kHeapFieldStrideBytes);
    //D12: the initializer's store publishes the value. Marking is
    //unconditional for initializer-bearing slots: gates precede every
    //user-visible store, so a mark here can never lag a visible value,
    //and a store from inside a failed attempt keeps the bit (D10's
    //attempts-counter retention — the retry re-runs and re-marks).
    if (meta.hasInitializer)
        m_staticSlots.MarkSlotInitialized(slotIdx);
}

void VmExecutor::OpNew(BytecodeReader& reader, uint8_t* locals) {
    uint16_t dst = reader.ReadUint16();
    uint16_t classIdx = reader.ReadUint16();
    //v0.13.1: the instantiation descriptor row (0xFFFF = none). The
    //cache was interned at run start; a row index past the table is a
    //corrupt image, not a user error.
    uint16_t descIdx = reader.ReadUint16();
    GenericInstDescriptor* instDesc = nullptr;
    if (descIdx != 0xFFFF) {
        if (descIdx >= m_descCache.size())
            throw std::runtime_error(
                "NLang VM: invalid descriptor index in OP_New");
        instDesc = m_descCache[descIdx];
    }
    int32_t heapIdx = AllocClassOnHeap(classIdx, instDesc);
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

int32_t VmExecutor::CastOperandClassIndex(int32_t heapIdx) const {
    const uint8_t slotKind = m_slotKinds[static_cast<size_t>(heapIdx)];
    if (slotKind == RTK_Boxed)
        return ObjectRootClassIdx();
    if (slotKind == RTK_Array || slotKind == RTK_ArrayView)
        return ArrayFaceClassIdx();
    if (slotKind == RTK_Class)
        return m_structHeap[static_cast<size_t>(heapIdx)][0];
    throw std::runtime_error(
        "NLang VM: CheckCast target is not a class object");
}

bool VmExecutor::ClassMatchesTarget(int32_t actualClassIdx,
                                    uint16_t targetClassIdx) const {
    if (actualClassIdx == static_cast<int32_t>(targetClassIdx))
        return true;
    int32_t cur = actualClassIdx;
    while (cur > 0)
    {
        if (static_cast<size_t>(cur) >= m_currModule->classes.size())
            break;
        const int16_t sup = m_currModule->classes[
            static_cast<size_t>(cur)].superClassIdx;
        if (sup < 0 || static_cast<size_t>(sup) >=
            m_currModule->classes.size())
            break;
        if (static_cast<uint16_t>(sup) == targetClassIdx)
            return true;
        cur = static_cast<int32_t>(sup);
    }
    return false;
}

void VmExecutor::OpCheckCast(BytecodeReader& reader, uint8_t* pResult) {
    uint16_t targetClassIdx = reader.ReadUint16();
    int32_t heapIdx;
    std::memcpy(&heapIdx, pResult, sizeof(heapIdx));
    //v0.12.7: null is a legal language value — `x as T` on null yields
    //null (the result cell already holds it); out-of-range stays host error.
    if (heapIdx == 0)
        return;
    if (heapIdx < 0
        || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: cast on invalid reference");
    //v0.12.7 (final-review I1): arrays (owned or view) and boxed values
    //are Object citizens — their protocol rows walk the same super
    //chain as class records, so every class-target failure raises the
    //catchable InvalidCastException below (the §4.5 error model);
    //before, these kinds hit the host error and escaped every catch.
    const int32_t actualClassIdx = CastOperandClassIndex(heapIdx);
    if (actualClassIdx < 0)
        throw std::runtime_error(
            "NLang VM: protocol root class row missing for cast");
    if (!ClassMatchesTarget(actualClassIdx, targetClassIdx))
    {
        const auto& tgt = m_currModule->classes[
            static_cast<size_t>(targetClassIdx)];
        const auto& act = m_currModule->classes[
            static_cast<size_t>(actualClassIdx)];
        //User-visible failure message: leaf names (the class table keys
        //are package-qualified link identities; shared rule, CompiledModule.h).
        //v0.8.5: raise the catchable InvalidCastException (NLang catch reads
        //e.message); the malformed-state guards above stay std::runtime_error.
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

int32_t VmExecutor::ObjectRootClassIdx() const {
    const auto& classes = m_currModule->classes;
    for (size_t i = 0; i < classes.size(); ++i)
        if (classes[i].name == "Object")
            return static_cast<int32_t>(i);
    return -1;
}

int32_t VmExecutor::ArrayFaceClassIdx() const {
    const auto& classes = m_currModule->classes;
    for (size_t i = 0; i < classes.size(); ++i)
        if (classes[i].name == "Array")
            return static_cast<int32_t>(i);
    return -1;
}

int32_t VmExecutor::ReceiverClassIndex(int32_t heapIdx) const {
    const uint8_t kind = m_slotKinds[static_cast<size_t>(heapIdx)];
    //Boxed receivers resolve against Object, the static type every
    //boxed value carries (their only reachable methods are the Object
    //protocol).
    if (kind == RTK_Boxed)
        return ObjectRootClassIdx();
    //v0.12.7 Task 5: an array receiver (owning record or view — both
    //record kinds dispatch alike) lands on the Array face — the face
    //row's stamped protocol methods (toString/equals/getHashCode)
    //answer Object-receiver protocol calls (spec 4.1: Array<E> <:
    //Object). Final-review C2: the view record's header holds its kind
    //byte, which must never reach the class-index read below.
    if (kind == RTK_Array || kind == RTK_ArrayView)
        return ArrayFaceClassIdx();
    return m_structHeap[static_cast<size_t>(heapIdx)][0];
}

} // namespace nlang
