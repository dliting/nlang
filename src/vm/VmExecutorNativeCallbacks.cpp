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

namespace {
//Rank-1 arrays are contiguous: stride is one element everywhere. Host-
//owned read-only storage beats a per-call scratch member.
constexpr int32_t kUnitStrides[1] = {1};
//Allocation guard for natively minted arrays: large enough for every
//legitimate kernel operand, small enough that 3 + n*cells cannot
//overflow int32 (cells <= 2).
constexpr int32_t kMaxNativeArrayLength = 100 * 1000 * 1000;
} // namespace

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
    out->rank = 1;
    out->dims = record.data() + 2;   //the length cell
    out->strides = kUnitStrides;
    out->elemCount = record[2];
    out->elemKind = at.elemKind;
    out->elemCellBytes = static_cast<uint8_t>(
        ArrayElemCells(at.elemKind) * kHeapCellBytes);
    out->reserved[0] = 0;
    out->reserved[1] = 0;
    out->data = record.data() + kArrayHeaderCells;   //payload base
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
    if (length < 0 || length > kMaxNativeArrayLength) {
        e->RaiseNlangException(e->m_oobExcClassIdx,
            "newArray: length out of range");
    }
    const uint16_t arrayTypeIdx = e->FindScalarArrayTypeIdx(elemKind);
    const int32_t handle = e->AllocArrayOnHeap(arrayTypeIdx, length);
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
        if (table[i].elemKind == elemRtk
            && table[i].elemTypeIdx == 0xFFFF) {
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
