/*---
    TestReduceStrided.cpp — the view-layer strided reduction contract.

    Constructs NativeArrayViews whose strides are not the dense row-major
    ones (step-slice, transpose, negative stride, zero-stride broadcast)
    and runs the reduction family — the whole-array sum/extreme/dot/mean
    and the per-axis reduceAxis/argmaxI32 — through the TensorOps slots,
    asserting each result matches an independent reference: a flat
    reduction over the dense-equivalent elements.

    The ctest links only the CPU object library, so OpsForResidency
    resolves to the CPU table; the CPU reduction slots are stride-aware
    (the TrySumFlat/TryDotFlat fast paths never fire on a non-dense view,
    so the StrideWalk odometer is what runs). This suite pins the view-layer
    contract that the CUDA strided kernels mirror — the CUDA path itself is
    covered by the e2e double-run (tools/gate_cuda_tree.sh sets
    NLANG_TENSOR_BACKEND=cuda), which is where the strided kernels go
    red-then-green. matmul/concat are deliberately absent here: their CPU
    slots assume dense operands, so their strided path is e2e-only too.
---*/
#include "TestHarness.h"
#include "TensorOps.h"

#include <nlang/vm/NativeHost.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace test_native_tensor {

namespace {

//The ctest links only the CPU object library: host residency always
//resolves to the CPU table (the device table is never active here).
const tensor::TensorOps& Ops() {
    return tensor::CpuTensorOps();
}

//Same-order walk; a relative budget this small only guards FMA contraction
//(the strided path is a single-order walk, so the result is bit-exact with
//the flat reference apart from that).
constexpr double kSumTol = 1e-12;
constexpr double kDotTol = 1e-6;

//The spec-defined addressing (NativeArrayView doc): element (i0, i1, ...)
//lives at data + (offset + sum(i_k * strides[k])) * elemCellBytes. This
//reference decomposes each flat index into row-major coordinates and applies
//that formula directly — independent of the slot's odometer walk.
template <typename T>
std::vector<T> DenseEquivalent(const NativeArrayView& v) {
    const T* owner = static_cast<const T*>(v.data);
    std::vector<T> dense(static_cast<size_t>(v.elemCount));
    for (int64_t flat = 0; flat < v.elemCount; ++flat) {
        int64_t idx[64] = {0};
        int64_t rem = flat;
        for (int32_t d = v.rank - 1; d >= 0; --d) {
            idx[d] = rem % v.dims[d];
            rem /= v.dims[d];
        }
        int64_t addr = v.offset;
        for (int32_t d = 0; d < v.rank; ++d)
            addr += idx[d] * v.strides[d];
        dense[static_cast<size_t>(flat)] = owner[static_cast<size_t>(addr)];
    }
    return dense;
}

template <typename T>
uint8_t KindOf() {
    if constexpr (std::is_same_v<T, float>) return NEK_Float32;
    if constexpr (std::is_same_v<T, double>) return NEK_Float64;
    if constexpr (std::is_same_v<T, int32_t>) return NEK_Int32;
    return NEK_Int64;
}

//Build a NativeArrayView over a host owner buffer for one strided variant.
//The caller owns `owner` (sized ownerCells) and `dims`/`strides` (each
//sized rank). reserved[0] is host residency (0) — the ctest's table.
void MakeView(NativeArrayView& v, void* owner, const int32_t* dims,
              const int32_t* strides, int32_t rank, uint8_t kind,
              uint8_t cellBytes, int64_t offset, int64_t count) {
    v = NativeArrayView{};
    v.data = owner;
    v.dims = dims;
    v.strides = v.stridesInline;
    for (int32_t d = 0; d < rank; ++d)
        v.stridesInline[d] = strides[d];
    v.rank = rank;
    v.elemKind = kind;
    v.elemCellBytes = cellBytes;
    v.reserved[0] = 0; //host
    v.offset = offset;
    v.elemCount = count;
}

//A varied owner fill (cell k = k): a constant fill would mask an addressing
//bug, so the values must differ. Small exact values keep the int lanes
//away from wraparound.
template <typename T>
void FillOwner(std::vector<T>& owner) {
    for (size_t k = 0; k < owner.size(); ++k)
        owner[k] = static_cast<T>(k);
}

//The six strided variants. Every one is non-dense (so the flat fast paths
//never fire) and reachable within its owner buffer (see the plan's
//owner-sizing note). The rank-2 variants exercise transpose (strides
//[1,4]), row step-slice ([10,1]), column step-slice ([5,2]) and the
//zero-stride broadcast ([0,1]); the rank-1 ones exercise the step-slice
//([2]) and the negative-stride reverse ([-1]).
struct Variant {
    int32_t rank;
    const int32_t* dims;
    const int32_t* strides;
    int64_t offset;
    int64_t count;
    int64_t ownerCells;
};

const int32_t kDims1_17[1] = {17};
const int32_t kStr1_step[1] = {2};
const int32_t kStr1_neg[1] = {-1};
const int32_t kDims2_45[2] = {4, 5};
const int32_t kStr2_transpose[2] = {1, 4};
const int32_t kStr2_rowStep[2] = {10, 1};
const int32_t kStr2_colStep[2] = {5, 2};
const int32_t kStr2_bcast[2] = {0, 1};

const Variant kVariants[] = {
    {1, kDims1_17, kStr1_step, 3, 17, 40},    //step-slice rank-1
    {1, kDims1_17, kStr1_neg, 16, 17, 17},    //negative stride rank-1
    {2, kDims2_45, kStr2_transpose, 0, 20, 20}, //transpose [1,4]
    {2, kDims2_45, kStr2_rowStep, 0, 20, 40},   //row step-slice [10,1]
    {2, kDims2_45, kStr2_colStep, 0, 20, 30},   //col step-slice [5,2]
    {2, kDims2_45, kStr2_bcast, 0, 20, 5},      //zero-stride broadcast [0,1]
};

//Run one whole-array reduction over every variant for a given dtype and
//compare the slot's scalar out against a flat reduction of dense[]. The
//slot and the reference accumulate in the same row-major order, so the
//budget only guards FMA contraction.
template <typename T, typename RunSlot, typename RefFlat>
void CheckWholeArray(const char* what, RunSlot runSlot, RefFlat refFlat,
                      double relTol) {
    for (const Variant& var : kVariants) {
        std::vector<T> owner(static_cast<size_t>(var.ownerCells));
        FillOwner<T>(owner);
        NativeArrayView v = {};
        MakeView(v, owner.data(), var.dims, var.strides, var.rank,
                 KindOf<T>(), static_cast<uint8_t>(sizeof(T)), var.offset,
                 var.count);
        auto dense = DenseEquivalent<T>(v);
        const double want = refFlat(dense);
        const double got = runSlot(v);
        const double scale = std::max(1.0, std::fabs(want));
        const double gap = std::fabs(got - want) / scale;
        NTT_CHECK(gap <= relTol, std::string(what) + " strided (worst "
            + std::to_string(gap) + ")");
    }
}

//Per-axis reference: for out cell o (the source coords with the axis
//removed), fold the dense-equivalent elements over the axis. axis==1 keeps
//rows (o = row, fold C columns); axis==0 keeps columns (o = column, fold R
//rows). Returns the Sum/Max/Min fold per cell.
void PerAxisReference(const std::vector<float>& dense, int32_t R, int32_t C,
                      int32_t axis, int32_t outCount, float* outSum,
                      float* outMax, float* outMin) {
    for (int32_t o = 0; o < outCount; ++o) {
        const int64_t flat0 =
            axis == 1 ? static_cast<int64_t>(o) * C : static_cast<int64_t>(o);
        float sum = dense[flat0];
        float mx = dense[flat0];
        float mn = dense[flat0];
        for (int32_t s = 1; s < (axis == 1 ? C : R); ++s) {
            const int64_t flat =
                axis == 1 ? static_cast<int64_t>(o) * C + s
                           : static_cast<int64_t>(s) * C + o;
            sum += dense[flat];
            mx = std::max(mx, dense[flat]);
            mn = std::min(mn, dense[flat]);
        }
        outSum[o] = sum;
        outMax[o] = mx;
        outMin[o] = mn;
    }
}

//argmax/argmin reference: first-wins index per out cell (the slot's
//contract — a tie keeps the earlier index).
void ArgExtremeReference(const std::vector<float>& dense, int32_t R,
                         int32_t C, int32_t axis, int32_t outCount,
                         bool isMax, int32_t* out) {
    for (int32_t o = 0; o < outCount; ++o) {
        const int64_t flat0 =
            axis == 1 ? static_cast<int64_t>(o) * C : static_cast<int64_t>(o);
        int32_t idx = 0;
        float best = dense[flat0];
        for (int32_t s = 1; s < (axis == 1 ? C : R); ++s) {
            const int64_t flat =
                axis == 1 ? static_cast<int64_t>(o) * C + s
                           : static_cast<int64_t>(s) * C + o;
            const float cur = dense[flat];
            if (isMax ? cur > best : cur < best) {
                idx = s;
                best = cur;
            }
        }
        out[o] = idx;
    }
}

void CheckSumStrided() {
    NTT_TEST(Sum_strided_matches_reference);
    CheckWholeArray<float>("sum",
        [](NativeArrayView& v) -> double {
            double out; Ops().sumAllF32(nullptr, v, &out); return out; },
        [](const std::vector<float>& d) {
            double acc = 0; for (float x : d) acc += x; return acc; },
        kSumTol);
    CheckWholeArray<double>("sum",
        [](NativeArrayView& v) -> double {
            double out; Ops().sumAllF64(nullptr, v, &out); return out; },
        [](const std::vector<double>& d) {
            double acc = 0; for (double x : d) acc += x; return acc; },
        kSumTol);
    CheckWholeArray<int32_t>("sum",
        [](NativeArrayView& v) -> double {
            int64_t out; Ops().sumAllI32(nullptr, v, &out);
            return static_cast<double>(out); },
        [](const std::vector<int32_t>& d) {
            int64_t acc = 0; for (int32_t x : d) acc += x;
            return static_cast<double>(acc); },
        kSumTol);
    CheckWholeArray<int64_t>("sum",
        [](NativeArrayView& v) -> double {
            int64_t out; Ops().sumAllI64(nullptr, v, &out);
            return static_cast<double>(out); },
        [](const std::vector<int64_t>& d) {
            int64_t acc = 0; for (int64_t x : d) acc += x;
            return static_cast<double>(acc); },
        kSumTol);
    NTT_PASS();
}

void CheckExtremeStrided() {
    NTT_TEST(Extreme_strided_matches_reference);
    CheckWholeArray<float>("max",
        [](NativeArrayView& v) -> double {
            float out; Ops().extremeF32(nullptr, true, v, &out); return out; },
        [](const std::vector<float>& d) {
            float best = d[0]; for (float x : d) if (x > best) best = x;
            return best; },
        kSumTol);
    CheckWholeArray<float>("min",
        [](NativeArrayView& v) -> double {
            float out; Ops().extremeF32(nullptr, false, v, &out);
            return out; },
        [](const std::vector<float>& d) {
            float best = d[0]; for (float x : d) if (x < best) best = x;
            return best; },
        kSumTol);
    CheckWholeArray<double>("max",
        [](NativeArrayView& v) -> double {
            double out; Ops().extremeF64(nullptr, true, v, &out);
            return out; },
        [](const std::vector<double>& d) {
            double best = d[0]; for (double x : d) if (x > best) best = x;
            return best; },
        kSumTol);
    CheckWholeArray<int32_t>("max",
        [](NativeArrayView& v) -> double {
            int32_t out; Ops().extremeI32(nullptr, true, v, &out);
            return static_cast<double>(out); },
        [](const std::vector<int32_t>& d) {
            int32_t best = d[0]; for (int32_t x : d) if (x > best) best = x;
            return static_cast<double>(best); },
        kSumTol);
    CheckWholeArray<int64_t>("max",
        [](NativeArrayView& v) -> double {
            int64_t out; Ops().extremeI64(nullptr, true, v, &out);
            return static_cast<double>(out); },
        [](const std::vector<int64_t>& d) {
            int64_t best = d[0]; for (int64_t x : d) if (x > best) best = x;
            return static_cast<double>(best); },
        kSumTol);
    NTT_PASS();
}

void CheckDotStrided() {
    NTT_TEST(Dot_strided_matches_reference);
    //dot(a, a): the two operands are the same strided view (sum of squares
    //of the logical elements).
    CheckWholeArray<float>("dot",
        [](NativeArrayView& v) -> double {
            float out; Ops().dotF32(nullptr, v, v, &out); return out; },
        [](const std::vector<float>& d) {
            float acc = 0; for (float x : d) acc = acc + x * x; return acc; },
        kDotTol);
    CheckWholeArray<double>("dot",
        [](NativeArrayView& v) -> double {
            double out; Ops().dotF64(nullptr, v, v, &out); return out; },
        [](const std::vector<double>& d) {
            double acc = 0; for (double x : d) acc = acc + x * x;
            return acc; },
        kSumTol);
    CheckWholeArray<int32_t>("dot",
        [](NativeArrayView& v) -> double {
            int64_t out; Ops().dotI32(nullptr, v, v, &out);
            return static_cast<double>(out); },
        [](const std::vector<int32_t>& d) {
            int64_t acc = 0;
            for (int32_t x : d) acc += static_cast<int64_t>(x) * x;
            return static_cast<double>(acc); },
        kSumTol);
    CheckWholeArray<int64_t>("dot",
        [](NativeArrayView& v) -> double {
            int64_t out; Ops().dotI64(nullptr, v, v, &out);
            return static_cast<double>(out); },
        [](const std::vector<int64_t>& d) {
            int64_t acc = 0;
            for (int64_t x : d) acc += x * x;
            return static_cast<double>(acc); },
        kSumTol);
    NTT_PASS();
}

void CheckMeanStrided() {
    NTT_TEST(Mean_strided_matches_reference);
    CheckWholeArray<float>("mean",
        [](NativeArrayView& v) -> double {
            double out; Ops().meanF32(nullptr, v, &out); return out; },
        [](const std::vector<float>& d) {
            double acc = 0; for (float x : d) acc += x;
            return acc / static_cast<double>(d.size()); },
        kSumTol);
    CheckWholeArray<int32_t>("mean",
        [](NativeArrayView& v) -> double {
            double out; Ops().meanI32(nullptr, v, &out); return out; },
        [](const std::vector<int32_t>& d) {
            double acc = 0; for (int32_t x : d) acc += x;
            return acc / static_cast<double>(d.size()); },
        kSumTol);
    NTT_PASS();
}

void CheckReduceAxisStrided() {
    NTT_TEST(ReduceAxis_strided_matches_reference);
    const int32_t R = 4, C = 5;
    for (int32_t axis = 0; axis <= 1; ++axis) {
        std::vector<float> owner(2 * R * C);
        FillOwner<float>(owner);
        const int32_t dims[2] = {R, C};
        const int32_t strides[2] = {2 * C, 1};
        NativeArrayView src = {};
        MakeView(src, owner.data(), dims, strides, 2, NEK_Float32, 4, 0,
                 R * C);
        auto dense = DenseEquivalent<float>(src);
        const int32_t outCount = axis == 0 ? C : R;
        std::vector<float> dst(static_cast<size_t>(outCount));
        const int32_t dstDims[1] = {outCount};
        const int32_t dstStrides[1] = {1};
        NativeArrayView dstView = {};
        MakeView(dstView, dst.data(), dstDims, dstStrides, 1, NEK_Float32, 4,
                 0, outCount);
        std::vector<float> wantSum(outCount), wantMax(outCount),
            wantMin(outCount);
        PerAxisReference(dense, R, C, axis, outCount, wantSum.data(),
                         wantMax.data(), wantMin.data());
        for (int op = 0; op <= 2; ++op) {
            std::fill(dst.begin(), dst.end(), 0.0f);
            const auto opEnum = static_cast<tensor::ReduceAxisOp>(op);
            Ops().reduceAxisF32(nullptr, opEnum, src, axis, dstView);
            for (int32_t o = 0; o < outCount; ++o) {
                const float want =
                    op == 0 ? wantSum[o] : (op == 1 ? wantMax[o]
                                                   : wantMin[o]);
                NTT_CHECK(std::fabs(dst[o] - want)
                              <= 1e-5 * std::max(1.0f, std::fabs(want)),
                          std::string("reduceAxis op ") + std::to_string(op)
                              + " axis " + std::to_string(axis)
                              + " cell " + std::to_string(o));
            }
        }
    }
    NTT_PASS();
}

void CheckArgExtremeStrided() {
    NTT_TEST(ArgExtreme_strided_matches_reference);
    const int32_t R = 4, C = 5;
    for (int32_t axis = 0; axis <= 1; ++axis) {
        std::vector<float> owner(2 * R * C);
        FillOwner<float>(owner);
        const int32_t dims[2] = {R, C};
        const int32_t strides[2] = {2 * C, 1};
        NativeArrayView src = {};
        MakeView(src, owner.data(), dims, strides, 2, NEK_Float32, 4, 0,
                 R * C);
        auto dense = DenseEquivalent<float>(src);
        const int32_t outCount = axis == 0 ? C : R;
        std::vector<int32_t> dst(static_cast<size_t>(outCount));
        const int32_t dstDims[1] = {outCount};
        const int32_t dstStrides[1] = {1};
        NativeArrayView dstView = {};
        MakeView(dstView, dst.data(), dstDims, dstStrides, 1, NEK_Int32, 4,
                 0, outCount);
        std::vector<int32_t> want(outCount);
        for (int isMax = 0; isMax <= 1; ++isMax) {
            ArgExtremeReference(dense, R, C, axis, outCount,
                                isMax != 0, want.data());
            std::fill(dst.begin(), dst.end(), 0);
            Ops().argmaxI32(nullptr, isMax, src, axis, dstView);
            for (int32_t o = 0; o < outCount; ++o) {
                NTT_CHECK(dst[o] == want[o],
                          std::string("argmax axis ") + std::to_string(axis)
                              + " cell " + std::to_string(o) + " got "
                              + std::to_string(dst[o]) + " want "
                              + std::to_string(want[o]));
            }
        }
    }
    NTT_PASS();
}

} // namespace

int RunReduceStridedTests() {
    CheckSumStrided();
    CheckExtremeStrided();
    CheckDotStrided();
    CheckMeanStrided();
    CheckReduceAxisStrided();
    CheckArgExtremeStrided();
    std::cerr << "reduce-strided suites: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail;
}

} // namespace test_native_tensor
