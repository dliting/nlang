/*---
VmExecutorNativeCallbacks.cpp — native-host callback implementations
split from VmExecutorNativeHost.cpp when that file reached the
source-size budget: the exception-family mapping of raiseException and
the ABI v6 array side (getArrayView's pinned payload view, newArray's
zero-initialized minting). Table wiring stays in InitNativeHost.
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
    GilEnsureGuard gil(*e);
    return e->m_rng();
}

void VmExecutor::NativeSeedRandom(NativeHost* self, int32_t seed) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    e->m_rng.seed(static_cast<uint32_t>(seed));
}

//Contract (NativeHost.h): the view's pointers are valid for the duration
//of this native call only. The GC is non-moving and the argument record
//is rooted (staged-argument address roots in the blocking protocol, the
//caller's frame in the non-blocking path), so the payload pointer cannot
//stale mid-call. Pointer math uses data()+offset throughout — &v[size]
//would be UB for zero-length records.

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
    if (e->m_slotKinds[static_cast<size_t>(handle)] != RTK_Array) {
        e->RaiseNlangExceptionBase("getArrayView: value is not an array");
    }
    auto& record = e->m_structHeap[static_cast<size_t>(handle)];
    const auto& at = e->m_currModule->arrayTypes[
        static_cast<size_t>(record[1])];
    //Stage-2 header: [2]=rank, [3]=elemCount, [4..]=dims, payload after
    //the padded header. Row-major strides compute into the view's own
    //inline storage (ABI v7): every live view owns its strides — the
    //v6 host scratch let a second view corrupt the first's.
    const int32_t rank = record[2];
    const int64_t elemCount = record[3];
    //Validate before touching caller memory: ArrayPayloadBase throws on a
    //corrupt rank/size. ABI v7 moved the strides buffer into the caller's
    //struct, so the loop below must not run on an unvalidated rank.
    const size_t payloadBase = ArrayPayloadBase(record);
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
    out->data = record.data() + payloadBase;   //payload base
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
    const uint16_t arrayTypeIdx = e->FindScalarArrayTypeIdx(elemKind);
    const int32_t handle = e->AllocArrayOnHeap(arrayTypeIdx, 1, &length);
    //Root across the blocking section: the native holds this handle while
    //filling the payload, and other carriers may GC before it reaches ret.
    e->RootMintedNativeHandle(handle);
    return handle;
}

uint16_t VmExecutor::FindScalarArrayTypeIdx(uint8_t elemRtk) const {
    if (!m_currModule) {
        throw std::runtime_error(
            "NLang VM: no linked module for array-type lookup");
    }
    const auto& table = m_currModule->arrayTypes;
    for (size_t i = 0; i < table.size(); ++i) {
        //rank == 1: native minting is one-dimensional; without the
        //filter int[] and int[,] would collapse onto one entry.
        if (table[i].elemKind == elemRtk
            && table[i].elemTypeIdx == 0xFFFF
            && table[i].rank == 1) {
            return static_cast<uint16_t>(i);
        }
    }
    //The unified-library mechanism inlines every stdlib TU, so each
    //tensor signature's array type is registered at compile time. A miss
    //here means a native minted a kind the program never declared — an
    //invariant break, not a runtime condition.
    throw std::runtime_error(
        "NLang VM: no scalar array type registered for elem kind "
        + std::to_string(static_cast<int>(elemRtk)));
}

} // namespace nlang
