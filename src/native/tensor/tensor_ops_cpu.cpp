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

//--- axis reductions (lifted from SumAxisKernel/ExtremeAxisKernel
//run-parts; the dispatcher keeps the axis/rank-1/empty validations and
//the rank-1 scalar-return arm). Sum: flat fast paths first (axis =
//rank-1 reduces contiguous segments; axis = 0 accumulates rows through
//the double addInto), then the outer-odometer walk with the Acc
//carrier. Max/Min: flat extremes for contiguous last-axis sources,
//then the per-cell carrier compare of the walk (first-seen wins ties).
//ArgMax/ArgMin ride the argmaxI32 slot instead (asserted defensive).
template <typename T>
void ReduceAxisImpl(NativeHost* host, ReduceAxisOp op, NativeArrayView& v,
                    int32_t axis, NativeArrayView& out) {
    const T* cells = CellsOf<const T>(v);
    const int32_t along = v.dims[axis];
    const int64_t axisStride = v.strides[axis];
    const int32_t outRank = v.rank - 1;
    int32_t outDims[nlang::kMaxArrayRank];
    AxisRemovedDims(v, axis, outDims);
    T* outCells = CellsOf<T>(out);
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t base = v.offset;
    if (op == ReduceAxisOp::Sum) {
        if (SumAxisFlatFastPath<T>(v, out, axis, along))
            return;
        using Acc = std::conditional_t<std::is_integral_v<T>, int64_t,
                                       double>;
        for (int64_t i = 0; i < out.elemCount; ++i) {
            Acc acc = 0;
            for (int64_t s = 0; s < along; ++s)
                acc = static_cast<Acc>(acc
                    + cells[base + s * axisStride]);
            outCells[i] = static_cast<T>(acc);
            AxisRemovedAdvance(v, axis, outRank, outDims, idx, &base);
        }
        return;
    }
    if (op == ReduceAxisOp::ArgMax || op == ReduceAxisOp::ArgMin) {
        //The index reductions ride the argmaxI32 slot (the gather
        //family owns them) — defensive only.
        Raise(host, NEXC_Base,
              "tensor: index reductions route through argmaxI32");
        return;
    }
    const bool isMax = op == ReduceAxisOp::Max;
    if (ExtremeAxisFlatFastPath<T>(v, out, axis, along, isMax))
        return;
    for (int64_t i = 0; i < out.elemCount; ++i) {
        T best = cells[base];
        for (int32_t s = 1; s < along; ++s) {
            const T cur = cells[base + s * axisStride];
            if (isMax ? cur > best : cur < best) {
                best = cur;
            }
        }
        outCells[i] = best;
        AxisRemovedAdvance(v, axis, outRank, outDims, idx, &base);
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
    //--- T1.2b reduction family ---
    //sumAll (lifted from UnaryReduceKernel's Sum arm): floats take the
    //flat double-accumulator sum on contiguous spans (order-free for
    //the exact sums the suite pins); the walk keeps the historical
    //accumulation — double for floats, the wrap carrier for ints.
    t.sumAllF32 = [](NativeHost* host, NativeArrayView& src, double* out) {
        (void)host;
        if (TrySumFlat<float>(src, out))
            return;
        double acc = 0.0;
        const float* cells = CellsOf<const float>(src);
        StrideWalk(src, [&](int64_t addr) {
            acc = acc + cells[addr];
        });
        *out = acc;
    };
    t.sumAllF64 = [](NativeHost* host, NativeArrayView& src, double* out) {
        (void)host;
        if (TrySumFlat<double>(src, out))
            return;
        double acc = 0.0;
        const double* cells = CellsOf<const double>(src);
        StrideWalk(src, [&](int64_t addr) {
            acc = acc + cells[addr];
        });
        *out = acc;
    };
    t.sumAllI32 = [](NativeHost* host, NativeArrayView& src, int64_t* out) {
        (void)host;
        int64_t acc = 0;
        const int32_t* cells = CellsOf<const int32_t>(src);
        StrideWalk(src, [&](int64_t addr) {
            acc = acc + cells[addr];
        });
        *out = acc;
    };
    t.sumAllI64 = [](NativeHost* host, NativeArrayView& src, int64_t* out) {
        (void)host;
        int64_t acc = 0;
        const int64_t* cells = CellsOf<const int64_t>(src);
        StrideWalk(src, [&](int64_t addr) {
            acc = acc + cells[addr];
        });
        *out = acc;
    };
    //dot (lifted from DotKernel run-parts): floats take the carrier
    //flat dot; ints keep the 64-bit walk. Shapes may differ while the
    //counts match (2x3 . 3x2 flattens alike) — both walk their own
    //odometers.
    t.dotF32 = [](NativeHost* host, NativeArrayView& va,
                  NativeArrayView& vb, float* out) {
        (void)host;
        float flatAcc = 0.0f;
        if (TryDotFlat<float>(va, vb, &flatAcc)) {
            *out = flatAcc;
            return;
        }
        const float* ca = CellsOf<const float>(va);
        const float* cb = CellsOf<const float>(vb);
        int64_t idxA[nlang::kMaxArrayRank] = {0};
        int64_t idxB[nlang::kMaxArrayRank] = {0};
        int64_t addrA = va.offset;
        int64_t addrB = vb.offset;
        float acc = 0.0f;
        for (int64_t i = 0; i < va.elemCount; ++i) {
            acc = acc + ca[addrA] * cb[addrB];
            StrideWalkAdvance(va, idxA, &addrA);
            StrideWalkAdvance(vb, idxB, &addrB);
        }
        *out = acc;
    };
    t.dotF64 = [](NativeHost* host, NativeArrayView& va,
                  NativeArrayView& vb, double* out) {
        (void)host;
        double flatAcc = 0.0;
        if (TryDotFlat<double>(va, vb, &flatAcc)) {
            *out = flatAcc;
            return;
        }
        const double* ca = CellsOf<const double>(va);
        const double* cb = CellsOf<const double>(vb);
        int64_t idxA[nlang::kMaxArrayRank] = {0};
        int64_t idxB[nlang::kMaxArrayRank] = {0};
        int64_t addrA = va.offset;
        int64_t addrB = vb.offset;
        double acc = 0.0;
        for (int64_t i = 0; i < va.elemCount; ++i) {
            acc = acc + ca[addrA] * cb[addrB];
            StrideWalkAdvance(va, idxA, &addrA);
            StrideWalkAdvance(vb, idxB, &addrB);
        }
        *out = acc;
    };
    t.dotI32 = [](NativeHost* host, NativeArrayView& va,
                  NativeArrayView& vb, int64_t* out) {
        (void)host;
        const int32_t* ca = CellsOf<const int32_t>(va);
        const int32_t* cb = CellsOf<const int32_t>(vb);
        int64_t idxA[nlang::kMaxArrayRank] = {0};
        int64_t idxB[nlang::kMaxArrayRank] = {0};
        int64_t addrA = va.offset;
        int64_t addrB = vb.offset;
        int64_t acc = 0;
        for (int64_t i = 0; i < va.elemCount; ++i) {
            acc = acc + static_cast<int64_t>(ca[addrA])
                * static_cast<int64_t>(cb[addrB]);
            StrideWalkAdvance(va, idxA, &addrA);
            StrideWalkAdvance(vb, idxB, &addrB);
        }
        *out = acc;
    };
    t.dotI64 = [](NativeHost* host, NativeArrayView& va,
                  NativeArrayView& vb, int64_t* out) {
        (void)host;
        const int64_t* ca = CellsOf<const int64_t>(va);
        const int64_t* cb = CellsOf<const int64_t>(vb);
        int64_t idxA[nlang::kMaxArrayRank] = {0};
        int64_t idxB[nlang::kMaxArrayRank] = {0};
        int64_t addrA = va.offset;
        int64_t addrB = vb.offset;
        int64_t acc = 0;
        for (int64_t i = 0; i < va.elemCount; ++i) {
            acc = acc + ca[addrA] * cb[addrB];
            StrideWalkAdvance(va, idxA, &addrA);
            StrideWalkAdvance(vb, idxB, &addrB);
        }
        *out = acc;
    };
    //extreme (lifted from UnaryReduceKernel's Max/Min arm): floats take
    //the cmp-and-select flat kernels (the NaN/tie rule matches the
    //walk); ints keep the walk.
    t.extremeF32 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                      float* out) {
        (void)host;
        float flatBest = 0.0f;
        if (TryExtremeFlat<float>(src, isMax, &flatBest)) {
            *out = flatBest;
            return;
        }
        const float* cells = CellsOf<const float>(src);
        float best = 0.0f;
        bool first = true;
        StrideWalk(src, [&](int64_t addr) {
            const float cur = cells[addr];
            if (first || (isMax ? cur > best : cur < best)) {
                best = cur;
                first = false;
            }
        });
        *out = best;
    };
    t.extremeF64 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                      double* out) {
        (void)host;
        double flatBest = 0.0;
        if (TryExtremeFlat<double>(src, isMax, &flatBest)) {
            *out = flatBest;
            return;
        }
        const double* cells = CellsOf<const double>(src);
        double best = 0.0;
        bool first = true;
        StrideWalk(src, [&](int64_t addr) {
            const double cur = cells[addr];
            if (first || (isMax ? cur > best : cur < best)) {
                best = cur;
                first = false;
            }
        });
        *out = best;
    };
    t.extremeI32 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                      int32_t* out) {
        (void)host;
        const int32_t* cells = CellsOf<const int32_t>(src);
        int32_t best = 0;
        bool first = true;
        StrideWalk(src, [&](int64_t addr) {
            const int32_t cur = cells[addr];
            if (first || (isMax ? cur > best : cur < best)) {
                best = cur;
                first = false;
            }
        });
        *out = best;
    };
    t.extremeI64 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                      int64_t* out) {
        (void)host;
        const int64_t* cells = CellsOf<const int64_t>(src);
        int64_t best = 0;
        bool first = true;
        StrideWalk(src, [&](int64_t addr) {
            const int64_t cur = cells[addr];
            if (first || (isMax ? cur > best : cur < best)) {
                best = cur;
                first = false;
            }
        });
        *out = best;
    };
    //mean (lifted from MeanKernel): double accumulation, double return,
    //every element kind — int kinds accumulate double (never wraps).
    t.meanF32 = [](NativeHost* host, NativeArrayView& src, double* out) {
        (void)host;
        double acc = 0.0;
        if (TrySumFlat<float>(src, &acc)) {
            *out = acc / static_cast<double>(src.elemCount);
            return;
        }
        const float* cells = CellsOf<const float>(src);
        StrideWalk(src, [&](int64_t addr) {
            acc += static_cast<double>(cells[addr]);
        });
        *out = acc / static_cast<double>(src.elemCount);
    };
    t.meanF64 = [](NativeHost* host, NativeArrayView& src, double* out) {
        (void)host;
        double acc = 0.0;
        if (TrySumFlat<double>(src, &acc)) {
            *out = acc / static_cast<double>(src.elemCount);
            return;
        }
        const double* cells = CellsOf<const double>(src);
        StrideWalk(src, [&](int64_t addr) {
            acc += static_cast<double>(cells[addr]);
        });
        *out = acc / static_cast<double>(src.elemCount);
    };
    t.meanI32 = [](NativeHost* host, NativeArrayView& src, double* out) {
        (void)host;
        const int32_t* cells = CellsOf<const int32_t>(src);
        double acc = 0.0;
        StrideWalk(src, [&](int64_t addr) {
            acc += static_cast<double>(cells[addr]);
        });
        *out = acc / static_cast<double>(src.elemCount);
    };
    t.meanI64 = [](NativeHost* host, NativeArrayView& src, double* out) {
        (void)host;
        const int64_t* cells = CellsOf<const int64_t>(src);
        double acc = 0.0;
        StrideWalk(src, [&](int64_t addr) {
            acc += static_cast<double>(cells[addr]);
        });
        *out = acc / static_cast<double>(src.elemCount);
    };
    //clamp (lifted from ClampKernel run-parts): floats ride the flat
    //clamp; ints keep the walk.
    t.clampF32 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                    float bound, NativeArrayView& dst) {
        (void)host;
        RunClampFlat<float>(src, dst, isMax, bound);
    };
    t.clampF64 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                    double bound, NativeArrayView& dst) {
        (void)host;
        RunClampFlat<double>(src, dst, isMax, bound);
    };
    t.clampI32 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                    int32_t bound, NativeArrayView& dst) {
        (void)host;
        const int32_t* in = CellsOf<const int32_t>(src);
        int32_t* outCells = CellsOf<int32_t>(dst);
        int64_t at = 0;
        StrideWalk(src, [&](int64_t addr) {
            outCells[at++] = detail::ClampCell(isMax, in[addr], bound);
        });
    };
    t.clampI64 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                    int64_t bound, NativeArrayView& dst) {
        (void)host;
        const int64_t* in = CellsOf<const int64_t>(src);
        int64_t* outCells = CellsOf<int64_t>(dst);
        int64_t at = 0;
        StrideWalk(src, [&](int64_t addr) {
            outCells[at++] = detail::ClampCell(isMax, in[addr], bound);
        });
    };
    //reduceAxis (lifted from SumAxisKernel/ExtremeAxisKernel run-parts:
    //the flat fast paths first, then the odometer walks; the dispatcher
    //keeps the axis/rank-1/empty validations and the scalar-return arm).
    t.reduceAxisF32 = [](NativeHost* host, ReduceAxisOp op,
                         NativeArrayView& src, int32_t axis,
                         NativeArrayView& dst) {
        ReduceAxisImpl<float>(host, op, src, axis, dst);
    };
    t.reduceAxisF64 = [](NativeHost* host, ReduceAxisOp op,
                         NativeArrayView& src, int32_t axis,
                         NativeArrayView& dst) {
        ReduceAxisImpl<double>(host, op, src, axis, dst);
    };
    t.reduceAxisI32 = [](NativeHost* host, ReduceAxisOp op,
                         NativeArrayView& src, int32_t axis,
                         NativeArrayView& dst) {
        ReduceAxisImpl<int32_t>(host, op, src, axis, dst);
    };
    t.reduceAxisI64 = [](NativeHost* host, ReduceAxisOp op,
                         NativeArrayView& src, int32_t axis,
                         NativeArrayView& dst) {
        ReduceAxisImpl<int64_t>(host, op, src, axis, dst);
    };

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
