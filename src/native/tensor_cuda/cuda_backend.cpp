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
#include <string>
#include <vector>

//The TensorOps contract types live in namespace tensor; the backend's
//slots and table reference them unqualified (the cuda_backend.cpp is
//the CUDA backend's implementation of that contract).
using tensor::TensorOps;
using tensor::UnaryKernelOp;
using tensor::BinaryKernelOp;
using tensor::ReduceAxisOp;
using tensor::DeviceBackendDescriptor;

namespace tensor_cuda {

//--- the hooks (ride the kernels TU's pool/transfer entry points) -----

void* HookAlloc(size_t bytes) {
    return PoolAlloc(bytes);
}

void HookZero(void* ptr, size_t bytes) {
    PoolZero(ptr, bytes);
}

void HookFree(void* ptr) {
    PoolFree(ptr);
}

void HookCopyH2D(void* dst, const void* src, size_t bytes) {
    CopyBytes(dst, src, bytes, true);
}

void HookCopyD2H(void* dst, const void* src, size_t bytes) {
    CopyBytes(dst, src, bytes, false);
}

void HookSync() {
    //The launchers sync per op — the hook's sync rides CopyBytes' sync.
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
//reserved[0] = 1 — the dispatcher's residency routing guaranteed it).
LaunchView ToLaunch(const NativeArrayView& v) {
    LaunchView lv;
    lv.rank = v.rank;
    lv.elemCount = v.elemCount;
    lv.offset = v.offset;
    lv.data = v.data;
    lv.elemKind = v.elemKind;
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

    //M2 (design 2026-11-15 §R6): every slot the P3 kernels do not
    //implement yet carries a named NotSupported raise — a null slot
    //would be a null function-pointer call once the per-call routing
    //directs device operands here. The uniform stub is the spelling.
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
    t.clampI32 = [](NativeHost* host, bool, NativeArrayView&, int32_t,
                    NativeArrayView&) {
        nlang::native::Raise(host, NEXC_Base,
            "tensor: integer clamp is not implemented on the cuda "
            "backend yet"); };
    t.clampI64 = [](NativeHost* host, bool, NativeArrayView&, int64_t,
                    NativeArrayView&) {
        nlang::native::Raise(host, NEXC_Base,
            "tensor: integer clamp is not implemented on the cuda "
            "backend yet"); };
    t.gatherF64 = t.gatherI64 =
        [](NativeHost* host, NativeArrayView&, int32_t,
           NativeArrayView&, NativeArrayView&) {
        nlang::native::Raise(host, NEXC_Base,
            "tensor.gather: f64/int64 are not implemented on the cuda "
            "backend yet"); };

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
        CUDA_CHECK(LaunchClamp(isMax, ToLaunch(src), bound, ToLaunch(dst)),
            "tensor (cuda clamp)");
    };
    t.clampF64 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                    double bound, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchClamp(isMax, ToLaunch(src), bound, ToLaunch(dst)),
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

    t.eqF32 = [](NativeHost* host, NativeArrayView& a,
                 NativeArrayView& b, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchEq(ToLaunch(a), ToLaunch(b), ToLaunch(dst), 1),
            "tensor (cuda eq)");
    };
    t.eqF64 = t.eqF32;
    t.eqI32 = [](NativeHost* host, NativeArrayView& a,
                 NativeArrayView& b, NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchEq(ToLaunch(a), ToLaunch(b), ToLaunch(dst), 0),
            "tensor (cuda eq)");
    };
    t.eqI64 = t.eqI32;

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
        (void)axis;
        RequireCuda(host);
        int32_t errCell = -1;
        CUDA_CHECK(LaunchGather(ToLaunch(src), 0, ToLaunch(indices),
            ToLaunch(dst), &errCell), "tensor (cuda gather)");
        if (errCell >= 0) {
            //The dispatcher stages the indices to host before the slot;
            //the message mirrors the CPU walk's per-index raise.
            const int32_t bad = static_cast<const int32_t*>(
                indices.data)[indices.offset + errCell];
            nlang::native::Raise(host, NEXC_OutOfBounds,
                "tensor.gather: index " + std::to_string(bad)
                + " out of bounds for axis " + std::to_string(axis)
                + " with size " + std::to_string(src.dims[axis]));
        }
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

    //The REAL axis reaches the launcher's geometry gate: rank-2 axis-1
    //raises NotSupported (the slot used to hardcode 0 — silently wrong).
    t.concatF32 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, int32_t axis,
                     NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchConcat(ToLaunch(a), ToLaunch(b), axis,
            ToLaunch(dst), 1), "tensor (cuda concat)");
    };
    t.concatF64 = t.concatF32;
    t.concatI32 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, int32_t axis,
                     NativeArrayView& dst) {
        RequireCuda(host);
        CUDA_CHECK(LaunchConcat(ToLaunch(a), ToLaunch(b), axis,
            ToLaunch(dst), 0), "tensor (cuda concat)");
    };
    t.concatI64 = t.concatI32;

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

__declspec(dllexport) unsigned nlang_device_abi_version(void) {
    return 1u;
}

__declspec(dllexport) const char* nlang_device_backend_name(void) {
    return "cuda";
}

__declspec(dllexport) const tensor::DeviceBackendDescriptor*
nlang_device_backend_init(void) {
    static tensor::DeviceBackendDescriptor desc;
    static bool built = false;
    if (!built) {
        tensor_cuda::BuildOnce();
        int deviceCount = 0;
        if (cudaGetDeviceCount(&deviceCount) != cudaSuccess)
            deviceCount = 0;
        desc.ops = &tensor_cuda::g_table;
        desc.hooks = &tensor_cuda::g_hooks;
        desc.name = "cuda";
        desc.description = "NVIDIA CUDA (sm_86, CUDA 12.6)";
        desc.deviceCount = deviceCount;
        built = true;
    }
    return &desc;
}

} // extern "C"
