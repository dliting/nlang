/*---
    Elementwise.h — in-module driver between the native entries and the
    KernelOps backend table: it owns shapes/strides/contiguity and the
    scalar-lane reference semantics; the table owns the hardware.

    Decision rule per call: a contiguous view rides the backend's flat
    kernels for the whole span; anything else walks strides and calls
    the scalar lane per element. Scalar-lane math here is THE reference
    the unit tests pin the flat kernels against.
---*/
#pragma once

#include "TensorBackends.h"
#include "TensorKernels.h"

#include <cmath>
#include <type_traits>
#include <vector>

namespace tensor {

namespace detail {

//Carrier-arithmetic binary lanes (float/double IEEE; the int families
//keep their wrap-and-raise loops in tensor_binary.cpp).
template <typename T>
T CarrierBinary(BinaryKernelOp op, T a, T b) {
    switch (op) {
        case BinaryKernelOp::Add: return static_cast<T>(a + b);
        case BinaryKernelOp::Sub: return static_cast<T>(a - b);
        case BinaryKernelOp::Mul: return static_cast<T>(a * b);
        case BinaryKernelOp::Div: return static_cast<T>(a / b);
        case BinaryKernelOp::Rsub: return static_cast<T>(b - a);
        case BinaryKernelOp::Rdiv: return static_cast<T>(b / a);
        case BinaryKernelOp::ReLUGrad:
            return b > T(0) ? a : T(0);
    }
    return T(0);
}

//The scalar clamp cell — one spelling for every tier's tail/stride
//loop (the lane kernels carry the same rule in SIMD form).
template <typename T>
T ClampCell(bool isMax, T v, T bound) {
    return isMax ? (v > bound ? v : bound) : (v < bound ? v : bound);
}

template <typename T>
T ScalarUnaryLane(UnaryKernelOp op, T v) {
    //Compute in double and narrow — the same rounding the accuracy
    //contracts' reference uses (std::exp(std::double) etc.).
    const double d = static_cast<double>(v);
    switch (op) {
        case UnaryKernelOp::Exp: return static_cast<T>(std::exp(d));
        case UnaryKernelOp::Log: return static_cast<T>(std::log(d));
        case UnaryKernelOp::Tanh: return static_cast<T>(std::tanh(d));
        case UnaryKernelOp::Neg: return static_cast<T>(-d);
    }
    return T(0);
}

template <typename T>
void UnaryByTable(UnaryKernelOp op, const T* src, T* dst, int64_t count) {
    if constexpr (std::is_same_v<T, float>) {
        ActiveKernelOps().unaryF32(op, src, dst, count);
    } else {
        ActiveKernelOps().unaryF64(op, src, dst, count);
    }
}

template <typename T>
void BinaryByTable(BinaryKernelOp op, const T* a, const T* b, T* out,
                   int64_t count) {
    if constexpr (std::is_same_v<T, float>) {
        ActiveKernelOps().binaryF32(op, a, b, out, count);
    } else {
        ActiveKernelOps().binaryF64(op, a, b, out, count);
    }
}

template <typename T>
void BinaryScalarByTable(BinaryKernelOp op, const T* a, T scalar, T* out,
                         int64_t count) {
    if constexpr (std::is_same_v<T, float>) {
        ActiveKernelOps().binaryScalarF32(op, a, scalar, out, count);
    } else {
        ActiveKernelOps().binaryScalarF64(op, a, scalar, out, count);
    }
}

inline bool SameDims(const NativeArrayView& a, const NativeArrayView& b) {
    if (a.rank != b.rank)
        return false;
    for (int32_t d = 0; d < a.rank; ++d) {
        if (a.dims[d] != b.dims[d])
            return false;
    }
    return true;
}

} // namespace detail

//Unary map over a whole view: contiguous -> backend flat kernel for the
//span; else the scalar lane through the stride walk.
template <typename T>
void RunUnaryFlat(const NativeArrayView& in, NativeArrayView& out,
                  UnaryKernelOp op) {
    const int64_t count = in.elemCount;
    if (count == 0)
        return;
    if (IsContiguous(in)) {
        detail::UnaryByTable<T>(op, FlatCellsOf<T>(in), CellsOf<T>(out),
                                count);
        return;
    }
    const T* cells = CellsOf<const T>(in);
    T* outCells = CellsOf<T>(out);
    int64_t at = 0;
    StrideWalk(in, [&](int64_t addr) {
        outCells[at++] = detail::ScalarUnaryLane<T>(op, cells[addr]);
    });
}

//Binary over two views: handles contiguous same-shape pairs and
//single-cell right operands through the backend's flat kernels.
//Returns false (writing nothing) when neither fast path applies — the
//caller falls back to its broadcast odometer.
template <typename T>
bool TryRunBinaryFlat(const NativeArrayView& va, const NativeArrayView& vb,
                      NativeArrayView& vout, BinaryKernelOp op) {
    const int64_t count = vout.elemCount;
    if (count == 0)
        return true;
    if (detail::SameDims(va, vb) && IsContiguous(va) && IsContiguous(vb)) {
        detail::BinaryByTable<T>(op, FlatCellsOf<T>(va), FlatCellsOf<T>(vb),
                                 CellsOf<T>(vout), count);
        return true;
    }
    if (vb.elemCount == 1 && IsContiguous(va)) {
        const T scalar = CellsOf<const T>(vb)[vb.offset];
        detail::BinaryScalarByTable<T>(op, FlatCellsOf<T>(va), scalar,
                                       CellsOf<T>(vout), count);
        return true;
    }
    return false;
}

//Binary with a carrier scalar (the addScalar native family).
template <typename T>
void RunBinaryScalarFlat(const NativeArrayView& v, NativeArrayView& vout,
                         BinaryKernelOp op, T scalar) {
    const int64_t count = v.elemCount;
    if (count == 0)
        return;
    if (IsContiguous(v)) {
        detail::BinaryScalarByTable<T>(op, FlatCellsOf<T>(v), scalar,
                                       CellsOf<T>(vout), count);
        return;
    }
    const T* cells = CellsOf<const T>(v);
    T* outCells = CellsOf<T>(vout);
    int64_t at = 0;
    StrideWalk(v, [&](int64_t addr) {
        outCells[at++] = detail::CarrierBinary<T>(op, cells[addr], scalar);
    });
}

//Clamp family: contiguous -> backend flat kernel; else the scalar
//carrier compare (exactly the historical loop body).
template <typename T>
void RunClampFlat(const NativeArrayView& v, NativeArrayView& vout,
                  bool isMax, T bound) {
    const int64_t count = v.elemCount;
    if (count == 0)
        return;
    if (IsContiguous(v)) {
        const T* src = FlatCellsOf<T>(v);
        T* outCells = CellsOf<T>(vout);
        if constexpr (std::is_same_v<T, float>) {
            ActiveKernelOps().clampF32(isMax, src, bound, outCells, count);
        } else {
            ActiveKernelOps().clampF64(isMax, src, bound, outCells, count);
        }
        return;
    }
    const T* in = CellsOf<const T>(v);
    T* outCells = CellsOf<T>(vout);
    int64_t at = 0;
    StrideWalk(v, [&](int64_t addr) {
        outCells[at++] = detail::ClampCell(isMax, in[addr], bound);
    });
}

//Reduction fast paths: true = written through the backend table;
//false = fall back to the caller's walk. sum returns the double
//accumulator; dot/max/min return the carrier cell.
template <typename T>
bool TrySumFlat(const NativeArrayView& v, double* out) {
    if (!IsContiguous(v) || v.elemCount == 0)
        return false;
    if constexpr (std::is_same_v<T, float>) {
        ActiveKernelOps().sumF32(FlatCellsOf<T>(v), v.elemCount, out);
    } else {
        ActiveKernelOps().sumF64(FlatCellsOf<T>(v), v.elemCount, out);
    }
    return true;
}

template <typename T>
bool TryDotFlat(const NativeArrayView& va, const NativeArrayView& vb,
                T* out) {
    if (!IsContiguous(va) || !IsContiguous(vb) || va.elemCount == 0)
        return false;
    if constexpr (std::is_same_v<T, float>) {
        ActiveKernelOps().dotF32(FlatCellsOf<T>(va), FlatCellsOf<T>(vb),
                                 va.elemCount, out);
    } else {
        ActiveKernelOps().dotF64(FlatCellsOf<T>(va), FlatCellsOf<T>(vb),
                                 va.elemCount, out);
    }
    return true;
}

template <typename T>
bool TryExtremeFlat(const NativeArrayView& v, bool isMax, T* out) {
    if (!IsContiguous(v) || v.elemCount == 0)
        return false;
    if constexpr (std::is_same_v<T, float>) {
        if (isMax) {
            ActiveKernelOps().maxF32(FlatCellsOf<T>(v), v.elemCount, out);
        } else {
            ActiveKernelOps().minF32(FlatCellsOf<T>(v), v.elemCount, out);
        }
    } else {
        if (isMax) {
            ActiveKernelOps().maxF64(FlatCellsOf<T>(v), v.elemCount, out);
        } else {
            ActiveKernelOps().minF64(FlatCellsOf<T>(v), v.elemCount, out);
        }
    }
    return true;
}

//Raw-span extreme (the axis-max fast path addresses rows directly):
//carrier cell out, count > 0 contract.
template <typename T>
void ExtremeFlatByCarrier(bool isMax, const T* cells, int64_t count,
                          T* out) {
    if constexpr (std::is_same_v<T, float>) {
        if (isMax) {
            ActiveKernelOps().maxF32(cells, count, out);
        } else {
            ActiveKernelOps().minF32(cells, count, out);
        }
    } else {
        if (isMax) {
            ActiveKernelOps().maxF64(cells, count, out);
        } else {
            ActiveKernelOps().minF64(cells, count, out);
        }
    }
}

//Flat-sum helpers over raw spans (the axis-arm fast paths address
//rows directly, without a view): double accumulator either way.
template <typename T>
void SumFlatByCarrier(const T* cells, int64_t count, double* out) {
    if constexpr (std::is_same_v<T, float>) {
        ActiveKernelOps().sumF32(cells, count, out);
    } else {
        ActiveKernelOps().sumF64(cells, count, out);
    }
}

template <typename T>
void AddIntoF64ByCarrier(double* acc, const T* row, int64_t count) {
    if constexpr (std::is_same_v<T, float>) {
        ActiveKernelOps().addIntoF64FromF32(acc, row, count);
    } else {
        ActiveKernelOps().addIntoF64(acc, row, count);
    }
}

//--- axis-arm fast paths (tensor_reduce.cpp) -------------------------------
//Float/double reductions on CONTIGUOUS sources reduce whole segments
//through the flat carriers; both mirror the per-cell double accumulator
//of the caller's general odometer. Each helper returns false when the
//shape has no fast path (strided views, middle axes, ints) — the
//caller keeps its walk.

//sum(a, axis): axis = rank-1 reduces contiguous segments (one flat
//double sum per output cell); axis = 0 accumulates whole rows through
//the double addInto.
template <typename T>
bool SumAxisFlatFastPath(const NativeArrayView& v, NativeArrayView& out,
                         int32_t axis, int32_t along, uint8_t* ret,
                         int32_t handle) {
    if constexpr (!std::is_integral_v<T>) {
        if (!IsContiguous(v))
            return false;
        T* outCells = CellsOf<T>(out);
        if (axis == v.rank - 1) {
            const int64_t outer = out.elemCount;
            const T* base = FlatCellsOf<T>(v);
            for (int64_t r = 0; r < outer; ++r) {
                double acc = 0.0;
                SumFlatByCarrier<T>(base + r * along, along, &acc);
                outCells[r] = static_cast<T>(acc);
            }
            nlang::native::ReturnInt(ret, handle);
            return true;
        }
        if (axis != 0)
            return false;
        int64_t inner = 1;
        for (int32_t d = 1; d < v.rank; ++d)
            inner *= v.dims[d];
        std::vector<double> acc(static_cast<size_t>(inner), 0.0);
        const T* base = FlatCellsOf<T>(v);
        for (int64_t o = 0; o < along; ++o)
            AddIntoF64ByCarrier<T>(acc.data(), base + o * inner, inner);
        for (int64_t j = 0; j < inner; ++j)
            outCells[j] = static_cast<T>(acc[j]);
        nlang::native::ReturnInt(ret, handle);
        return true;
    }
    return false;
}

//max/min(a, axis): axis = rank-1 reduces contiguous segments through
//the flat extreme kernels (NaN/tie rule identical to the general walk).
template <typename T>
bool ExtremeAxisFlatFastPath(const NativeArrayView& v, NativeArrayView& out,
                             int32_t axis, int32_t along, bool isMax,
                             uint8_t* ret, int32_t handle) {
    if constexpr (!std::is_integral_v<T>) {
        if (IsContiguous(v) && axis == v.rank - 1) {
            T* outCells = CellsOf<T>(out);
            const int64_t outer = out.elemCount;
            const T* base = FlatCellsOf<T>(v);
            for (int64_t r = 0; r < outer; ++r)
                ExtremeFlatByCarrier<T>(isMax, base + r * along, along,
                                        outCells + r);
            nlang::native::ReturnInt(ret, handle);
            return true;
        }
    }
    return false;
}

} // namespace tensor
