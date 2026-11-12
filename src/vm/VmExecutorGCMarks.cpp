/*---
    VmExecutorGCMarks.cpp — mark-phase tracing of value-
    bearing records: boxed payloads, class/struct fields and
    array elements, with the declared-vs-runtime acceptance
    predicates. Split out of VmExecutorGC.cpp (2026-11-12
    source-size split, zero behavior change).
---*/
#include "VmExecutor.h"
#include <cstdint>
#include <vector>

namespace nlang {

//String objectization: a boxed string's payload (slot[1]) is a
//string handle. Other boxed tags carry raw bits (no children).
void VmExecutor::MarkBoxedPayload(int32_t idx) {
    if (m_structHeap[idx][0] == RTK_String)
        MarkString(m_structHeap[idx][1]);
}

namespace {
//Single source for the class/struct field arms' declared-vs-runtime
//double condition (was two hand-expanded four-way conditions — same
//shape as IsChildBearingHeapKind/ArrayElemKindAccepts). Object-declared
//fields also hold boxed primitive records (runtime RTK_Boxed) and,
//since arrays are Object values (§4.11.1), array records owned or view
//(final-review C1: without these arms the sweep frees a reachable
//array and the field keeps a dangling handle). An array-declared slot
//may hold a VIEW record at runtime (the unannotated Array<T> boundary
//admits any rank) — the worklist's array/view arms walk them.
inline bool DeclaredFieldAcceptsRuntime(uint8_t declaredKind,
                                        uint8_t runtimeKind) {
    return (declaredKind == RTK_Class
                && (runtimeKind == RTK_Class || runtimeKind == RTK_Boxed
                    || runtimeKind == RTK_Array
                    || runtimeKind == RTK_ArrayView))
        || (declaredKind == RTK_Struct && runtimeKind == RTK_Struct)
        || (declaredKind == RTK_Func && runtimeKind == RTK_Func)
        || (declaredKind == RTK_Array
                && (runtimeKind == RTK_Array
                    || runtimeKind == RTK_ArrayView));
}
} // namespace

void VmExecutor::MarkClassFields(int32_t idx,
                                 std::vector<int32_t>& worklist) {
    int32_t classIdx = m_structHeap[idx][0];
    //0.9.12 T1: declared field kinds come from the layout view.
    const auto& fields = m_classLayout.Fields(static_cast<size_t>(classIdx));
    for (size_t i = 0; i < fields.size(); ++i) {
        int32_t refIdx = m_structHeap[idx][i * 2 + 1];
        //String fields carry a string-store handle, not a heap
        //index — route before the heap-index guard (a valid handle
        //may lie beyond the struct heap's size). No runtime slot-
        //kind double condition: the string store has no slotKinds.
        if (fields[i].kind == RTK_String) {
            MarkString(refIdx);
            continue;
        }
        //A T field's representation depends on the instantiation (store
        //handle, heap index or inline scalar) — test both domains, the
        //same conservative routing the param scan and the frame band
        //apply to RTK_TypeParam slots.
        if (fields[i].kind == RTK_TypeParam) {
            TestRootValue(refIdx, worklist);
            continue;
        }
        if (refIdx <= 0 || static_cast<size_t>(refIdx) >= m_slotKinds.size())
            continue;
        const uint8_t refKind = m_slotKinds[static_cast<size_t>(refIdx)];
        //Array redesign B: field kinds come from the declared type,
        //so RTK_Array fields are routed explicitly (same declared-
        //plus-runtime double condition as the reference kinds).
        //The old unconditional runtime-kind fallback is safe to drop:
        //jagged declaration forms that could smuggle an array record
        //into a non-array field slot are rejected at resolve time.
        //Pre-fix v1.9 modules also misfile array fields as
        //RTK_Int32 here; the v1.10 loader floor (same release)
        //refuses them, closing the GC under-trace window.
        //Object-declared fields also hold boxed primitive records —
        //push those too; the worklist's boxed arm marks a wrapped
        //string payload. Without this arm the sweep frees the
        //box while the field still points at it.
        if (DeclaredFieldAcceptsRuntime(fields[i].kind, refKind)) {
            if (!m_markBits[refIdx])
                PushMarked(refIdx, worklist);
        }
    }
}

void VmExecutor::MarkStructFields(int32_t idx,
                                  std::vector<int32_t>& worklist) {
    uint16_t structIdx = m_slotStructIdx[idx];
    auto& cs = m_currModule->structs[structIdx];
    for (uint16_t i = 0; i < cs.fieldCount; ++i) {
        int32_t refIdx = m_structHeap[idx][i * 2];
        //String fields carry a string-store handle, not a heap
        //index — same pre-guard routing as the class field arm.
        if (cs.fieldTypeKinds[i] == RTK_String) {
            MarkString(refIdx);
            continue;
        }
        if (refIdx <= 0 || static_cast<size_t>(refIdx) >= m_slotKinds.size())
            continue;
        const uint8_t refKind = m_slotKinds[static_cast<size_t>(refIdx)];
        //Explicit RTK_Array route (declared kind now authoritative) —
        //see the RTK_Class branch above for why the old runtime-kind
        //fallback is gone, including the v1.10 module-floor coupling.
        //Acceptance is the class arm's shared predicate.
        if (DeclaredFieldAcceptsRuntime(cs.fieldTypeKinds[i], refKind)) {
            if (!m_markBits[refIdx])
                PushMarked(refIdx, worklist);
        }
    }
}

namespace {
//Single source for the array-element trace acceptance (was four
//hand-expanded else-if arms): declared elemKind must match the record's
//runtime slot kind. Object[] elements carry boxed primitive records
//(declared RTK_Class, runtime RTK_Boxed) and array records owned or
//view (arrays are Object values, §4.11.1 — final-review C1) — same
//acceptance as the field arms. The RTK_Array row (an array whose
//elements are themselves array records, array redesign B spec §5.3#1)
//is currently unreachable from compilable source (jagged declarations
//rejected; List<int[]> elements live in the container store) —
//defensive base for future/external .nmod paths, kept symmetric with
//the field predicate (a view element rides the view arm). String
//elements never reach this test (they carry store handles, routed
//before it).
inline bool ArrayElemKindAccepts(uint8_t elemKind, uint8_t runtimeKind)
{
    return (elemKind == RTK_Class
                && (runtimeKind == RTK_Class || runtimeKind == RTK_Boxed
                    || runtimeKind == RTK_Array
                    || runtimeKind == RTK_ArrayView))
        || (elemKind == RTK_Struct && runtimeKind == RTK_Struct)
        || (elemKind == RTK_Func && runtimeKind == RTK_Func)
        || (elemKind == RTK_Array
                && (runtimeKind == RTK_Array
                    || runtimeKind == RTK_ArrayView));
}
} // namespace

void VmExecutor::MarkArrayElements(int32_t idx,
                                   std::vector<int32_t>& worklist) {
    uint16_t arrayTypeIdx = m_slotStructIdx[idx];
    auto& at = m_currModule->arrayTypes[arrayTypeIdx];
    //Stage-2 header: [3]=elemCount; the payload base derives from
    //the record's own rank cell.
    const size_t payloadBase = ArrayPayloadBase(m_structHeap[idx]);
    int32_t length = m_structHeap[idx][3];
    //0.7.5: stride from the registry — reference-bearing kinds are all
    //1-cell (the acceptance test below unchanged); 2-cell scalars hold
    //no references, the honest offset just keeps the reads
    //element-aligned.
    const int cells = ArrayElemCells(at.elemKind);
    for (int32_t i = 0; i < length; ++i) {
        int32_t elemRef =
            m_structHeap[idx][payloadBase + i * cells];
        //String elements are raw string handles (hole-1 uses 0 =
        //null) — same pre-guard routing as the field arms.
        if (at.elemKind == RTK_String) {
            MarkString(elemRef);
            continue;
        }
        if (elemRef <= 0 || static_cast<size_t>(elemRef) >= m_slotKinds.size())
            continue;
        if (ArrayElemKindAccepts(at.elemKind, m_slotKinds[elemRef])
                && !m_markBits[elemRef]) {
            PushMarked(elemRef, worklist);
        }
    }
}
} // namespace nlang
