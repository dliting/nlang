// --- VmExecutor debug view: static slots (ndb `i statics`) ---
// v0.12.6 Task 9: the IVmDebugView::StaticSlots implementation plus its
// two rendering helpers (owner init phase, one cell by declared kind).
// Same TU discipline as VmExecutorDebug.cpp: frozen-time, const, no
// NLang code execution, no NLang-heap allocation.

#include "VmExecutor.h"
#include "IDebugHooks.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstring>
#include <string>
#include <vector>

namespace nlang {

//v0.12.6 Task 9: the module's static slots for the debug view. Values
//render through the same channels as frame locals (the cell is the
//uniform value representation); the init phase belongs to the owner
//machine — class/interface slots resolve their owner by the record key,
//static-local slots read their own per-slot machine.
std::vector<DebugStaticSlotInfo> VmExecutor::StaticSlots() const {
    VmScheduler::GilEnsureGuard gil(*m_spScheduler);
    std::vector<DebugStaticSlotInfo> out;
    if (!m_currModule)
        return out;
    const size_t count = m_staticSlots.Count();
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const CompiledStaticSlot& slot = m_currModule->staticSlots[i];
        DebugStaticSlotInfo row;
        row.name = slot.name;
        row.ownerKey = slot.ownerKey;
        row.ownerKind = slot.ownerKind;
        row.display = FormatDebugStaticCell(slot.type.kind,
                                            m_staticSlots.Cell(i));
        row.initPhase = StaticSlotInitPhase(slot, i);
        out.push_back(std::move(row));
    }
    return out;
}

//Owner lookup for one slot's init phase. The slot table carries the
//owner KEY (not an index), so class/interface slots scan their record
//table by name — frozen-time cost on a tiny table. An unresolved key
//(linker naming drift) renders "" rather than failing the display.
//Owners with no initialization machinery emit no phase either: an
//initializer-less class/interface has no synthesized $staticInit (the
//gate is never emitted, D9 zero cost) and an initializer-less static
//local never claims its machine — the state would read "uninitialized"
//forever while the slots are perfectly live, so "" is the honest row.
std::string VmExecutor::StaticSlotInitPhase(const CompiledStaticSlot& slot,
                                            size_t slotIdx) const {
    const VmStaticSlots::StaticOwnerState* owner = nullptr;
    if (slot.ownerKind == 2) {
        if (!slot.hasInitializer)
            return "";
        owner = m_staticSlots.SlotMachine(slotIdx);
    } else if (slot.ownerKind == 0) {
        for (size_t c = 0; c < m_currModule->classes.size(); ++c) {
            const CompiledClass& cc = m_currModule->classes[c];
            if (cc.name != slot.ownerKey)
                continue;
            if (cc.staticInitFuncIdx == 0xFFFF)
                return "";
            owner = m_staticSlots.Owner(c);
            break;
        }
    } else {
        for (size_t f = 0; f < m_currModule->interfaces.size(); ++f) {
            const CompiledInterface& ci = m_currModule->interfaces[f];
            if (ci.name != slot.ownerKey)
                continue;
            if (ci.staticInitFuncIdx == 0xFFFF)
                return "";
            owner = m_staticSlots.OwnerByKind(1, f);
            break;
        }
    }
    if (!owner)
        return "";
    switch (owner->phase.load(std::memory_order_relaxed)) {
    case VmStaticSlots::StaticOwnerState::kInitializing:
        return "initializing";
    case VmStaticSlots::StaticOwnerState::kDone:
        return "done";
    default:
        return "uninitialized";
    }
}

//One static-slot cell by declared kind — the same routing as
//FormatDebugLocalSlot, minus the char identity tag (a static cell's
//declared kind never narrows to char through the slot table).
std::string VmExecutor::FormatDebugStaticCell(uint8_t kind,
                                              const uint8_t* cell) const {
    int32_t lo = 0, hi = 0;
    std::memcpy(&lo, cell, sizeof(lo));
    std::memcpy(&hi, cell + sizeof(hi), sizeof(hi));
    switch (kind) {
    case RTK_String:
        return FormatDebugStringIdx(lo);
    case RTK_Struct:
    case RTK_Class:
    case RTK_Array:
    case RTK_Func:
        return FormatDebugHeapValue(lo);
    default:
        if (ScalarPrimIndexOfRtk(kind) >= 0)
            return FormatScalarValue(kind, lo, hi);
        return "<unknown>";
    }
}

} //namespace nlang
