/*---
VmExecutorNativeCallbacks.cpp — native-host callback implementations
split from VmExecutorNativeHost.cpp when that file reached the
source-size budget: the exception-family mapping of raiseException and
the array side (getArrayView's pinned payload view, the minting family
through newArray/newArrayShaped/newArrayView, ABI v6 through v10).
Table wiring stays in InitNativeHost.
---*/
#include "VmExecutor.h"
#include "VmExecutorNativeHost.h"

#include <stdexcept>
#include <string>

namespace nlang {

namespace {
//A native side holds NativeHost* equal to &vmNativeHost.c (the first
//member); restore the wrapper.
VmNativeHost* Wrap(NativeHost* self) {
    return reinterpret_cast<VmNativeHost*>(self);
}
} // namespace

void VmExecutor::NativeRaiseException(NativeHost* self, int exceptionKind,
                                      const char* message) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    //K8: keeps the store lock — FireOnThrow is a scheduler-facing
    //checkpoint (the OpThrow family keeps its guard for the same
    //reason); restated with the scheduler lock in Step 7.
    //K8: keeps the store lock — FireOnThrow is a scheduler-facing
    //checkpoint (the OpThrow family keeps its guard for the same
    //reason); restated with the scheduler lock in Step 7.
    //v6: native kernels raise the same catchable families the language
    //runtime does. A negative class index (builtin class absent from the
    //linked module — defensive, every module registers them) falls back
    //to the base Exception.
    int16_t classIdx = e->m_exceptionClassIdx;
    switch (exceptionKind) {
        case NEXC_IOException:   classIdx = e->m_ioExcClassIdx; break;
        case NEXC_NullPointer:   classIdx = e->m_nullPtrExcClassIdx; break;
        case NEXC_OutOfBounds:   classIdx = e->m_oobExcClassIdx; break;
        case NEXC_DivideByZero:  classIdx = e->m_divZeroExcClassIdx; break;
        default:                 classIdx = e->m_exceptionClassIdx; break;
    }
    if (classIdx < 0)
        classIdx = e->m_exceptionClassIdx;
    //Build-then-root-then-throw: between this throw and the blocking
    //arm's catch the GIL is away (the unwinding guard above releases
    //it), and a GC on another carrier must not sweep the in-flight
    //exception object while the native unwinds toward it.
    const int32_t heapIdx = e->BuildNlangException(
        classIdx, message ? message : "");
    e->RootMintedNativeHandle(heapIdx);
    e->FireOnThrow();
    throw NLangThrow(heapIdx, message ? message : "");
}

uint32_t VmExecutor::NativeNextRandom(NativeHost* self) {
    VmExecutor* e = Wrap(self)->executor;
    //K8: the shared engine serializes draws and reseeds on the RNG
    //mutex (host-boundary leaf — the engine is not a store).
    std::lock_guard<std::mutex> rngGuard(e->m_rngMutex);
    return e->m_rng();
}

void VmExecutor::NativeSeedRandom(NativeHost* self, int32_t seed) {
    VmExecutor* e = Wrap(self)->executor;
    //K8: the shared engine serializes draws and reseeds on the RNG
    //mutex (host-boundary leaf — the engine is not a store).
    std::lock_guard<std::mutex> rngGuard(e->m_rngMutex);
    e->m_rng.seed(static_cast<uint32_t>(seed));
}

//Contract (NativeHost.h): the view's pointers are valid for the duration
//of this native call only. The GC is non-moving and the argument record
//is rooted (staged-argument address roots in the blocking protocol, the
//caller's frame in the non-blocking path), so the payload pointer cannot
//stale mid-call. Pointer math uses data()+offset throughout — &v[size]
//would be UB for zero-length records.

//View filling for NativeGetArrayView: row-major strides computed into
//the view's own inline storage (ABI v7) — every live view owns its
//strides; the v6 host scratch let a second view corrupt the first's.
//Dims and data point into the validated record header (contract: the
//pointers stay valid for the duration of the native call only). The
//record is VmExecutor's heap StructSlot (a vector of int32 cells),
//taken non-const because the view exposes its raw storage.
static void FillArrayView(NativeArrayView* out,
                          std::vector<int32_t>& record,
                          const CompiledArrayType& at,
                          size_t payloadBase) {
    //Stage-2 header: [2]=rank, [3]=elemCount, [4..]=dims, payload after
    //the padded header.
    const int32_t rank = record[2];
    const int64_t elemCount = record[3];
    int32_t stride = 1;
    for (int32_t d = rank - 1; d >= 0; --d)
    {
        out->stridesInline[d] = stride;
        stride *= record[4 + d];
    }
    out->rank = rank;
    out->dims = record.data() + 4;   //the dims run
    out->strides = out->stridesInline;
    out->elemCount = elemCount;
    out->elemKind = at.elemKind;
    out->elemCellBytes = static_cast<uint8_t>(
        ArrayElemCells(at.elemKind) * kHeapCellBytes);
    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->offset = 0;   //owning array: data already points at element zero
    out->data = record.data() + payloadBase;   //payload base
}

//ABI v10 view arm of getArrayView: fill from the VIEW record's own
//dims/strides/offset (owner-payload-space element units), with data
//pointing at the OWNING record's payload base — element (i0,..) lives
//at data + (offset + sum(ik*strides[k])) * elemCellBytes. The view's
//type row carries the element kind (creation validated it against the
//owner, and the rank may differ from the owner's — slicing reduces it).
static void FillViewArrayView(NativeArrayView* out,
                              std::vector<int32_t>& view,
                              std::vector<int32_t>& owner,
                              const CompiledArrayType& at,
                              size_t ownerPayloadBase) {
    const int32_t rank = view[2];
    for (int32_t d = 0; d < rank; ++d)
        out->stridesInline[d] =
            view[ArrayViewStridesBase(static_cast<uint8_t>(rank)) + d];
    out->rank = rank;
    out->dims = view.data() + 4;
    out->strides = out->stridesInline;
    out->elemCount = view[3];
    out->elemKind = at.elemKind;
    out->elemCellBytes = static_cast<uint8_t>(
        ArrayElemCells(at.elemKind) * kHeapCellBytes);
    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->offset = view[ArrayViewOffsetCell(static_cast<uint8_t>(rank))];
    out->data = owner.data() + ownerPayloadBase;
}

//ABI v10 view-record arm of NativeGetArrayView (declaration in
//VmExecutor.h): fill from the VIEW record's own recorded strides/offset,
//with data at the OWNING record's payload base. Corrupt-record checks
//are VM invariant breaks (runtime_error), like the owning arm's reads.
void VmExecutor::FillViewRecordArrayView(VmExecutor* e, int32_t handle,
                                         NativeArrayView* out) {
    auto& view = e->m_structHeap[static_cast<size_t>(handle)];
    const int32_t viewRank = view[2];
    if (viewRank < 1 || viewRank > kMaxArrayRank)
        throw std::runtime_error(
            "NLang VM: corrupt view record (rank)");
    const int32_t target = view[ArrayViewTargetCell(
        static_cast<uint8_t>(viewRank))];
    if (target <= 0
        || static_cast<size_t>(target) >= e->m_structHeap.size()
        || e->m_slotKinds[static_cast<size_t>(target)] != RTK_Array)
        throw std::runtime_error(
            "NLang VM: corrupt view record (target out of range)");
    auto& owner = e->m_structHeap[static_cast<size_t>(target)];
    const int32_t viewTypeIdx = view[1];
    if (viewTypeIdx < 0
        || static_cast<size_t>(viewTypeIdx)
            >= e->m_currModule->arrayTypes.size())
        e->RaiseNlangExceptionBase(
            "getArrayView: corrupt array type index");
    FillViewArrayView(out, view, owner,
        e->m_currModule->arrayTypes[static_cast<size_t>(viewTypeIdx)],
        ArrayPayloadBase(owner));
}

int VmExecutor::NativeGetArrayView(NativeHost* self, int32_t handle,
                                   NativeArrayView* out) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    if (!out)
        e->RaiseNlangExceptionBase("getArrayView: null out view");
    //A dead handle (0, negative, or past the table) reads as null — the
    //live-language null array is handle 0; the rest is defensive.
    if (handle <= 0
        || static_cast<size_t>(handle) >= e->m_slotKinds.size()) {
        e->RaiseNlangException(e->m_nullPtrExcClassIdx,
            "getArrayView: array is null");
    }
    const uint8_t kind = e->m_slotKinds[static_cast<size_t>(handle)];
    if (kind != RTK_Array && kind != RTK_ArrayView) {
        e->RaiseNlangExceptionBase("getArrayView: value is not an array");
    }
    //ABI v10: a view handle fills from its own record (recorded
    //strides/offset) with data at the owning record's payload base.
    if (kind == RTK_ArrayView) {
        FillViewRecordArrayView(e, handle, out);
        return 1;
    }
    auto& record = e->m_structHeap[static_cast<size_t>(handle)];
    //Validate before reading any header cell: ArrayPayloadBase covers the
    //fixed header, the rank range, and the dims run in one place. The
    //arrayTypes index is bounds-checked here too — a corrupt record[1]
    //must not index the module table (or the record) out of range.
    const size_t payloadBase = ArrayPayloadBase(record);
    const int32_t arrayTypeIdx = record[1];
    const auto& arrayTypes = e->m_currModule->arrayTypes;
    if (arrayTypeIdx < 0
        || static_cast<size_t>(arrayTypeIdx) >= arrayTypes.size())
        e->RaiseNlangExceptionBase("getArrayView: corrupt array type index");
    FillArrayView(out, record,
                  arrayTypes[static_cast<size_t>(arrayTypeIdx)], payloadBase);
    return 1;
}

int32_t VmExecutor::NativeNewArray(NativeHost* self, uint8_t elemKind,
                                   int32_t length) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    //NEK values mirror the RTK scalar codes (NativeHost.h tie comment) —
    //accept exactly the four mintable kinds, reject the rest by name.
    if (elemKind != NEK_Int32 && elemKind != NEK_Int64
        && elemKind != NEK_Float32 && elemKind != NEK_Float64) {
        e->RaiseNlangExceptionBase(
            "newArray: element kind is not a mintable scalar");
    }
    //Length bounds (negative and cap) raise in AllocArrayOnHeap — the
    //choke point shared with the language's sized creation. Native
    //minting is rank-1 in stage 1; the single dim is the length.
    const uint16_t arrayTypeIdx = e->FindScalarArrayTypeIdx(elemKind, 1);
    const int32_t handle = e->AllocArrayOnHeap(arrayTypeIdx, 1, &length);
    //Root across the blocking section: the native holds this handle while
    //filling the payload, and other carriers may GC before it reaches ret.
    e->RootMintedNativeHandle(handle);
    return handle;
}

int32_t VmExecutor::NativeNewArrayShaped(NativeHost* self, uint8_t elemKind,
                                         int32_t rank, const int32_t* dims) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    //Same four mintable kinds as newArray (rationale there).
    if (elemKind != NEK_Int32 && elemKind != NEK_Int64
        && elemKind != NEK_Float32 && elemKind != NEK_Float64) {
        e->RaiseNlangExceptionBase(
            "newArrayShaped: element kind is not a mintable scalar");
    }
    //The language cap is the ABI contract: a mint above kMaxArrayRank
    //could never be addressed from NLang code.
    if (rank < 1 || rank > kMaxArrayRank) {
        e->RaiseNlangExceptionBase("newArrayShaped: rank out of range");
    }
    //Per-dim bounds (negative, cap, product) raise in AllocArrayOnHeap —
    //the choke point shared with the language's sized creation.
    const uint16_t arrayTypeIdx = e->FindScalarArrayTypeIdx(elemKind,
        static_cast<uint8_t>(rank));
    const int32_t handle = e->AllocArrayOnHeap(arrayTypeIdx,
        static_cast<uint8_t>(rank), dims);
    //Root across the blocking section (same discipline as newArray).
    e->RootMintedNativeHandle(handle);
    return handle;
}

//ABI v10 owner resolution for NativeNewArrayView (declaration in
//VmExecutor.h): depth-one peel to the owning record (the view-record
//invariant), then the owner's element kind picks the minted rank's type
//row. Raises for a non-array owner and a non-scalar element kind; a
//corrupt view record is a VM invariant break (runtime_error).
uint16_t VmExecutor::ResolveViewOwnerType(VmExecutor* e, int32_t targetHandle,
                                          uint8_t rank) {
    int32_t ownerIdx = targetHandle;
    if (e->m_slotKinds[static_cast<size_t>(ownerIdx)] == RTK_ArrayView) {
        const int32_t viewRank =
            e->m_structHeap[static_cast<size_t>(ownerIdx)][2];
        ownerIdx = e->m_structHeap[static_cast<size_t>(ownerIdx)]
            [ArrayViewTargetCell(static_cast<uint8_t>(viewRank))];
        if (ownerIdx <= 0
            || static_cast<size_t>(ownerIdx) >= e->m_structHeap.size())
            throw std::runtime_error(
                "NLang VM: corrupt view record (target out of range)");
    }
    if (e->m_slotKinds[static_cast<size_t>(ownerIdx)] != RTK_Array)
        e->RaiseNlangException(e->m_oobExcClassIdx,
            "newArrayView: target is not an array");
    const int32_t ownerTypeIdx =
        e->m_structHeap[static_cast<size_t>(ownerIdx)][1];
    if (ownerTypeIdx < 0
        || static_cast<size_t>(ownerTypeIdx)
            >= e->m_currModule->arrayTypes.size())
        e->RaiseNlangExceptionBase(
            "newArrayView: corrupt array type index");
    const uint8_t elemKind =
        e->m_currModule->arrayTypes[static_cast<size_t>(ownerTypeIdx)]
            .elemKind;
    //The scalar-array type rows exist for the four mintable kinds; a
    //target of any other kind could never be addressed from NLang code.
    if (elemKind != NEK_Int32 && elemKind != NEK_Int64
        && elemKind != NEK_Float32 && elemKind != NEK_Float64) {
        e->RaiseNlangExceptionBase(
            "newArrayView: target element kind is not a scalar view kind");
    }
    return e->FindScalarArrayTypeIdx(elemKind, rank);
}

//ABI v10 (contract in NativeHost.h): mint a view over an existing
//handle. The element kind comes from the TARGET (a view's type row must
//match its owner's), so the arrayTypes row derives after resolving the
//target family; every further check (rank, dims, type identity,
//coverage) raises inside AllocViewOnHeap's validation — the choke point
//the language's view creation shares.
int32_t VmExecutor::NativeNewArrayView(NativeHost* self,
    int32_t targetHandle, int32_t rank, const int32_t* dims,
    const int32_t* strides, int64_t offset) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    if (rank < 1 || rank > kMaxArrayRank)
        e->RaiseNlangExceptionBase("newArrayView: rank out of range");
    //The view record stores offset in an int32 cell (element units);
    //a 64-bit offset beyond that range could never be addressed.
    if (offset < INT32_MIN || offset > INT32_MAX)
        e->RaiseNlangException(e->m_oobExcClassIdx,
            "newArrayView: offset out of range");
    if (targetHandle <= 0
        || static_cast<size_t>(targetHandle) >= e->m_slotKinds.size())
        e->RaiseNlangException(e->m_nullPtrExcClassIdx,
            "newArrayView: target array is null");
    const uint16_t arrayTypeIdx = ResolveViewOwnerType(e, targetHandle,
        static_cast<uint8_t>(rank));
    const int32_t handle = e->AllocViewOnHeap(arrayTypeIdx,
        static_cast<uint8_t>(rank), dims, strides,
        static_cast<int32_t>(offset), targetHandle);
    //Root across the blocking section (same discipline as newArray).
    e->RootMintedNativeHandle(handle);
    return handle;
}

uint16_t VmExecutor::FindScalarArrayTypeIdx(uint8_t elemRtk,
                                            uint8_t rank) const {
    return FindArrayTypeDef(elemRtk, 0xFFFF, rank);
}

//General row lookup by (elemKind, elemTypeIdx, rank) — the scalar
//finder is the elemTypeIdx==0xFFFF case, struct/string rows carry
//their element type index. A miss means the shape was never declared
//in this module (an invariant break for native minting; for readArray
//a stream naming a shape the reader never declared).
uint16_t VmExecutor::FindArrayTypeDef(uint8_t elemRtk, uint16_t elemTypeIdx,
                                      uint8_t rank) const {
    if (!m_currModule) {
        throw std::runtime_error(
            "NLang VM: no linked module for array-type lookup");
    }
    const auto& table = m_currModule->arrayTypes;
    for (size_t i = 0; i < table.size(); ++i) {
        //Rank filter: without it int[] and int[,] would collapse onto
        //one entry — native minting picks the entry matching its rank.
        if (table[i].elemKind == elemRtk
            && table[i].elemTypeIdx == elemTypeIdx
            && table[i].rank == rank) {
            return static_cast<uint16_t>(i);
        }
    }
    //The unified-library mechanism inlines every stdlib TU, so each
    //tensor signature's array type is registered at compile time. A miss
    //here means a (kind, element type, rank) the program never declared
    //— an invariant break, not a runtime condition.
    throw std::runtime_error(
        "NLang VM: no array type registered for elem kind "
        + std::to_string(static_cast<int>(elemRtk)) + " type idx "
        + std::to_string(static_cast<int>(elemTypeIdx)) + " rank "
        + std::to_string(static_cast<int>(rank)));
}

} // namespace nlang
