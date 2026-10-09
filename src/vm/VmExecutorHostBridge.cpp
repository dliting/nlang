/*---
    VmExecutorHostBridge.cpp — 扩展点②的 by-name 入口与宿主值桥。
    桥方法是既有内部机制的薄公开包装（spec §8：无新语义）；
    CallFunctionByIdx 复刻 OP_CallFunc 帧构造并自填默认值。
---*/
#include "VmExecutor.h"
//ArrayElemCells/kFrameSlotBytes/INTR_None 都在公共头（已核验：
//CompiledModule.h:113/127/170），VmExecutor.h 已可见，无需额外 include。
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace nlang {

//--- by-name 入口 ---------------------------------------------------------

void VmExecutor::CallByName(const std::string& qualifiedName,
                            const uint8_t* argCells, uint32_t argc,
                            uint8_t* resultCell) {
    if (!m_currModule)
        throw std::runtime_error(
            "NLang VM: CallByName before InitializeForRun");
    //Scan all functions carrying the key (overloads share it); the
    //arity test is argc <= paramCount <= argc + defaults-available.
    //The adapter already disambiguated; here take the first whose
    //paramCount can absorb argc with defaults.
    for (size_t i = 0; i < m_currModule->functions.size(); ++i) {
        const CompiledFunction& f = m_currModule->functions[i];
        if (f.name != qualifiedName)
            continue;
        if (argc <= f.paramCount) {
            CallFunctionByIdx(static_cast<uint16_t>(i), argCells, argc,
                              resultCell);
            return;
        }
    }
    throw std::runtime_error(
        "NLang VM: function not found or arity mismatch: " + qualifiedName);
}

void VmExecutor::CallFunctionByIdx(uint16_t funcIndex,
                                   const uint8_t* argCells, uint32_t argc,
                                   uint8_t* resultCell) {
    if (!m_currModule || funcIndex >= m_currModule->functions.size())
        throw std::runtime_error("NLang VM: invalid function index");
    const CompiledFunction& callee = m_currModule->functions[funcIndex];
    if (argc > callee.paramCount)
        throw std::runtime_error(
            "NLang VM: too many arguments for " + callee.name);
    if (callee.intrinsicId != INTR_None)
        throw std::runtime_error(
            "NLang VM: intrinsic function not callable by name: "
            + callee.name);
    if (callee.isNative) {
        //Stage into our own buffer — CallNative reads the caller-frame
        //ABI (args at base 0 of the given buffer).
        std::vector<uint8_t> staged(
            static_cast<size_t>(callee.paramCount) * kFrameSlotBytes, 0);
        if (argc)
            std::memcpy(staged.data(), argCells,
                        static_cast<size_t>(argc) * kFrameSlotBytes);
        FillDefaults(callee, staged.data(), argc);
        CallNative(callee, 0, staged.data(), resultCell);
        return;
    }
    std::vector<uint8_t> calleeLocals(callee.localsSize, 0);
    if (argc)
        std::memcpy(calleeLocals.data(), argCells,
                    static_cast<size_t>(argc) * kFrameSlotBytes);
    FillDefaults(callee, calleeLocals.data(), argc);
    try {
        ExecuteFunction(callee, resultCell, calleeLocals.data());
    } catch (...) {
        m_lastBacktrace = FormatBacktrace();
        throw;
    }
}

//Formals beyond argc fill from the declaration's Option-B default
//tags. There is no compiler call site here, so the runtime fills what
//the compiler would have staged (spec §13 default-duty note). The
//adapter layer already rejected arity/default mismatches as BadValue,
//so the throws here are defensive depth for direct VM-level callers.
void VmExecutor::FillDefaults(const CompiledFunction& callee,
                              uint8_t* frameCells, uint32_t argc) {
    //Option-B contract: dense vector aligned to paramCount (RTK_Void
    //marks "no default"). A misaligned table is a malformed module.
    if (callee.defaultValues.size() != callee.paramCount)
        throw std::runtime_error(
            "NLang VM: default-value table not aligned on " + callee.name);
    for (uint32_t i = argc; i < callee.paramCount; ++i) {
        FillOneDefault(callee, callee.defaultValues[i],
                       frameCells + i * kFrameSlotBytes);
    }
}

//One formal's default staged into its frame cell.
void VmExecutor::FillOneDefault(const CompiledFunction& callee,
                                const DefaultValueDesc& d, uint8_t* cell) {
    switch (d.tag) {
    case RTK_Int32: {
        //Producer fact (src/vm/backend/Types.cpp:62-107
        //ExtractLiteralDefault): defaults only ever carry
        //RTK_Int32/Long/ULong/Float/Double/Null/String. A 4-byte
        //formal's default lands as RTK_Int32 regardless of declared
        //width (byte/short/uint literals are RnInt32); bool/char/
        //non-foldable defaults stay RTK_Void/Unfoldable and are
        //rejected below — same limitation as the cross-module stub
        //path. Value in the low 4 bytes; negative via uint32 cast.
        const int32_t v = static_cast<int32_t>(d.intValue);
        std::memcpy(cell, &v, sizeof(v));
        break;
    }
    case RTK_Long:
    case RTK_ULong:
        std::memcpy(cell, &d.longValue, sizeof(d.longValue));
        break;
    case RTK_Float:
        std::memcpy(cell, &d.floatValue, sizeof(d.floatValue));
        break;
    case RTK_Double:
        std::memcpy(cell, &d.doubleValue, sizeof(d.doubleValue));
        break;
    case RTK_Null: {
        const int32_t nullHandle = 0;
        std::memcpy(cell, &nullHandle, sizeof(nullHandle));
        break;
    }
    case RTK_String: {
        //Post-link the index is remapped into this module's table;
        //mint a runtime string so the value behaves like any other.
        if (d.stringIdx >= m_currModule->stringConstants.size())
            throw std::runtime_error(
                "NLang VM: default string index out of range");
        const int32_t handle =
            MintNewString(m_currModule->stringConstants[d.stringIdx]);
        std::memcpy(cell, &handle, sizeof(handle));
        break;
    }
    case RTK_Void:
    default:
        throw std::runtime_error(
            "NLang VM: missing argument without a constant default on "
            + callee.name + " (embedding entry cannot rebuild it)");
    }
}

//--- 宿主值桥 -------------------------------------------------------------
//（每个方法 3-12 行，包装既有机制；实现时逐一对照私有机制名）

int32_t VmExecutor::MintHostString(const std::string& content) {
    return MintNewString(content);
}

//Extension point ⑤'s raise door: a host C++ failure becomes a catchable
//script Exception. Backtrace/message population lives in the private
//raiser — this wrapper only makes it reachable from the adapter.
void VmExecutor::RaiseHostException(const std::string& msg) {
    RaiseNlangExceptionBase(msg);
}

//Extension point ③: registration half. The embed adapter pushes a root
//when a reference Value is created and pops when its last shared copy
//dies; MarkPhase reads the registry during collection.
void VmExecutor::PushHostRoot(uint8_t kind, int32_t value) {
    m_hostRoots.push_back(HostRoot{kind, value});
}

//Reverse scan erases the most recently pushed matching pair — LIFO keeps
//nested push/pop of equal values balanced (Value copy + destroy order).
void VmExecutor::PopHostRoot(uint8_t kind, int32_t value) {
    for (auto it = m_hostRoots.rbegin(); it != m_hostRoots.rend(); ++it) {
        if (it->kind == kind && it->value == value) {
            m_hostRoots.erase(std::next(it).base());
            return;
        }
    }
}

int32_t VmExecutor::BoxHostScalar(uint8_t typeTag, int64_t bits) {
    return AllocBoxedValue(typeTag, bits);
}

int32_t VmExecutor::AllocHostList() {
    if (m_listClassIdx < 0)
        throw std::runtime_error("NLang VM: List class not registered");
    int32_t heapIdx = AllocClassOnHeap(
        static_cast<uint16_t>(m_listClassIdx));
    int32_t handle = AllocListHandle();
    m_structHeap[static_cast<size_t>(heapIdx)][kListHandleFieldOffset]
        = handle;
    return heapIdx;
}

int32_t VmExecutor::AllocHostDict() {
    if (m_dictClassIdx < 0)
        throw std::runtime_error("NLang VM: Dict class not registered");
    int32_t heapIdx = AllocClassOnHeap(
        static_cast<uint16_t>(m_dictClassIdx));
    //Dict instances carry the side-table handle in the SAME field slot
    //as List (slot[1] = kListHandleFieldOffset) — pinned at
    //VmExecutorDebug.cpp:193-196 (FormatDebugHeapValue routes both
    //containers through it), so the shared constant is correct here.
    int32_t handle = AllocDictHandle();
    m_structHeap[static_cast<size_t>(heapIdx)][kListHandleFieldOffset]
        = handle;
    return heapIdx;
}

void VmExecutor::HostListPushBack(int32_t listHeapIdx,
                                  int32_t elemHeapIdx) {
    auto& slot = m_structHeap[static_cast<size_t>(listHeapIdx)];
    const int32_t handle = slot[kListHandleFieldOffset];
    m_listStore[static_cast<size_t>(handle) - 1].elements.push_back(
        elemHeapIdx);
}

void VmExecutor::HostDictUpsert(int32_t dictHeapIdx, int32_t keyHeapIdx,
                                int32_t valHeapIdx) {
    auto& slot = m_structHeap[static_cast<size_t>(dictHeapIdx)];
    const int32_t handle = slot[kListHandleFieldOffset];
    auto& entries =
        m_dictStore[static_cast<size_t>(handle) - 1].entries;
    for (auto& e : entries) {
        if (HostKeysEqual(e.first, keyHeapIdx)) {
            e.second = valHeapIdx;
            return;
        }
    }
    entries.emplace_back(keyHeapIdx, valHeapIdx);
}

bool VmExecutor::HostKeysEqual(int32_t aHeapIdx, int32_t bHeapIdx) {
    //内容等价（装箱按 tag/位，引用按恒等）——转发内建 Dict 的比较器
    //（已核验：两参形态，经 m_slotKinds 自带 kind 感知）。
    if (aHeapIdx == bHeapIdx)
        return true;
    return DictKeysEqual(aHeapIdx, bHeapIdx);
}

uint32_t VmExecutor::HostArrayLength(int32_t heapIdx) const {
    return static_cast<uint32_t>(
        m_structHeap[static_cast<size_t>(heapIdx)][2]);
}

void VmExecutor::HostArrayGetCell(int32_t heapIdx, uint32_t index,
                                  uint8_t outCell[8]) const {
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    if (index >= HostArrayLength(heapIdx))
        throw std::runtime_error("NLang VM: host array index out of bounds");
    //Heap layout fact: [1]=arrayTypeIdx (VmExecutorAlloc.cpp:75-110).
    const auto& at = m_currModule->arrayTypes[
        static_cast<size_t>(slot[1])];
    const int cells = ArrayElemCells(at.elemKind);
    const size_t base = 3 + static_cast<size_t>(index) * cells;
    std::memset(outCell, 0, kFrameSlotBytes);
    std::memcpy(outCell, &slot[base], sizeof(int32_t));
    if (cells == 2)
        std::memcpy(outCell + sizeof(int32_t), &slot[base + 1],
                    sizeof(int32_t));
}

void VmExecutor::HostArraySetCell(int32_t heapIdx, uint32_t index,
                                  const uint8_t cell[8]) {
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    if (index >= HostArrayLength(heapIdx))
        throw std::runtime_error(
            "NLang VM: host array set out of bounds");
    const auto& at = m_currModule->arrayTypes[
        static_cast<size_t>(slot[1])];
    const int cells = ArrayElemCells(at.elemKind);
    const size_t base = 3 + static_cast<size_t>(index) * cells;
    std::memcpy(&slot[base], cell, sizeof(int32_t));
    if (cells == 2)
        std::memcpy(&slot[base + 1], cell + sizeof(int32_t),
                    sizeof(int32_t));
}

uint32_t VmExecutor::HostListSize(int32_t heapIdx) const {
    const int32_t handle =
        m_structHeap[static_cast<size_t>(heapIdx)][kListHandleFieldOffset];
    return static_cast<uint32_t>(
        m_listStore[static_cast<size_t>(handle) - 1].elements.size());
}

int32_t VmExecutor::HostListGet(int32_t heapIdx, uint32_t index) const {
    const int32_t handle =
        m_structHeap[static_cast<size_t>(heapIdx)][kListHandleFieldOffset];
    const auto& elements =
        m_listStore[static_cast<size_t>(handle) - 1].elements;
    if (index >= elements.size())
        throw std::runtime_error("NLang VM: host list index out of bounds");
    return elements[index];
}

bool VmExecutor::HostDictEntryGet(int32_t heapIdx, uint32_t index,
                                  int32_t& keyOut, int32_t& valOut) const {
    const int32_t handle =
        m_structHeap[static_cast<size_t>(heapIdx)][kListHandleFieldOffset];
    const auto& entries =
        m_dictStore[static_cast<size_t>(handle) - 1].entries;
    if (index >= entries.size())
        return false;
    keyOut = entries[index].first;
    valOut = entries[index].second;
    return true;
}

uint32_t VmExecutor::HostDictSize(int32_t heapIdx) const {
    const int32_t handle =
        m_structHeap[static_cast<size_t>(heapIdx)][kListHandleFieldOffset];
    return static_cast<uint32_t>(
        m_dictStore[static_cast<size_t>(handle) - 1].entries.size());
}

bool VmExecutor::HostBoxedRead(int32_t boxHeapIdx, uint8_t& tagOut,
                               int64_t& bitsOut) const {
    const auto& slot = m_structHeap[static_cast<size_t>(boxHeapIdx)];
    if (slot.size() < 3)
        return false;
    tagOut = static_cast<uint8_t>(slot[0]);
    const int64_t lo = static_cast<int64_t>(
        static_cast<uint32_t>(slot[kBoxedValueSlot]));
    const int64_t hi = static_cast<int64_t>(
        static_cast<uint32_t>(slot[kBoxedValueSlot + 1]));
    bitsOut = lo | (hi << 32);
    return true;
}

//Descriptor + base cell for a host field access. The heap slot's
//runtime kind decides the descriptor table: RTK_Class reads the class
//idx from slot[0]; RTK_Struct reads m_slotStructIdx (structs[] is a
//separate table). CompiledStruct and CompiledClass are UNRELATED types
//(no inheritance, CompiledModule.h:148/:384) whose field members merely
//share names, so the descriptor returns as a small nested view struct
//(HostFieldDesc below) — one code path serves both tables. Both use
//the uniform 2-cells-per-field stride.
VmExecutor::HostFieldDesc VmExecutor::ResolveHostField(
        int32_t heapIdx, uint16_t fieldIndex, size_t& baseOut) const {
    if (heapIdx < 0
            || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: host field access on bad heap idx");
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const uint8_t kind = m_slotKinds[static_cast<size_t>(heapIdx)];
    HostFieldDesc desc = {};
    size_t base = 0;
    if (kind == RTK_Struct) {
        const uint16_t idx =
            m_slotStructIdx[static_cast<size_t>(heapIdx)];
        if (idx >= m_currModule->structs.size())
            throw std::runtime_error("NLang VM: struct idx out of range");
        const CompiledStruct& st = m_currModule->structs[idx];
        desc.fieldNames = &st.fieldNames;
        desc.fieldTypeKinds = &st.fieldTypeKinds;
        desc.fieldCount = st.fieldCount;
        base = static_cast<size_t>(fieldIndex) * 2;   //uniform stride
    } else if (kind == RTK_Class) {
        const uint16_t idx = static_cast<uint16_t>(slot[0]);
        if (idx >= m_currModule->classes.size())
            throw std::runtime_error("NLang VM: class idx out of range");
        const CompiledClass& cl = m_currModule->classes[idx];
        desc.fieldNames = &cl.fieldNames;
        desc.fieldTypeKinds = &cl.fieldTypeKinds;
        desc.fieldCount = cl.fieldCount;
        base = 1 + static_cast<size_t>(fieldIndex) * 2;  //[0]=class id
    } else {
        throw std::runtime_error(
            "NLang VM: host field access on non-instance slot");
    }
    if (fieldIndex >= desc.fieldCount)
        throw std::runtime_error("NLang VM: field index out of range");
    if (base + 1 >= slot.size())
        throw std::runtime_error("NLang VM: instance slot too small");
    baseOut = base;
    return desc;
}

uint8_t VmExecutor::HostFieldCellGet(int32_t heapIdx, uint16_t fieldIndex,
                                     uint8_t outCell[8]) const {
    size_t base = 0;
    const HostFieldDesc desc = ResolveHostField(heapIdx, fieldIndex, base);
    const uint8_t declaredKind =
        static_cast<uint8_t>((*desc.fieldTypeKinds)[fieldIndex]);
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    std::memset(outCell, 0, kFrameSlotBytes);
    std::memcpy(outCell, &slot[base], sizeof(int32_t));
    if (declaredKind == RTK_Long || declaredKind == RTK_ULong
            || declaredKind == RTK_Double)
        std::memcpy(outCell + sizeof(int32_t), &slot[base + 1],
                    sizeof(int32_t));
    return declaredKind;
}

void VmExecutor::HostFieldCellSet(int32_t heapIdx, uint16_t fieldIndex,
                                  const uint8_t cell[8]) {
    size_t base = 0;
    const HostFieldDesc desc = ResolveHostField(heapIdx, fieldIndex, base);
    const uint8_t declaredKind =
        static_cast<uint8_t>((*desc.fieldTypeKinds)[fieldIndex]);
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    std::memcpy(&slot[base], cell, sizeof(int32_t));
    if (declaredKind == RTK_Long || declaredKind == RTK_ULong
            || declaredKind == RTK_Double)
        std::memcpy(&slot[base + 1], cell + sizeof(int32_t),
                    sizeof(int32_t));
}

bool VmExecutor::HostFieldNameToIndex(int32_t heapIdx,
                                      const std::string& fieldName,
                                      uint16_t& indexOut) const {
    //fieldIndex=0 sentinel note: a ZERO-field instance makes
    //ResolveHostField throw "field index out of range" instead of
    //returning false — acceptable (any getField on a fieldless
    //instance fails either way; the adapter translates the
    //runtime_error to BadValue).
    size_t base = 0;   //validates heapIdx/instance-ness as a side effect
    const HostFieldDesc desc = ResolveHostField(heapIdx, 0, base);
    for (uint16_t i = 0; i < desc.fieldCount; ++i) {
        if ((*desc.fieldNames)[i] == fieldName) {
            indexOut = i;
            return true;
        }
    }
    return false;
}

std::string VmExecutor::HostClassName(int32_t heapIdx) const {
    if (heapIdx < 0
            || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: host class name on bad heap idx");
    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    if (slot.empty() || m_slotKinds[static_cast<size_t>(heapIdx)] != RTK_Class)
        throw std::runtime_error(
            "NLang VM: host class name on non-class slot");
    const uint16_t idx = static_cast<uint16_t>(slot[0]);
    if (idx >= m_currModule->classes.size())
        throw std::runtime_error("NLang VM: class idx out of range");
    return m_currModule->classes[idx].name;
}

}  // namespace nlang
