/*---
    cuda_backend.cpp — the CUDA device backend (P3): the free-list
    pool, the TensorDeviceHooks (the VM routes device records through
    them — spec D3), the TensorOps CUDA table (each slot: validate,
    launch under the differentiated sync discipline — async by default,
    the error-capable families drain per op — and raise the named NLang
    exception), and the plugin exports (probe/name/init — the
    DeviceBackend contract, spec D5).

    The CUDA runtime is lazy-initialized (spec D4 — backend=cuda runs
    that never touch a device pay no context cost). Errors surface as
    the named catchable NLang exceptions (the CHECK discipline —
    spec §3.1's darknet/PyTorch cross-check).
---*/
#include "../tensor/TensorOps.h"
#include "backend_descriptor.h"
#include "cuda_kernels.h"

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

//The TensorOps contract types live in namespace tensor; the backend's
//slots and table reference them unqualified (the cuda_backend.cpp is
//the CUDA backend's implementation of that contract).
using tensor::TensorOps;
using tensor::UnaryKernelOp;
using tensor::BinaryKernelOp;
using tensor::CompareOp;
using tensor::ReduceAxisOp;
using tensor::DeviceBackendDescriptor;

namespace tensor_cuda {

//--- the hooks (ride the kernels TU's pool/transfer entry points) -----

//The sticky device-fault register — one row per device (M-G2): a
//fault on one device must not blackhole the observation points on
//the others; within a device the CUDA sticky semantics still apply.
//The pool, the transfer and the sync paths record a failure here
//when their caller cannot carry one out (the GC release path's free,
//an execution fault surfacing at a sync whose slot has no error
//parameter). The next status-returning observation point on the same
//device drains it first (cudaGetLastError's contract, mirrored at
//the hook boundary).
TensorDeviceStatus g_stickyStatus[tensor::kNativeMaxDevices] =
    {{DEVICE_OK, 0, nullptr}};
std::mutex g_stickyMutex;

TensorDeviceStatus ParkSticky(int32_t device, TensorDeviceStatus st) {
    std::lock_guard<std::mutex> guard(g_stickyMutex);
    if (g_stickyStatus[device].category == DEVICE_OK)
        g_stickyStatus[device] = st;
    return st;
}

TensorDeviceStatus DrainSticky(int32_t device) {
    std::lock_guard<std::mutex> guard(g_stickyMutex);
    TensorDeviceStatus st = g_stickyStatus[device];
    g_stickyStatus[device] = {DEVICE_OK, 0, nullptr};
    return st;
}

//The pool/transfer primitives' cudaError → status fold (the vendor
//text rides cudaGetErrorString's static buffer). `device` is the
//sticky row the fold consults — the device the primitive ran on.
TensorDeviceStatus StatusOf(int32_t device, cudaError_t err) {
    if (err == cudaSuccess) {
        TensorDeviceStatus drained = DrainSticky(device);
        if (drained.category != DEVICE_OK)
            return drained;
        return {DEVICE_OK, 0, nullptr};
    }
    return {DEVICE_FAULT, 0, cudaGetErrorString(err)};
}

//The hooks carry the payload's device selector (host ABI v16): each
//gates the selector against the plugin's census (CudaDeviceAccepted —
//the same named fault the mirror returns for an unknown device),
//drains that device's sticky row, then pins the thread's context for
//the call (SetDeviceForCall) so the pool/transfer primitives act on
//the named device. The gate comes first everywhere: a plumbing fault
//must name itself, never reach a context the census never blessed —
//and never consume a parked record belonging to some other device's
//row.
TensorDeviceStatus HookAlloc(int32_t device, void** out, size_t bytes) {
    if (!tensor_cuda::CudaDeviceAccepted(device))
        return {DEVICE_FAULT, 0, "cuda: payload on unknown device"};
    TensorDeviceStatus sticky = DrainSticky(device);
    if (sticky.category != DEVICE_OK)
        return sticky;
    //A zero-byte allocation is legitimate (an empty device array's
    //payload): the pointer is never dereferenced, so the null the
    //CUDA runtime returns for cudaMalloc(0) is handed through as ok.
    if (bytes == 0) {
        *out = nullptr;
        return {DEVICE_OK, 0, nullptr};
    }
    SetDeviceForCall pin(device);
    void* ptr = PoolAllocOnStream(bytes);
    if (ptr == nullptr)
        return {DEVICE_OUT_OF_MEMORY, 0,
                "cuda: device allocation failed"};
    *out = ptr;
    return {DEVICE_OK, 0, nullptr};
}

TensorDeviceStatus HookZero(int32_t device, void* ptr, size_t bytes) {
    if (!tensor_cuda::CudaDeviceAccepted(device))
        return {DEVICE_FAULT, 0, "cuda: payload on unknown device"};
    TensorDeviceStatus sticky = DrainSticky(device);
    if (sticky.category != DEVICE_OK)
        return sticky;
    SetDeviceForCall pin(device);
    PoolZero(ptr, bytes);
    return StatusOf(device, cudaGetLastError());
}

TensorDeviceStatus HookFree(int32_t device, void* ptr) {
    if (!tensor_cuda::CudaDeviceAccepted(device))
        return {DEVICE_FAULT, 0, "cuda: payload on unknown device"};
    //The GC release path: a failure parks sticky on the owning device's
    //row (free's caller cannot throw inside the sweep invariants).
    SetDeviceForCall pin(device);
    PoolFreeOnStream(ptr);
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess)
        ParkSticky(device, {DEVICE_FAULT, 0, cudaGetErrorString(err)});
    return {DEVICE_OK, 0, nullptr};
}

TensorDeviceStatus HookCopyH2D(int32_t device, void* dst, const void* src,
                               size_t bytes) {
    if (!tensor_cuda::CudaDeviceAccepted(device))
        return {DEVICE_FAULT, 0, "cuda: payload on unknown device"};
    TensorDeviceStatus sticky = DrainSticky(device);
    if (sticky.category != DEVICE_OK)
        return sticky;
    SetDeviceForCall pin(device);
    return StatusOf(device, CopyBytes(dst, src, bytes, true));
}

TensorDeviceStatus HookCopyD2H(int32_t device, void* dst, const void* src,
                               size_t bytes) {
    if (!tensor_cuda::CudaDeviceAccepted(device))
        return {DEVICE_FAULT, 0, "cuda: payload on unknown device"};
    TensorDeviceStatus sticky = DrainSticky(device);
    if (sticky.category != DEVICE_OK)
        return sticky;
    SetDeviceForCall pin(device);
    return StatusOf(device, CopyBytes(dst, src, bytes, false));
}

//The cross-device copy (device ABI v3's copyD2D): the payload moves
//between two rows, so both ids gate; a sticky fault on either row
//involves this copy's hardware and drains first. New faults attribute
//to the destination's row (the caller's handle lands there — the
//status the registry folds names the device the caller holds).
TensorDeviceStatus HookCopyD2D(int32_t dstDevice, void* dst,
                               int32_t srcDevice, const void* src,
                               size_t bytes) {
    if (!tensor_cuda::CudaDeviceAccepted(dstDevice)
        || !tensor_cuda::CudaDeviceAccepted(srcDevice))
        return {DEVICE_FAULT, 0, "cuda: payload on unknown device"};
    TensorDeviceStatus sticky = DrainSticky(srcDevice);
    if (sticky.category != DEVICE_OK)
        return sticky;
    sticky = DrainSticky(dstDevice);
    if (sticky.category != DEVICE_OK)
        return sticky;
    const bool peer = CudaPeerReady(srcDevice, dstDevice);
    return StatusOf(dstDevice,
        peer ? CopyPeer(dstDevice, dst, srcDevice, src, bytes)
             : CopyStaged(dstDevice, dst, srcDevice, src, bytes));
}

TensorDeviceStatus HookSync() {
    //The stream sync: the authoritative observation point for the
    //async group's execution faults. It syncs device 0's main stream
    //(the VM never routes here under operand routing — each compute
    //op observes its own row at its own boundary — this hook keeps
    //the pre-M-G3 whole-device spelling, pinned explicitly now that
    //the stream reads are selection-dependent).
    TensorDeviceStatus sticky = DrainSticky(0);
    if (sticky.category != DEVICE_OK)
        return sticky;
    SetDeviceForCall pin(0);
    return StatusOf(0, SyncStream());
}

TensorDeviceHooks MakeHooks() {
    TensorDeviceHooks h;
    h.alloc = &HookAlloc;
    h.zero = &HookZero;
    h.free = &HookFree;
    h.copyH2D = &HookCopyH2D;
    h.copyD2H = &HookCopyD2H;
    h.sync = &HookSync;
    return h;
}

//--- the CHECK-style raise (the darknet/PyTorch discipline) -------------

//The last CUDA error the CHECK macro saw (per-slot raise path).
cudaError_t LastCudaError = cudaSuccess;

#define CUDA_CHECK(call, who)                                              \
    do {                                                                   \
        LastCudaError = (call);                                            \
        if (LastCudaError != cudaSuccess)                                  \
            nlang::native::Raise(host, NEXC_Base,                          \
                std::string(who) + ": "                                    \
                + cudaGetErrorName(LastCudaError) + " ("                   \
                + cudaGetErrorString(LastCudaError) + ")");                \
    } while (0)

//The LaunchView builder from a pinned view (data = the device pointer,
//reserved[0] = 1 — the dispatcher's residency routing guaranteed it;
//reserved[1] = the owning device, folded into the launcher's pin —
//device ABI v3's operand-driven routing).
LaunchView ToLaunch(const NativeArrayView& v) {
    LaunchView lv;
    lv.rank = v.rank;
    lv.elemCount = v.elemCount;
    lv.offset = v.offset;
    lv.data = v.data;
    lv.elemKind = v.elemKind;
    lv.device = v.reserved[1];
    for (int32_t i = 0; i < v.rank && i < kMaxRank; ++i) {
        lv.dims[i] = v.dims[i];
        lv.strides[i] = v.strides[i];
    }
    return lv;
}

//--- the TensorOps CUDA table -------------------------------------------
//Each slot: RequireCuda (the lazy context) → launch → raise on errors.
//The op codes ride the shared enums; the launcher-level errors surface
//as the named NLang exceptions.

namespace {

void RequireCuda(NativeHost* host) {
    if (CudaReady())
        return;
    std::string error;
    if (!CudaLazyInit(&error))
        nlang::native::Raise(host, NEXC_Base, "tensor (cuda): " + error);
}

} // namespace

TensorOps MakeCudaTable() {
    TensorOps t;
    std::memset(&t, 0, sizeof(t));

    //Design 2026-11-15 §R6: an unimplemented slot must carry a named
    //raise, never a null pointer (a null slot is a null function-
    //pointer call once per-call routing directs device operands here).
    //The table is now fully populated.
    //extreme (full reduction): the launcher branches on IsDense — a dense
    //view takes the flat dense kernel (offset folded into the base), a
    //strided view takes the strided-aware kernel (FlatAddress per
    //element). The dispatcher does not materialize; the slot passes the
    //real view through.
    t.extremeF32 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                      float* out) {
        RequireCuda(host);
        double f = 0.0;
        CUDA_CHECK(LaunchExtreme(isMax, ToLaunch(src), &f, nullptr,
            false), "tensor (cuda extreme)");
        *out = (float)f;
    };
    t.extremeF64 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                      double* out) {
        RequireCuda(host);
        CUDA_CHECK(LaunchExtreme(isMax, ToLaunch(src), out, nullptr,
            false), "tensor (cuda extreme)");
    };
    t.extremeI32 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                      int32_t* out) {
        RequireCuda(host);
        int64_t i = 0;
        CUDA_CHECK(LaunchExtreme(isMax, ToLaunch(src), nullptr, &i,
            true), "tensor (cuda extreme)");
        *out = (int32_t)i;
    };
    t.extremeI64 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                      int64_t* out) {
        RequireCuda(host);
        CUDA_CHECK(LaunchExtreme(isMax, ToLaunch(src), nullptr, out,
            true), "tensor (cuda extreme)");
    };
    //mean: reuse the device sum; the CPU's double accumulation and double
    //return match it (int kinds never wrap the sum) — bit-identical whenever
    //the sum and every partial are exactly representable (|sum| < 2^53; int32
    //inputs always satisfy it, int64 inputs need no wrap, |sum| < 2^63).
    t.meanF32 = t.meanF64 =
        [](NativeHost* host, NativeArrayView& src, double* out) {
        RequireCuda(host);
        double s = 0.0;
        CUDA_CHECK(LaunchSumAll(ToLaunch(src), &s, nullptr, false),
            "tensor (cuda mean)");
        *out = s / static_cast<double>(src.elemCount);
    };
    t.meanI32 = t.meanI64 =
        [](NativeHost* host, NativeArrayView& src, double* out) {
        RequireCuda(host);
        int64_t s = 0;
        CUDA_CHECK(LaunchSumAll(ToLaunch(src), nullptr, &s, true),
            "tensor (cuda mean)");
        *out = static_cast<double>(s) / static_cast<double>(src.elemCount);
    };
    t.matmulF64 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchMatmul(ToLaunch(a), ToLaunch(b),
            ToLaunch(dst), a.elemKind), "tensor (cuda matmul)");
    };
    t.matmulI32 = t.matmulI64 =
        [](NativeHost* host, NativeArrayView& a, NativeArrayView& b,
           NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchMatmul(ToLaunch(a), ToLaunch(b),
            ToLaunch(dst), a.elemKind), "tensor (cuda matmul)");
    };
    t.clampI32 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                    int32_t bound, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchClamp(isMax, ToLaunch(src), 0.0, bound,
            ToLaunch(dst)), "tensor (cuda clamp)");
    };
    t.clampI64 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                    int64_t bound, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchClamp(isMax, ToLaunch(src), 0.0, bound,
            ToLaunch(dst)), "tensor (cuda clamp)");
    };
    t.gatherF64 = t.gatherF32;
    t.gatherI64 = t.gatherF32;

    t.sortF32 = [](NativeHost* host, NativeArrayView& a, int32_t axis,
                   NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchSort(false, ToLaunch(a), axis, ToLaunch(dst)),
            "tensor (cuda sort)");
    };
    t.sortF64 = t.sortF32;
    t.sortI32 = t.sortF32;
    t.sortI64 = t.sortF32;
    t.argsortF32 = [](NativeHost* host, NativeArrayView& a,
                      int32_t axis, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchSort(true, ToLaunch(a), axis, ToLaunch(dst)),
            "tensor (cuda argsort)");
    };
    t.argsortF64 = t.argsortF32;
    t.argsortI32 = t.argsortF32;
    t.argsortI64 = t.argsortF32;

    t.takeAlongF32 = [](NativeHost* host, NativeArrayView& a,
                        int32_t axis, const int32_t* indices,
                        NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchTakeAlong(ToLaunch(a), axis, indices,
            ToLaunch(dst)), "tensor (cuda takeAlongAxis)");
    };
    t.takeAlongF64 = t.takeAlongF32;
    t.takeAlongI32 = t.takeAlongF32;
    t.takeAlongI64 = t.takeAlongF32;

    t.assignIntoF32 = [](NativeHost* host, NativeArrayView& view,
                         NativeArrayView& values) {
        RequireCuda(host);
        //The values stage host-side (the dispatcher's metadata pattern)
        //— a dense read rides the H2D scratch inside the launcher.
        CUDA_CHECK(LaunchAssignInto(ToLaunch(view),
            static_cast<const uint8_t*>(values.data)
                + static_cast<size_t>(values.offset)
                    * values.elemCellBytes,
            view.elemKind), "tensor (cuda assignInto)");
    };
    t.assignIntoF64 = t.assignIntoF32;
    t.assignIntoI32 = t.assignIntoF32;
    t.assignIntoI64 = t.assignIntoF32;

    t.assignIntoSF32 = [](NativeHost* host, NativeArrayView& view,
                          double sv) {
        RequireCuda(host);
        CUDA_CHECK(LaunchAssignIntoScalar(ToLaunch(view), sv, 0, 1),
            "tensor (cuda assignIntoS)");
    };
    t.assignIntoSF64 = [](NativeHost* host, NativeArrayView& view,
                          double sv) {
        RequireCuda(host);
        CUDA_CHECK(LaunchAssignIntoScalar(ToLaunch(view), sv, 0, 17),
            "tensor (cuda assignIntoS)");
    };
    t.assignIntoSI32 = [](NativeHost* host, NativeArrayView& view,
                          int64_t sv) {
        RequireCuda(host);
        CUDA_CHECK(LaunchAssignIntoScalar(ToLaunch(view), 0.0, sv, 0),
            "tensor (cuda assignIntoS)");
    };
    t.assignIntoSI64 = [](NativeHost* host, NativeArrayView& view,
                          int64_t sv) {
        RequireCuda(host);
        CUDA_CHECK(LaunchAssignIntoScalar(ToLaunch(view), 0.0, sv, 15),
            "tensor (cuda assignIntoS)");
    };

    t.unaryF32 = [](NativeHost* host, UnaryKernelOp op,
                    NativeArrayView& src, NativeArrayView& dst) {
        RequireCuda(host);
        const int code = op == UnaryKernelOp::Neg ? 12
            : op == UnaryKernelOp::Exp ? 13
            : op == UnaryKernelOp::Log ? 14 : 15;
        CUDA_CHECK(LaunchUnary(code, ToLaunch(src), ToLaunch(dst)),
            "tensor (cuda unary)");
    };
    t.unaryF64 = t.unaryF32;
    t.unaryI32 = [](NativeHost* host, UnaryKernelOp op,
                    NativeArrayView& src, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchUnary(12, ToLaunch(src), ToLaunch(dst)),
            "tensor (cuda neg)");
    };
    t.unaryI64 = t.unaryI32;

    t.binaryF32 = [](NativeHost* host, BinaryKernelOp op,
                     NativeArrayView& a, NativeArrayView& b,
                     NativeArrayView& dst) {
        RequireCuda(host);
        int32_t errCell = -1;
        CUDA_CHECK(LaunchBinary((int)op, ToLaunch(a), ToLaunch(b),
            ToLaunch(dst), &errCell), "tensor (cuda binary)");
        if (errCell >= 0)
            nlang::native::Raise(host, NEXC_DivideByZero,
                "tensor.div: integer division by zero");
    };
    t.binaryF64 = t.binaryF32;
    t.binaryI32 = t.binaryF32;
    t.binaryI64 = t.binaryF32;

    t.binaryScalarF32 = [](NativeHost* host, BinaryKernelOp op,
                           NativeArrayView& a, float scalar,
                           NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchBinaryScalar((int)op, ToLaunch(a), scalar,
            op == BinaryKernelOp::Rsub || op == BinaryKernelOp::Rdiv,
            ToLaunch(dst), nullptr), "tensor (cuda scalar)");
    };
    t.binaryScalarF64 = [](NativeHost* host, BinaryKernelOp op,
                           NativeArrayView& a, double scalar,
                           NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchBinaryScalar((int)op, ToLaunch(a), scalar,
            op == BinaryKernelOp::Rsub || op == BinaryKernelOp::Rdiv,
            ToLaunch(dst), nullptr), "tensor (cuda scalar)");
    };
    t.binaryScalarI32 = [](NativeHost* host, BinaryKernelOp op,
                           NativeArrayView& a, int32_t scalar,
                           NativeArrayView& dst) {
        RequireCuda(host);
        int32_t errCell = -1;
        CUDA_CHECK(LaunchBinaryScalar((int)op, ToLaunch(a), scalar,
            op == BinaryKernelOp::Rsub || op == BinaryKernelOp::Rdiv,
            ToLaunch(dst), &errCell), "tensor (cuda scalar)");
        if (errCell >= 0)
            nlang::native::Raise(host, NEXC_DivideByZero,
                "tensor.div: integer division by zero");
    };
    t.binaryScalarI64 = [](NativeHost* host, BinaryKernelOp op,
                           NativeArrayView& a, int64_t scalar,
                           NativeArrayView& dst) {
        RequireCuda(host);
        int32_t errCell = -1;
        CUDA_CHECK(LaunchBinaryScalar((int)op, ToLaunch(a), scalar,
            op == BinaryKernelOp::Rsub || op == BinaryKernelOp::Rdiv,
            ToLaunch(dst), &errCell), "tensor (cuda scalar)");
        if (errCell >= 0)
            nlang::native::Raise(host, NEXC_DivideByZero,
                "tensor.div: integer division by zero");
    };

    t.clampF32 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                    float bound, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchClamp(isMax, ToLaunch(src), bound, 0, ToLaunch(dst)),
            "tensor (cuda clamp)");
    };
    t.clampF64 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                    double bound, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchClamp(isMax, ToLaunch(src), bound, 0, ToLaunch(dst)),
            "tensor (cuda clamp)");
    };

    t.sumAllF32 = [](NativeHost* host, NativeArrayView& src,
                     double* out) {
        RequireCuda(host);
        CUDA_CHECK(LaunchSumAll(ToLaunch(src), out, nullptr, false),
            "tensor (cuda sum)");
    };
    t.sumAllF64 = t.sumAllF32;
    t.sumAllI32 = [](NativeHost* host, NativeArrayView& src,
                     int64_t* out) {
        RequireCuda(host);
        CUDA_CHECK(LaunchSumAll(ToLaunch(src), nullptr, out, true),
            "tensor (cuda sum)");
    };
    t.sumAllI64 = t.sumAllI32;

    t.dotF32 = [](NativeHost* host, NativeArrayView& a,
                  NativeArrayView& b, float* out) {
        RequireCuda(host);
        double acc = 0.0;
        CUDA_CHECK(LaunchDot(ToLaunch(a), ToLaunch(b), &acc,
            nullptr, false), "tensor (cuda dot)");
        *out = (float)acc;
    };
    t.dotF64 = [](NativeHost* host, NativeArrayView& a,
                  NativeArrayView& b, double* out) {
        RequireCuda(host);
        CUDA_CHECK(LaunchDot(ToLaunch(a), ToLaunch(b), out,
            nullptr, false), "tensor (cuda dot)");
    };
    t.dotI32 = [](NativeHost* host, NativeArrayView& a,
                  NativeArrayView& b, int64_t* out) {
        RequireCuda(host);
        CUDA_CHECK(LaunchDot(ToLaunch(a), ToLaunch(b), nullptr,
            out, true), "tensor (cuda dot)");
    };
    t.dotI64 = t.dotI32;

    t.compareF32 = [](NativeHost* host, CompareOp op, NativeArrayView& a,
                      NativeArrayView& b, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchCompare(op, ToLaunch(a), ToLaunch(b),
            ToLaunch(dst)), "tensor (cuda compare)");
    };
    t.compareF64 = t.compareF32;
    t.compareI32 = t.compareF32;
    t.compareI64 = t.compareF32;
    //The scalar arm lands with the device compare kernel (v0.14.2 Task
    //3); named raises keep the slots callable.
    t.compareScalarF32 = [](NativeHost* host, CompareOp op,
                            NativeArrayView& a, double scalar,
                            NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchCompareScalar(op, ToLaunch(a), scalar, 0,
            ToLaunch(dst)), "tensor (cuda compare scalar)");
    };
    t.compareScalarF64 = t.compareScalarF32;
    t.compareScalarI32 = [](NativeHost* host, CompareOp op,
                            NativeArrayView& a, int64_t scalar,
                            NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchCompareScalar(op, ToLaunch(a), 0.0, scalar,
            ToLaunch(dst)), "tensor (cuda compare scalar)");
    };
    t.compareScalarI64 = t.compareScalarI32;
    t.whereF32 = [](NativeHost* host, NativeArrayView& mask,
                    NativeArrayView& x, NativeArrayView& y,
                    NativeArrayView& dst, bool maskLong, bool xScalar,
                    bool yScalar, double sx, double sy) {
        RequireCuda(host);
        CUDA_CHECK(LaunchWhere(ToLaunch(mask), ToLaunch(x), ToLaunch(y),
            ToLaunch(dst), 1, maskLong, xScalar, yScalar, sx, sy, 0, 0),
            "tensor (cuda where)");
    };
    t.whereF64 = [](NativeHost* host, NativeArrayView& mask,
                    NativeArrayView& x, NativeArrayView& y,
                    NativeArrayView& dst, bool maskLong, bool xScalar,
                    bool yScalar, double sx, double sy) {
        RequireCuda(host);
        CUDA_CHECK(LaunchWhere(ToLaunch(mask), ToLaunch(x), ToLaunch(y),
            ToLaunch(dst), 17, maskLong, xScalar, yScalar, sx, sy, 0, 0),
            "tensor (cuda where)");
    };
    t.whereI32 = [](NativeHost* host, NativeArrayView& mask,
                    NativeArrayView& x, NativeArrayView& y,
                    NativeArrayView& dst, bool maskLong, bool xScalar,
                    bool yScalar, int64_t isx, int64_t isy) {
        RequireCuda(host);
        CUDA_CHECK(LaunchWhere(ToLaunch(mask), ToLaunch(x), ToLaunch(y),
            ToLaunch(dst), 0, maskLong, xScalar, yScalar, 0.0, 0.0, isx,
            isy), "tensor (cuda where)");
    };
    t.whereI64 = [](NativeHost* host, NativeArrayView& mask,
                    NativeArrayView& x, NativeArrayView& y,
                    NativeArrayView& dst, bool maskLong, bool xScalar,
                    bool yScalar, int64_t isx, int64_t isy) {
        RequireCuda(host);
        CUDA_CHECK(LaunchWhere(ToLaunch(mask), ToLaunch(x), ToLaunch(y),
            ToLaunch(dst), 15, maskLong, xScalar, yScalar, 0.0, 0.0, isx,
            isy), "tensor (cuda where)");
    };

    t.scatterF32 = [](NativeHost* host, NativeArrayView& a,
                      int32_t axis, NativeArrayView& indices,
                      NativeArrayView& values, NativeArrayView& dst,
                      bool valuesScalar, double sv, int32_t* errorOut) {
        (void)axis;
        RequireCuda(host);
        int32_t errCell = -1;
        CUDA_CHECK(LaunchScatter(ToLaunch(a), ToLaunch(indices),
            ToLaunch(values), ToLaunch(dst), valuesScalar, sv, 0,
            &errCell), "tensor (cuda scatter)");
        if (errCell >= 0) {
            //The dispatcher stages the indices to host; the message
            //mirrors the CPU walk's per-index raise.
            const int32_t bad = static_cast<const int32_t*>(
                indices.data)[indices.offset + errCell];
            nlang::native::Raise(host, NEXC_OutOfBounds,
                "tensor.scatter: index " + std::to_string(bad)
                + " out of bounds for axis 0 with size "
                + std::to_string(a.dims[0]));
        }
    };
    t.scatterF64 = t.scatterF32;
    t.scatterI32 = [](NativeHost* host, NativeArrayView& a,
                      int32_t axis, NativeArrayView& indices,
                      NativeArrayView& values, NativeArrayView& dst,
                      bool valuesScalar, int64_t isv, int32_t* errorOut) {
        (void)axis;
        (void)errorOut;
        RequireCuda(host);
        int32_t errCell = -1;
        CUDA_CHECK(LaunchScatter(ToLaunch(a), ToLaunch(indices),
            ToLaunch(values), ToLaunch(dst), valuesScalar, 0.0, isv,
            &errCell), "tensor (cuda scatter)");
        if (errCell >= 0) {
            const int32_t bad = static_cast<const int32_t*>(
                indices.data)[indices.offset + errCell];
            nlang::native::Raise(host, NEXC_OutOfBounds,
                "tensor.scatter: index " + std::to_string(bad)
                + " out of bounds for axis 0 with size "
                + std::to_string(a.dims[0]));
        }
    };
    t.scatterI64 = t.scatterI32;

    t.scatterAddF32 = [](NativeHost* host, NativeArrayView& a,
                         int32_t axis, NativeArrayView& indices,
                         NativeArrayView& values, NativeArrayView& dst,
                         bool valuesScalar, double sv,
                         int32_t* errorOut) {
        (void)axis;
        (void)errorOut;
        RequireCuda(host);
        int32_t errCell = -1;
        CUDA_CHECK(LaunchScatterAdd(ToLaunch(a), ToLaunch(indices),
            ToLaunch(values), ToLaunch(dst), valuesScalar, sv, 0,
            &errCell), "tensor (cuda scatterAdd)");
        if (errCell >= 0) {
            //The dispatcher stages the indices to host; the message
            //mirrors the CPU walk's per-index raise.
            const int32_t bad = static_cast<const int32_t*>(
                indices.data)[indices.offset + errCell];
            nlang::native::Raise(host, NEXC_OutOfBounds,
                "tensor.scatterAdd: index " + std::to_string(bad)
                + " out of bounds for axis 0 with size "
                + std::to_string(a.dims[0]));
        }
    };
    t.scatterAddF64 = t.scatterAddF32;
    t.scatterAddI32 = [](NativeHost* host, NativeArrayView& a,
                         int32_t axis, NativeArrayView& indices,
                         NativeArrayView& values, NativeArrayView& dst,
                         bool valuesScalar, int64_t isv,
                         int32_t* errorOut) {
        (void)axis;
        (void)errorOut;
        RequireCuda(host);
        int32_t errCell = -1;
        CUDA_CHECK(LaunchScatterAdd(ToLaunch(a), ToLaunch(indices),
            ToLaunch(values), ToLaunch(dst), valuesScalar, 0.0, isv,
            &errCell), "tensor (cuda scatterAdd)");
        if (errCell >= 0) {
            const int32_t bad = static_cast<const int32_t*>(
                indices.data)[indices.offset + errCell];
            nlang::native::Raise(host, NEXC_OutOfBounds,
                "tensor.scatterAdd: index " + std::to_string(bad)
                + " out of bounds for axis 0 with size "
                + std::to_string(a.dims[0]));
        }
    };
    t.scatterAddI64 = t.scatterAddI32;

    t.oneHotI32 = [](NativeHost* host, NativeArrayView& labels,
                     int32_t n, NativeArrayView& dst) {
        RequireCuda(host);
        int32_t errCell = -1;
        CUDA_CHECK(LaunchOneHot(ToLaunch(labels), n, ToLaunch(dst),
            &errCell), "tensor (cuda oneHot)");
        if (errCell >= 0) {
            //The dispatcher stages the labels to host before the slot
            //(the kernel reads them from host memory), so the offending
            //value is readable here — the message mirrors the CPU walk.
            const int32_t bad = static_cast<const int32_t*>(
                labels.data)[labels.offset + errCell];
            nlang::native::Raise(host, NEXC_OutOfBounds,
                "tensor.oneHot: label " + std::to_string(bad)
                + " out of range for " + std::to_string(n) + " classes");
        }
    };

    t.matmulF32 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchMatmul(ToLaunch(a), ToLaunch(b),
            ToLaunch(dst), a.elemKind), "tensor (cuda matmul)");
    };

    t.gatherF32 = [](NativeHost* host, NativeArrayView& src, int32_t axis,
                     NativeArrayView& indices, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchGather(ToLaunch(src), axis, ToLaunch(indices),
            ToLaunch(dst)), "tensor (cuda gather)");
    };
    t.gatherI32 = t.gatherF32;

    t.fillF32 = [](NativeHost* host, NativeArrayView& dst, double value) {
        RequireCuda(host);
        CUDA_CHECK(LaunchFill(ToLaunch(dst), value, 0, 1),
            "tensor (cuda fill)");
    };
    t.fillF64 = [](NativeHost* host, NativeArrayView& dst, double value) {
        RequireCuda(host);
        CUDA_CHECK(LaunchFill(ToLaunch(dst), value, 0, 17),
            "tensor (cuda fill)");
    };
    t.fillI32 = [](NativeHost* host, NativeArrayView& dst, int64_t v) {
        RequireCuda(host);
        CUDA_CHECK(LaunchFill(ToLaunch(dst), 0, v, 0),
            "tensor (cuda fill)");
    };
    t.fillI64 = [](NativeHost* host, NativeArrayView& dst, int64_t v) {
        RequireCuda(host);
        CUDA_CHECK(LaunchFill(ToLaunch(dst), 0, v, 15),
            "tensor (cuda fill)");
    };

    t.arangeI32 = [](NativeHost* host, NativeArrayView& dst,
                     int64_t start, int64_t step) {
        RequireCuda(host);
        CUDA_CHECK(LaunchArange(ToLaunch(dst), (double)start,
            (double)step, true), "tensor (cuda arange)");
    };
    t.arangeF32 = [](NativeHost* host, NativeArrayView& dst,
                     double start, double step) {
        RequireCuda(host);
        CUDA_CHECK(LaunchArange(ToLaunch(dst), start, step, false),
            "tensor (cuda arange)");
    };
    t.arangeI64 = t.arangeI32;
    t.arangeF64 = t.arangeF32;

    t.copyF32 = [](NativeHost* host, NativeArrayView& src,
                   NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchCopy(ToLaunch(src), ToLaunch(dst), 1),
            "tensor (cuda copy)");
    };
    t.copyF64 = [](NativeHost* host, NativeArrayView& src,
                   NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchCopy(ToLaunch(src), ToLaunch(dst), 17),
            "tensor (cuda copy)");
    };
    t.copyI32 = [](NativeHost* host, NativeArrayView& src,
                   NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchCopy(ToLaunch(src), ToLaunch(dst), 0),
            "tensor (cuda copy)");
    };
    t.copyI64 = [](NativeHost* host, NativeArrayView& src,
                   NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchCopy(ToLaunch(src), ToLaunch(dst), 15),
            "tensor (cuda copy)");
    };

    //The REAL axis reaches the launcher (the join kernel folds any
    //axis; the slot used to hardcode 0 — silently wrong, and the
    //launcher's axis-0-only gate then raised where the host path had
    //results). One body for all four kinds, the cell width riding the
    //operand view (a.elemKind — the matmul/assignInto convention): the
    //old per-family hardcoding handed the f64/i64 slots the f32/i32
    //codes, copying and reading half the cells' bytes (silent, zeros
    //at the tail).
    t.concatF32 = t.concatF64 = t.concatI32 = t.concatI64 =
        [](NativeHost* host, NativeArrayView& a, NativeArrayView& b,
           int32_t axis, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchConcat(ToLaunch(a), ToLaunch(b), axis,
            ToLaunch(dst), a.elemKind), "tensor (cuda concat)");
    };

    t.reduceAxisF32 = [](NativeHost* host, ReduceAxisOp op,
                         NativeArrayView& src, int32_t axis,
                         NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchReduceAxis((int)op, ToLaunch(src), axis,
            ToLaunch(dst)), "tensor (cuda reduce)");
    };
    t.reduceAxisF64 = t.reduceAxisF32;
    t.reduceAxisI32 = t.reduceAxisF32;
    t.reduceAxisI64 = t.reduceAxisF32;

    t.argmaxI32 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                     int32_t axis, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchArgExtreme(isMax, ToLaunch(src), axis,
            ToLaunch(dst)), "tensor (cuda argmax)");
    };

    t.uniformF32 = [](NativeHost* host, NativeArrayView& dst, float low,
                      float high, const uint32_t* mt624,
                      uint32_t position) {
        (void)host; (void)dst; (void)low; (void)high; (void)mt624;
        (void)position;
        nlang::native::Raise(host, NEXC_Base,
            "tensor.uniform: the cuda snapshot kernel lands with the "
            "parity suite");
    };
    t.uniformF64 = [](NativeHost* host, NativeArrayView& dst,
                      double low, double high, const uint32_t* mt624,
                      uint32_t position) {
        (void)host; (void)dst; (void)low; (void)high; (void)mt624;
        (void)position;
        nlang::native::Raise(host, NEXC_Base,
            "tensor.uniform: the cuda snapshot kernel lands with the "
            "parity suite");
    };

    return t;
}

//The table instance (BuildOnce copies it into the plugin's static).

//--- the plugin exports (the DeviceBackend contract) --------------------

namespace {

TensorOps g_table;
TensorDeviceHooks g_hooks;
bool g_built = false;

void BuildOnce() {
    if (g_built)
        return;
    g_table = MakeCudaTable();
    g_hooks = MakeHooks();
    g_built = true;
}

} // namespace

} // namespace tensor_cuda

extern "C" {

NLANG_DEVICE_EXPORT unsigned nlang_device_abi_version(void) {
    //The descriptor header owns the number (the registry's probe compares
    //against the same constant — they can never drift).
    return tensor::NLANG_DEVICE_ABI_VERSION;
}

NLANG_DEVICE_EXPORT const char* nlang_device_backend_name(void) {
    return "cuda";
}

NLANG_DEVICE_EXPORT const tensor::DeviceBackendDescriptor*
nlang_device_backend_init(void) {
    static tensor::DeviceBackendDescriptor desc;
    static bool built = false;
    if (!built) {
        tensor_cuda::BuildOnce();
        int deviceCount = 0;
        if (cudaGetDeviceCount(&deviceCount) != cudaSuccess)
            deviceCount = 0;
        //Clamp with the descriptor's shared census bound — the same
        //number the state tables are sized by — so the exported census
        //can never name a device the plugin cannot serve.
        desc.deviceCount = deviceCount < tensor::kNativeMaxDevices
            ? deviceCount : tensor::kNativeMaxDevices;
        desc.ops = &tensor_cuda::g_table;
        desc.hooks = &tensor_cuda::g_hooks;
        desc.name = "cuda";
        desc.description = "NVIDIA CUDA (sm_86, CUDA 12.6)";
        desc.streamCreate = &tensor_cuda::CudaStreamCreate;
        desc.streamDestroy = &tensor_cuda::CudaStreamDestroy;
        desc.streamPush = &tensor_cuda::CudaStreamPush;
        desc.streamPop = &tensor_cuda::CudaStreamPop;
        desc.streamJoin = &tensor_cuda::CudaStreamJoin;
        desc.copyD2D = &tensor_cuda::HookCopyD2D;
        built = true;
    }
    return &desc;
}

} // extern "C"
