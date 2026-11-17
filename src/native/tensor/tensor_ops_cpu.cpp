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

    The registry: the cpu table is the module's only static table;
    CpuTensorOps() returns it directly and OpsForResidency routes
    device-resident operands to the active backend's table (design
    2026-11-15 R2 — one routing rule, no name lookup).
---*/
#include "TensorOps.h"
#include "backend_registry.h"
#include "../tensor_cuda/backend_descriptor.h"

#include "Elementwise.h"

#include <cmath>
#include <cstdint>
#include <type_traits>
#include <vector>

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
        //rdiv's divisor is the array cell: the per-cell raise mirrors
        //the array/array walk (the scalar walk's own contract).
        if (op == BinaryKernelOp::Rdiv && cells[addr] == T(0))
            Raise(host, NEXC_DivideByZero,
                  "tensor.div: integer division by zero");
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

//--- concat copy (lifted from CopyConcatBlocks — byte-level, kind-
//independent; the dispatcher materialized both operands so the bases
//fold their view offsets in and each (outer-slice, source) block is
//one memcpy) ---
void CpuConcat(const NativeArrayView& a, const NativeArrayView& b,
               int32_t axis, const NativeArrayView& out) {
    int64_t outer = 1;
    int64_t inner = 1;
    for (int32_t d = 0; d < axis; ++d)
        outer *= a.dims[d];
    for (int32_t d = axis + 1; d < a.rank; ++d)
        inner *= a.dims[d];
    const int64_t cellBytes = a.elemCellBytes;
    const int64_t aAlong = a.dims[axis];
    const int64_t bAlong = b.dims[axis];
    const char* aCells = static_cast<const char*>(a.data)
        + static_cast<int64_t>(a.offset) * cellBytes;
    const char* bCells = static_cast<const char*>(b.data)
        + static_cast<int64_t>(b.offset) * cellBytes;
    char* outCells = static_cast<char*>(out.data);
    for (int64_t o = 0; o < outer; ++o) {
        char* dstA = outCells
            + o * (aAlong + bAlong) * inner * cellBytes;
        std::memcpy(dstA, aCells + o * aAlong * inner * cellBytes,
                    static_cast<size_t>(aAlong * inner) * cellBytes);
        std::memcpy(dstA + aAlong * inner * cellBytes,
                    bCells + o * bAlong * inner * cellBytes,
                    static_cast<size_t>(bAlong * inner) * cellBytes);
    }
}

//--- T1.2d phase 2 family (lifted from tensor_gather.cpp run-parts) ---

//Rank-1 int32 index read over the view's own strides (the gather
//indices walk; the kind/rank gate stays in the dispatcher).
void CpuReadIndices(const NativeArrayView& v, std::vector<int32_t>* out) {
    out->clear();
    out->reserve(static_cast<size_t>(v.elemCount));
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addr = v.offset;
    const int32_t* cells = static_cast<const int32_t*>(v.data);
    for (int64_t i = 0; i < v.elemCount; ++i) {
        out->push_back(cells[addr]);
        StrideWalkAdvance(v, idx, &addr);
    }
}

//A gather-shaped visit: walks the out flat order (dims minus axis),
//hands each visit the source address of the non-axis position, the
//flat non-axis position, and the axis index for that position
//(indices[flat]). Works on any view (addressing via the recorded
//strides).
template <typename Fn>
void VisitGather(const NativeArrayView& a, int32_t axis,
                 const std::vector<int32_t>& indices, Fn fn) {
    const int32_t rank = a.rank;
    const int32_t along = a.dims[axis];
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addr = a.offset;
    int64_t flat = 0;
    for (int64_t f = 0; f < a.elemCount / along; ++f) {
        fn(addr, flat, indices[static_cast<size_t>(flat)]);
        //Advance the non-axis odometer: innermost first, skipping the
        //axis dim (the gather drives it from indices).
        for (int32_t d = rank - 1; d >= 0; --d) {
            if (d == axis)
                continue;
            idx[d] += 1;
            addr += a.strides[d];
            if (idx[d] < a.dims[d])
                break;
            addr -= static_cast<int64_t>(a.dims[d]) * a.strides[d];
            idx[d] = 0;
        }
        flat += 1;
    }
}

//One gather slot body (lifted from GatherKernel's run-parts: the rank-1
//fancy-indexing walk and the VisitGather walk, per-cell raises).
template <typename T>
void CpuGather(NativeHost* host, const NativeArrayView& va, int32_t axis,
               const NativeArrayView& vIdx, const NativeArrayView& out) {
    std::vector<int32_t> indices;
    CpuReadIndices(vIdx, &indices);
    const int32_t along = va.dims[axis];
    const T* cells = static_cast<const T*>(va.data);
    T* outCells = static_cast<T*>(out.data);
    if (va.rank == 1) {
        for (size_t i = 0; i < indices.size(); ++i) {
            const int32_t index = indices[i];
            if (index < 0 || index >= along)
                Raise(host, NEXC_OutOfBounds,
                      "tensor.gather: index " + std::to_string(index)
                      + " out of bounds for axis 0 with size "
                      + std::to_string(along));
            outCells[i] = cells[va.offset + index * va.strides[0]];
        }
        return;
    }
    VisitGather(va, axis, indices,
                [&](int64_t addr, int64_t, int32_t index) {
        if (index < 0 || index >= along)
            Raise(host, NEXC_OutOfBounds,
                  "tensor.gather: index " + std::to_string(index)
                  + " out of bounds for axis " + std::to_string(axis)
                  + " with size " + std::to_string(along));
        *outCells++ = cells[addr + index * va.strides[axis]];
    });
}

//One argmax/argmin slot body (lifted from ExtremeAxisIndicesKernel's
//walk: the outer odometer with the first-wins axis scan).
template <typename T>
void CpuArgExtreme(NativeHost* host, bool isMax, const NativeArrayView& va,
                   int32_t axis, const NativeArrayView& out) {
    (void)host;
    const int32_t along = va.dims[axis];
    const int64_t outer = va.elemCount / along;
    const int32_t outRank = va.rank - 1;
    int32_t outDims[nlang::kMaxArrayRank];
    AxisRemovedDims(va, axis, outDims);
    int32_t* outCells = static_cast<int32_t*>(out.data);
    const T* cells = static_cast<const T*>(va.data);
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addr = va.offset;
    for (int64_t f = 0; f < outer; ++f) {
        int32_t best = 0;
        T bestVal = cells[addr];
        for (int32_t s = 1; s < along; ++s) {
            const T cur = cells[addr + s * va.strides[axis]];
            if (isMax ? cur > bestVal : cur < bestVal) {
                bestVal = cur;
                best = s;
            }
        }
        outCells[f] = best;
        AxisRemovedAdvance(va, axis, outRank, outDims, idx, &addr);
    }
}

//One eq slot body (lifted from EqKernel: the broadcast odometer with
//the == compare).
//The per-point compare: IEEE ordering for the float kinds (NaN false
//in every ordered compare and in Eq, true in Ne — numpy's rule); plain
//integer ordering for the int kinds. Returns the 0/1 mask cell.
template <typename T>
int32_t ApplyCompare(CompareOp op, T a, T b) {
    switch (op) {
        case CompareOp::Eq: return a == b ? 1 : 0;
        case CompareOp::Ne: return a != b ? 1 : 0;
        case CompareOp::Gt: return a > b ? 1 : 0;
        case CompareOp::Ge: return a >= b ? 1 : 0;
        case CompareOp::Lt: return a < b ? 1 : 0;
        case CompareOp::Le: return a <= b ? 1 : 0;
    }
    return 0;
}

template <typename T>
void CpuCompare(CompareOp op, const NativeArrayView& va,
                const NativeArrayView& vb, const NativeArrayView& out) {
    const T* ca = static_cast<const T*>(va.data);
    const T* cb = static_cast<const T*>(vb.data);
    int32_t* cout = static_cast<int32_t*>(out.data);
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addA = va.offset;
    int64_t addB = vb.offset;
    int32_t sa[nlang::kMaxArrayRank];
    int32_t sb[nlang::kMaxArrayRank];
    BroadcastStrides(va, out.rank, sa);
    BroadcastStrides(vb, out.rank, sb);
    for (int64_t i = 0; i < out.elemCount; ++i) {
        cout[i] = ApplyCompare(op, ca[addA], cb[addB]);
        BroadcastAdvance(sa, sb, out.dims, out.rank, idx, &addA, &addB);
    }
}

//The scalar arm of the compare family: one odometer over the array
//(the scalar side carries no strides).
template <typename T>
void CpuCompareScalar(CompareOp op, const NativeArrayView& va, T scalar,
                      const NativeArrayView& out) {
    const T* ca = static_cast<const T*>(va.data);
    int32_t* cout = static_cast<int32_t*>(out.data);
    int64_t at = va.offset;
    for (int64_t i = 0; i < out.elemCount; ++i) {
        cout[i] = ApplyCompare(op, ca[at], scalar);
        at += va.strides[0];
    }
}

//The where walk: one odometer over the output advancing three tracks
//(mask always an array; x/y arrays or scalar arms reading the slot's
//field). Mask != 0 picks x else y (numpy's truthiness).
template <typename T, typename M>
void CpuWhere(const NativeArrayView& mask, const NativeArrayView& vx,
              const NativeArrayView& vy, const NativeArrayView& out,
              bool maskLong, bool xScalar, bool yScalar, double sx,
              double sy, int64_t isx, int64_t isy) {
    const M* cm = static_cast<const M*>(mask.data);
    const T* cx = static_cast<const T*>(vx.data);
    const T* cy = static_cast<const T*>(vy.data);
    T* cout = static_cast<T*>(out.data);
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addM = mask.offset;
    int64_t addX = vx.offset;
    int64_t addY = vy.offset;
    int32_t sm[nlang::kMaxArrayRank];
    int32_t sxb[nlang::kMaxArrayRank];
    int32_t syb[nlang::kMaxArrayRank];
    BroadcastStrides(mask, out.rank, sm);
    (void)maskLong;
    if (!xScalar)
        BroadcastStrides(vx, out.rank, sxb);
    if (!yScalar)
        BroadcastStrides(vy, out.rank, syb);
    //The scalar fields: the float kinds read the double; the int
    //kinds read the exact int64 twin (the double roundtrip would lose
    //precision above 2^53).
    const T xs = std::is_integral_v<T> ? static_cast<T>(isx)
                                       : static_cast<T>(sx);
    const T ys = std::is_integral_v<T> ? static_cast<T>(isy)
                                       : static_cast<T>(sy);
    for (int64_t i = 0; i < out.elemCount; ++i) {
        cout[i] = cm[addM] != 0
            ? (xScalar ? xs : cx[addX])
            : (yScalar ? ys : cy[addY]);
        //Advance the three odometers (the manual form — BroadcastAdvance
        //advances exactly two tracks).
        for (int32_t d = out.rank - 1; d >= 0; --d) {
            idx[d] += 1;
            if (idx[d] < out.dims[d]) {
                addM += sm[d];
                if (!xScalar)
                    addX += sxb[d];
                if (!yScalar)
                    addY += syb[d];
                break;
            }
            idx[d] = 0;
            addM -= sm[d] * (out.dims[d] - 1);
            if (!xScalar)
                addX -= sxb[d] * (out.dims[d] - 1);
            if (!yScalar)
                addY -= syb[d] * (out.dims[d] - 1);
        }
    }
}

//--- copy (the same-shape elementwise materialization — StrideCopy's
//walk typed per kind; the shared primitive of view materialization,
//toDevice/toHost and serialization staging) ---
template <typename T>
void CpuCopy(const NativeArrayView& src, const NativeArrayView& dst) {
    const T* cells = CellsOf<const T>(src);
    T* outCells = CellsOf<T>(dst);
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addr = src.offset;
    for (int64_t i = 0; i < dst.elemCount; ++i) {
        outCells[i] = cells[addr];
        StrideWalkAdvance(src, idx, &addr);
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
    //--- T1.2d phase 2 family (gather/argmax/eq/oneHot; uniform keeps
    //its dispatcher until the P2 snapshot signature) ---
    t.gatherF32 = [](NativeHost* host, NativeArrayView& src, int32_t axis,
                     NativeArrayView& indices, NativeArrayView& dst) {
        CpuGather<float>(host, src, axis, indices, dst);
    };
    t.gatherF64 = [](NativeHost* host, NativeArrayView& src, int32_t axis,
                     NativeArrayView& indices, NativeArrayView& dst) {
        CpuGather<double>(host, src, axis, indices, dst);
    };
    t.gatherI32 = [](NativeHost* host, NativeArrayView& src, int32_t axis,
                     NativeArrayView& indices, NativeArrayView& dst) {
        CpuGather<int32_t>(host, src, axis, indices, dst);
    };
    t.gatherI64 = [](NativeHost* host, NativeArrayView& src, int32_t axis,
                     NativeArrayView& indices, NativeArrayView& dst) {
        CpuGather<int64_t>(host, src, axis, indices, dst);
    };
    t.argmaxI32 = [](NativeHost* host, bool isMax, NativeArrayView& src,
                     int32_t axis, NativeArrayView& dst) {
        //Kind dispatch lives in the dispatcher; the slot is int32-index
        //output over a same-kind source — dispatch by source kind here.
        switch (src.elemKind) {
            case NEK_Int32:
                CpuArgExtreme<int32_t>(host, isMax, src, axis, dst); break;
            case NEK_Int64:
                CpuArgExtreme<int64_t>(host, isMax, src, axis, dst); break;
            case NEK_Float32:
                CpuArgExtreme<float>(host, isMax, src, axis, dst); break;
            default:
                CpuArgExtreme<double>(host, isMax, src, axis, dst); break;
        }
    };
    t.compareF32 = [](NativeHost* host, CompareOp op, NativeArrayView& a,
                      NativeArrayView& b, NativeArrayView& dst) {
        (void)host;
        CpuCompare<float>(op, a, b, dst);
    };
    t.compareF64 = [](NativeHost* host, CompareOp op, NativeArrayView& a,
                      NativeArrayView& b, NativeArrayView& dst) {
        (void)host;
        CpuCompare<double>(op, a, b, dst);
    };
    t.compareI32 = [](NativeHost* host, CompareOp op, NativeArrayView& a,
                      NativeArrayView& b, NativeArrayView& dst) {
        (void)host;
        CpuCompare<int32_t>(op, a, b, dst);
    };
    t.compareI64 = [](NativeHost* host, CompareOp op, NativeArrayView& a,
                      NativeArrayView& b, NativeArrayView& dst) {
        (void)host;
        CpuCompare<int64_t>(op, a, b, dst);
    };
    t.compareScalarF32 = [](NativeHost* host, CompareOp op,
                            NativeArrayView& a, double scalar,
                            NativeArrayView& dst) {
        (void)host;
        CpuCompareScalar<float>(op, a, static_cast<float>(scalar), dst);
    };
    t.compareScalarF64 = [](NativeHost* host, CompareOp op,
                            NativeArrayView& a, double scalar,
                            NativeArrayView& dst) {
        (void)host;
        CpuCompareScalar<double>(op, a, scalar, dst);
    };
    t.compareScalarI32 = [](NativeHost* host, CompareOp op,
                            NativeArrayView& a, int64_t scalar,
                            NativeArrayView& dst) {
        (void)host;
        CpuCompareScalar<int32_t>(op, a, static_cast<int32_t>(scalar), dst);
    };
    t.compareScalarI64 = [](NativeHost* host, CompareOp op,
                            NativeArrayView& a, int64_t scalar,
                            NativeArrayView& dst) {
        (void)host;
        CpuCompareScalar<int64_t>(op, a, scalar, dst);
    };
    t.whereF32 = [](NativeHost* host, NativeArrayView& mask,
                    NativeArrayView& x, NativeArrayView& y,
                    NativeArrayView& dst, bool maskLong, bool xScalar,
                    bool yScalar, double sx, double sy) {
        (void)host;
        if (maskLong)
            CpuWhere<float, int64_t>(mask, x, y, dst, maskLong, xScalar,
                yScalar, sx, sy, 0, 0);
        else
            CpuWhere<float, int32_t>(mask, x, y, dst, maskLong, xScalar,
                yScalar, sx, sy, 0, 0);
    };
    t.whereF64 = [](NativeHost* host, NativeArrayView& mask,
                    NativeArrayView& x, NativeArrayView& y,
                    NativeArrayView& dst, bool maskLong, bool xScalar,
                    bool yScalar, double sx, double sy) {
        (void)host;
        if (maskLong)
            CpuWhere<double, int64_t>(mask, x, y, dst, maskLong, xScalar,
                yScalar, sx, sy, 0, 0);
        else
            CpuWhere<double, int32_t>(mask, x, y, dst, maskLong, xScalar,
                yScalar, sx, sy, 0, 0);
    };
    t.whereI32 = [](NativeHost* host, NativeArrayView& mask,
                    NativeArrayView& x, NativeArrayView& y,
                    NativeArrayView& dst, bool maskLong, bool xScalar,
                    bool yScalar, int64_t isx, int64_t isy) {
        (void)host;
        if (maskLong)
            CpuWhere<int32_t, int64_t>(mask, x, y, dst, maskLong, xScalar,
                yScalar, 0.0, 0.0, isx, isy);
        else
            CpuWhere<int32_t, int32_t>(mask, x, y, dst, maskLong, xScalar,
                yScalar, 0.0, 0.0, isx, isy);
    };
    t.scatterF32 = [](NativeHost* host, NativeArrayView& a,
                      int32_t axis, NativeArrayView& indices,
                      NativeArrayView& values, NativeArrayView& dst,
                      bool valuesScalar, double sv, int32_t* errorOut) {
        (void)errorOut;
        ScatterCpuF32(host, a, axis, indices, values, dst, valuesScalar,
            sv, errorOut);
    };
    t.scatterF64 = [](NativeHost* host, NativeArrayView& a,
                      int32_t axis, NativeArrayView& indices,
                      NativeArrayView& values, NativeArrayView& dst,
                      bool valuesScalar, double sv, int32_t* errorOut) {
        (void)errorOut;
        ScatterCpuF64(host, a, axis, indices, values, dst, valuesScalar,
            sv, errorOut);
    };
    t.scatterI32 = [](NativeHost* host, NativeArrayView& a,
                      int32_t axis, NativeArrayView& indices,
                      NativeArrayView& values, NativeArrayView& dst,
                      bool valuesScalar, int64_t isv, int32_t* errorOut) {
        (void)errorOut;
        ScatterCpuI32(host, a, axis, indices, values, dst, valuesScalar,
            isv, errorOut);
    };
    t.scatterI64 = [](NativeHost* host, NativeArrayView& a,
                      int32_t axis, NativeArrayView& indices,
                      NativeArrayView& values, NativeArrayView& dst,
                      bool valuesScalar, int64_t isv, int32_t* errorOut) {
        (void)errorOut;
        ScatterCpuI64(host, a, axis, indices, values, dst, valuesScalar,
            isv, errorOut);
    };
    t.whereI64 = [](NativeHost* host, NativeArrayView& mask,
                    NativeArrayView& x, NativeArrayView& y,
                    NativeArrayView& dst, bool maskLong, bool xScalar,
                    bool yScalar, int64_t isx, int64_t isy) {
        (void)host;
        if (maskLong)
            CpuWhere<int64_t, int64_t>(mask, x, y, dst, maskLong, xScalar,
                yScalar, 0.0, 0.0, isx, isy);
        else
            CpuWhere<int64_t, int32_t>(mask, x, y, dst, maskLong, xScalar,
                yScalar, 0.0, 0.0, isx, isy);
    };
    t.oneHotI32 = [](NativeHost* host, NativeArrayView& labels, int32_t n,
                     NativeArrayView& dst) {
        const int32_t len = labels.dims[0];
        const int32_t* labelsCells = static_cast<const int32_t*>(
            labels.data);
        float* outCells = static_cast<float*>(dst.data);
        int64_t addr = labels.offset;
        for (int32_t i = 0; i < len; ++i) {
            if (labelsCells[addr] < 0 || labelsCells[addr] >= n)
                Raise(host, NEXC_OutOfBounds,
                      "tensor.oneHot: label "
                      + std::to_string(labelsCells[addr])
                      + " out of range for " + std::to_string(n)
                      + " classes");
            addr += labels.strides[0];
        }
        for (int32_t i = 0; i < len * n; ++i)
            outCells[i] = 0.0f;
        addr = labels.offset;
        for (int32_t i = 0; i < len; ++i) {
            outCells[static_cast<int64_t>(i) * n + labelsCells[addr]]
                = 1.0f;
            addr += labels.strides[0];
        }
    };
    //--- T1.2e generation & movement family (lifted from FillKernel/
    //FullDimsKernel/ArangeKernel run-parts: the mint and its validation
    //stay in the dispatcher; the slots fill/progress over the fresh
    //dst. copy = StrideCopy — the same-shape dense-dst walk that
    //Materialize uses, shared by P2's toDevice/toHost staging) ---
    t.fillF32 = [](NativeHost* host, NativeArrayView& dst, double value) {
        (void)host;
        float* cells = CellsOf<float>(dst);
        for (int64_t i = 0; i < dst.elemCount; ++i)
            cells[i] = static_cast<float>(value);
    };
    t.fillF64 = [](NativeHost* host, NativeArrayView& dst, double value) {
        (void)host;
        double* cells = CellsOf<double>(dst);
        for (int64_t i = 0; i < dst.elemCount; ++i)
            cells[i] = value;
    };
    t.fillI32 = [](NativeHost* host, NativeArrayView& dst, int64_t value) {
        (void)host;
        int32_t* cells = CellsOf<int32_t>(dst);
        for (int64_t i = 0; i < dst.elemCount; ++i)
            cells[i] = static_cast<int32_t>(value);
    };
    t.fillI64 = [](NativeHost* host, NativeArrayView& dst, int64_t value) {
        (void)host;
        int64_t* cells = CellsOf<int64_t>(dst);
        for (int64_t i = 0; i < dst.elemCount; ++i)
            cells[i] = value;
    };
    //arange: dst[i] = start + i*step accumulated in the element carrier
    //((T)cur + (T)step per element — the historical rounding).
    t.arangeI32 = [](NativeHost* host, NativeArrayView& dst, int64_t start,
                     int64_t step) {
        (void)host;
        int32_t* cells = CellsOf<int32_t>(dst);
        int32_t cur = static_cast<int32_t>(start);
        for (int64_t i = 0; i < dst.elemCount; ++i) {
            cells[i] = cur;
            cur = static_cast<int32_t>(cur + static_cast<int32_t>(step));
        }
    };
    t.arangeI64 = [](NativeHost* host, NativeArrayView& dst, int64_t start,
                     int64_t step) {
        (void)host;
        int64_t* cells = CellsOf<int64_t>(dst);
        int64_t cur = start;
        for (int64_t i = 0; i < dst.elemCount; ++i) {
            cells[i] = cur;
            cur = cur + step;
        }
    };
    t.arangeF32 = [](NativeHost* host, NativeArrayView& dst, double start,
                     double step) {
        (void)host;
        float* cells = CellsOf<float>(dst);
        float cur = static_cast<float>(start);
        for (int64_t i = 0; i < dst.elemCount; ++i) {
            cells[i] = cur;
            cur = static_cast<float>(cur + static_cast<float>(step));
        }
    };
    t.arangeF64 = [](NativeHost* host, NativeArrayView& dst, double start,
                     double step) {
        (void)host;
        double* cells = CellsOf<double>(dst);
        double cur = start;
        for (int64_t i = 0; i < dst.elemCount; ++i) {
            cells[i] = cur;
            cur = cur + step;
        }
    };
    //copy: same-shape elementwise materialization (StrideCopy's walk,
    //typed per kind).
    t.copyF32 = [](NativeHost* host, NativeArrayView& src,
                   NativeArrayView& dst) {
        (void)host;
        CpuCopy<float>(src, dst);
    };
    t.copyF64 = [](NativeHost* host, NativeArrayView& src,
                   NativeArrayView& dst) {
        (void)host;
        CpuCopy<double>(src, dst);
    };
    t.copyI32 = [](NativeHost* host, NativeArrayView& src,
                   NativeArrayView& dst) {
        (void)host;
        CpuCopy<int32_t>(src, dst);
    };
    t.copyI64 = [](NativeHost* host, NativeArrayView& src,
                   NativeArrayView& dst) {
        (void)host;
        CpuCopy<int64_t>(src, dst);
    };
    //--- T1.2c matmul family (lifted from MatMulTyped's run-part: the
    //dispatcher materializes non-contiguous operands and gates shapes,
    //so the slots receive dense rank-2 operands; the flat bases fold
    //the view offsets in — FlatCellsOf — and dst is freshly minted) ---
    t.matmulF32 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, NativeArrayView& dst) {
        (void)host;
        ActiveKernelOps().matmulF32(a.dims[0], a.dims[1], b.dims[1],
            FlatCellsOf<float>(a), FlatCellsOf<float>(b),
            CellsOf<float>(dst));
    };
    t.matmulF64 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, NativeArrayView& dst) {
        (void)host;
        ActiveKernelOps().matmulF64(a.dims[0], a.dims[1], b.dims[1],
            FlatCellsOf<double>(a), FlatCellsOf<double>(b),
            CellsOf<double>(dst));
    };
    t.matmulI32 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, NativeArrayView& dst) {
        (void)host;
        ActiveKernelOps().matmulI32(a.dims[0], a.dims[1], b.dims[1],
            FlatCellsOf<int32_t>(a), FlatCellsOf<int32_t>(b),
            CellsOf<int32_t>(dst));
    };
    t.matmulI64 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, NativeArrayView& dst) {
        (void)host;
        ActiveKernelOps().matmulI64(a.dims[0], a.dims[1], b.dims[1],
            FlatCellsOf<int64_t>(a), FlatCellsOf<int64_t>(b),
            CellsOf<int64_t>(dst));
    };
    //--- T1.2c concat family (lifted from CopyConcatBlocks — the byte-
    //level copy is kind-independent, one shared body) ---
    t.concatF32 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, int32_t axis,
                     NativeArrayView& dst) {
        (void)host;
        CpuConcat(a, b, axis, dst);
    };
    t.concatF64 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, int32_t axis,
                     NativeArrayView& dst) {
        (void)host;
        CpuConcat(a, b, axis, dst);
    };
    t.concatI32 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, int32_t axis,
                     NativeArrayView& dst) {
        (void)host;
        CpuConcat(a, b, axis, dst);
    };
    t.concatI64 = [](NativeHost* host, NativeArrayView& a,
                     NativeArrayView& b, int32_t axis,
                     NativeArrayView& dst) {
        (void)host;
        CpuConcat(a, b, axis, dst);
    };

    return t;
}

const TensorOps kCpuTable = MakeCpuTable();

} //namespace

//The cpu table's stable lookup (design 2026-11-15 R2): OpsForResidency's
//host arm — and every "run this on host" site (staged walks, transfer
//twins) — must reach the cpu table DIRECTLY. The historical
//device-first override on the env-selected lookup routed a staged host
//walk to the cuda table under NLANG_TENSOR_BACKEND=cuda (the mirror
//backend masked this — its table IS the cpu table).
const TensorOps& CpuTensorOps() {
    return kCpuTable;
}

} // namespace tensor
