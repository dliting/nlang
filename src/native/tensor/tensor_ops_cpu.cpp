/*---
    tensor_ops_cpu.cpp — the CPU backend's TensorOps table + the whole-op
    registry (P1 T1.2 migration in progress).

    The slots are the run-parts lifted verbatim from the dispatcher
    kernels (validation, kind dispatch and output minting stay in the
    dispatcher layer): floats ride the tier-dispatched flat drivers
    (Elementwise.h + KernelOps — the SIMD stack, untouched) with the
    broadcast odometer as the fallback; ints keep the wrap-and-raise
    scalar walks. Unmigrated slots stay nullptr — their dispatchers
    still run the host path directly, so nullptr is never invoked
    mid-migration (order: implement slot, then switch the dispatcher).

    The registry: the cpu table registers at static init;
    ActiveTensorOps() resolves NLANG_TENSOR_BACKEND (unset/unknown →
    cpu). RegisterTensorOps is the hook for out-of-TU backends (the P2
    mirror registration, the cuda plugin).
---*/
#include "TensorOps.h"

#include "Elementwise.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

using nlang::native::Raise;

namespace tensor {

namespace {

//The int-lane arithmetic with the language's wrap semantics — lifted
//from the dispatcher kernels (ApplyBin): MIN/-1 division wraps through
//the unsigned carrier, everything else is plain arithmetic (the callers
//do the per-cell zero-divisor checks their contracts require).
template <typename T>
T ApplyBinWrap(BinaryKernelOp op, T a, T b) {
    switch (op) {
        case BinaryKernelOp::Add: return static_cast<T>(a + b);
        case BinaryKernelOp::Sub: return static_cast<T>(a - b);
        case BinaryKernelOp::Mul: return static_cast<T>(a * b);
        case BinaryKernelOp::Div:
            if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
                if (b == T(-1)) {
                    using U = std::make_unsigned_t<T>;
                    return static_cast<T>(static_cast<U>(0)
                        - static_cast<U>(a));
                }
            }
            return static_cast<T>(a / b);
        case BinaryKernelOp::Rsub: return static_cast<T>(b - a);
        case BinaryKernelOp::Rdiv: return static_cast<T>(b / a);
        case BinaryKernelOp::ReLUGrad:
            return b > T(0) ? a : T(0);
    }
    return T(0);
}

//--- binary array-array (lifted from BinaryKernel) ---
template <typename T>
void CpuBinary(NativeHost* host, BinaryKernelOp op,
               NativeArrayView& va, NativeArrayView& vb,
               NativeArrayView& vout) {
    //Floats: same-shape contiguous pairs ride the tier-dispatched flat
    //kernels; everything else falls back to the broadcast odometer.
    //Ints always take the odometer — their wrap-and-raise semantics
    //stay in the scalar loop (the array arm raises DivByZero when the
    //dividend's divisor cell is reached, mirroring the language rows).
    if constexpr (!std::is_integral_v<T>) {
        if (TryRunBinaryFlat<T>(va, vb, vout, op))
            return;
    }
    int32_t sa[nlang::kMaxArrayRank];
    int32_t sb[nlang::kMaxArrayRank];
    BroadcastStrides(va, vout.rank, sa);
    BroadcastStrides(vb, vout.rank, sb);
    const T* ca = CellsOf<const T>(va);
    const T* cb = CellsOf<const T>(vb);
    T* cout = CellsOf<T>(vout);
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addA = va.offset;
    int64_t addB = vb.offset;
    for (int64_t i = 0; i < vout.elemCount; ++i) {
        if constexpr (std::is_integral_v<T>) {
            if (op == BinaryKernelOp::Div && cb[addB] == T(0))
                Raise(host, NEXC_DivideByZero,
                      "tensor.div: integer division by zero");
        }
        cout[i] = ApplyBinWrap<T>(op, ca[addA], cb[addB]);
        BroadcastAdvance(sa, sb, vout.dims, vout.rank, idx, &addA, &addB);
    }
}

//--- binary scalar arms (lifted from ScalarKernel/ReversedScalarKernel
//run-parts; the scalar==0 pre-checks stay in the dispatcher, before the
//mint) ---
template <typename T>
void CpuBinaryScalar(NativeHost* host, BinaryKernelOp op,
                     NativeArrayView& v, T scalar,
                     NativeArrayView& vout) {
    if constexpr (!std::is_integral_v<T>) {
        RunBinaryScalarFlat<T>(v, vout, op, scalar);
        return;
    }
    const T* cells = CellsOf<const T>(v);
    T* cout = CellsOf<T>(vout);
    int64_t at = 0;
    StrideWalk(v, [&](int64_t addr) {
        cout[at++] = ApplyBinWrap<T>(op, cells[addr], scalar);
    });
    (void)host;
}

//--- unary (lifted from UnaryMathKernel/NegKernel/TanhKernel run-parts)
template <typename T>
void CpuUnary(NativeHost* host, UnaryKernelOp op,
              NativeArrayView& v, NativeArrayView& out) {
    if constexpr (std::is_integral_v<T>) {
        //int neg wraps like language arithmetic in the scalar walk.
        const T* cells = CellsOf<const T>(v);
        T* outCells = CellsOf<T>(out);
        int64_t at = 0;
        StrideWalk(v, [&](int64_t addr) {
            outCells[at++] = static_cast<T>(-cells[addr]);
        });
        (void)op;
        (void)host;
    } else {
        RunUnaryFlat<T>(v, out, op);
        (void)host;
    }
}

//--- the cpu table: families fill in as T1.2 migrates them ---
TensorOps MakeCpuTable() {
    TensorOps t;
    std::memset(&t, 0, sizeof(t));

    //T1.2a elementwise family:
    t.unaryF32 = [](NativeHost* host, UnaryKernelOp op,
                    NativeArrayView& src,
                    NativeArrayView& dst) {
        CpuUnary<float>(host, op, src, dst);
    };
    t.unaryF64 = [](NativeHost* host, UnaryKernelOp op,
                    NativeArrayView& src,
                    NativeArrayView& dst) {
        CpuUnary<double>(host, op, src, dst);
    };
    t.unaryI32 = [](NativeHost* host, UnaryKernelOp op,
                    NativeArrayView& src,
                    NativeArrayView& dst) {
        CpuUnary<int32_t>(host, op, src, dst);
    };
    t.unaryI64 = [](NativeHost* host, UnaryKernelOp op,
                    NativeArrayView& src,
                    NativeArrayView& dst) {
        CpuUnary<int64_t>(host, op, src, dst);
    };
    t.binaryF32 = [](NativeHost* host, BinaryKernelOp op,
                     NativeArrayView& a, NativeArrayView& b,
                     NativeArrayView& dst) {
        CpuBinary<float>(host, op, a, b, dst);
    };
    t.binaryF64 = [](NativeHost* host, BinaryKernelOp op,
                     NativeArrayView& a, NativeArrayView& b,
                     NativeArrayView& dst) {
        CpuBinary<double>(host, op, a, b, dst);
    };
    t.binaryI32 = [](NativeHost* host, BinaryKernelOp op,
                     NativeArrayView& a, NativeArrayView& b,
                     NativeArrayView& dst) {
        CpuBinary<int32_t>(host, op, a, b, dst);
    };
    t.binaryI64 = [](NativeHost* host, BinaryKernelOp op,
                     NativeArrayView& a, NativeArrayView& b,
                     NativeArrayView& dst) {
        CpuBinary<int64_t>(host, op, a, b, dst);
    };
    t.binaryScalarF32 = [](NativeHost* host, BinaryKernelOp op,
                           NativeArrayView& a, float scalar,
                           NativeArrayView& dst) {
        CpuBinaryScalar<float>(host, op, a, scalar, dst);
    };
    t.binaryScalarF64 = [](NativeHost* host, BinaryKernelOp op,
                           NativeArrayView& a, double scalar,
                           NativeArrayView& dst) {
        CpuBinaryScalar<double>(host, op, a, scalar, dst);
    };
    t.binaryScalarI32 = [](NativeHost* host, BinaryKernelOp op,
                           NativeArrayView& a, int32_t scalar,
                           NativeArrayView& dst) {
        CpuBinaryScalar<int32_t>(host, op, a, scalar, dst);
    };
    t.binaryScalarI64 = [](NativeHost* host, BinaryKernelOp op,
                           NativeArrayView& a, int64_t scalar,
                           NativeArrayView& dst) {
        CpuBinaryScalar<int64_t>(host, op, a, scalar, dst);
    };
    //clamp/reductions/matmul/phase2/generation slots migrate in
    //T1.2b-e (still nullptr — their dispatchers run the host path).

    return t;
}

const TensorOps kCpuTable = MakeCpuTable();

//Registered backend tables by name; the cpu table is always present.
struct BackendEntry {
    const char* name;
    const TensorOps* ops;
    BackendEntry* next;
};

BackendEntry* g_backends = nullptr;
const TensorOps* g_active = nullptr;   //resolved once, then cached

void RegisterBuiltinBackends() {
    static BackendEntry cpuEntry = {"cpu", &kCpuTable, nullptr};
    cpuEntry.next = g_backends;
    g_backends = &cpuEntry;
}

} //namespace

void RegisterTensorOps(const char* name, const TensorOps* ops) {
    if (g_backends == nullptr)
        RegisterBuiltinBackends();
    auto* entry = new BackendEntry{name, ops, g_backends};
    g_backends = entry;
    g_active = nullptr;   //re-resolve on next use
}

const TensorOps& ActiveTensorOps() {
    if (g_backends == nullptr)
        RegisterBuiltinBackends();
    if (g_active == nullptr) {
        const char* requested = std::getenv("NLANG_TENSOR_BACKEND");
        const TensorOps* found =
            (requested == nullptr) ? &kCpuTable : nullptr;
        for (BackendEntry* e = g_backends; e != nullptr && found == nullptr;
             e = e->next) {
            if (requested != nullptr && std::strcmp(e->name, requested) == 0)
                found = e->ops;
        }
        //Unknown/unset names resolve to the built-in cpu backend (the
        //KernelOps-era selector contract; device plugins register under
        //their own names and load their tables in T3.8).
        if (found == nullptr)
            found = &kCpuTable;
        g_active = found;
    }
    return *g_active;
}

} // namespace tensor
