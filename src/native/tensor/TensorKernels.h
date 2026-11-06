/*---
TensorKernels.h — internal (in-DLL) interface of the tensor native
module: shared payload helpers and the entry-point declarations the
registration unit binds. Not installed; only the module's own .cpp
files include it.
---*/
#pragma once

#include <nlang/vm/NativeHost.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace tensor {

//The view's data pointer aliases the record's raw cells; each element
//type reads/writes its own carrier width. elemCellBytes is asserted by
//the callers' kind checks, not re-checked here.
template <typename T>
T* CellsOf(const NativeArrayView& v) {
    return static_cast<T*>(v.data);
}

//Base for the flat-addressing kernels (matmul, concat) that index a
//dense operand from element zero: a view's element zero sits `offset`
//elements past the owning payload's base, so the flat base must fold
//the offset in. Owning arrays report offset 0 — the same address as
//CellsOf. The stride-walking kernels must NOT use this: they already
//start their odometers at v.offset (StrideWalk), so it would apply
//twice.
template <typename T>
T* FlatCellsOf(const NativeArrayView& v) {
    return static_cast<T*>(static_cast<void*>(
        static_cast<uint8_t*>(v.data)
        + static_cast<int64_t>(v.offset) * v.elemCellBytes));
}

//"2x3" style shape text for the mismatch messages (elementwise and
//matmul raise with it).
inline std::string ShapeText(const NativeArrayView& v) {
    std::string text = std::to_string(v.dims[0]);
    for (int32_t d = 1; d < v.rank; ++d)
        text += "x" + std::to_string(v.dims[d]);
    return text;
}

//--- stride addressing (v0.12.7 Task 12) ---------------------------------------
//Owning records report dense row-major strides and offset 0; views
//report their recorded strides/offset in owner space. Every kernel
//below addresses as data[(offset + sum(idx*strides)) * elemCellBytes].

//Row-major dense strides of dims — the contiguity reference and the
//mint input for re-viewing.
inline void DenseStrides(const int32_t* dims, int32_t rank, int32_t* out) {
    int64_t acc = 1;
    for (int32_t d = rank - 1; d >= 0; --d) {
        out[d] = static_cast<int32_t>(acc);
        acc *= dims[d];
    }
}

//True when the view's strides are the dense strides of its dims (any
//offset) — the layout reshape/flatten can re-view without a copy, and
//the flat-index kernels can keep their memcpy fast paths.
inline bool IsContiguous(const NativeArrayView& v) {
    int32_t dense[nlang::kMaxArrayRank];
    DenseStrides(v.dims, v.rank, dense);
    for (int32_t d = 0; d < v.rank; ++d)
        if (v.strides[d] != dense[d])
            return false;
    return true;
}

//Advance a row-major odometer one step (idx/addr are caller-owned
//state). O(rank) worst case, O(1) on the inner axis — the shared step
//of the single- and paired-view walks below.
inline void StrideWalkAdvance(const NativeArrayView& v, int64_t* idx,
                              int64_t* addr) {
    for (int32_t d = v.rank - 1; d >= 0; --d) {
        idx[d] += 1;
        *addr += v.strides[d];
        if (idx[d] < v.dims[d])
            return;
        *addr -= static_cast<int64_t>(v.dims[d]) * v.strides[d];
        idx[d] = 0;
    }
}

//Visit every logical element of a view in row-major order, handing fn
//the element-unit address (offset included).
template <typename Fn>
void StrideWalk(const NativeArrayView& v, Fn fn) {
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addr = v.offset;
    for (int64_t i = 0; i < v.elemCount; ++i) {
        fn(addr);
        StrideWalkAdvance(v, idx, &addr);
    }
}

//Copy every element of src into out (same dims, dense out) in
//row-major order — the generic materialization body.
inline void StrideCopy(const NativeArrayView& src, NativeArrayView& out) {
    const int64_t cellBytes = src.elemCellBytes;
    const char* from = static_cast<const char*>(src.data);
    char* to = static_cast<char*>(out.data);
    int64_t flat = 0;
    StrideWalk(src, [&](int64_t addr) {
        std::memcpy(to + flat * cellBytes, from + addr * cellBytes,
                    static_cast<size_t>(cellBytes));
        flat += 1;
    });
}

//Dense owning copy of any view (fresh handle, same dims).
inline int32_t Materialize(NativeHost* host, const NativeArrayView& src) {
    const int32_t handle = nlang::native::NewArrayShaped(
        host, src.elemKind, src.rank, src.dims);
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    StrideCopy(src, out);
    return handle;
}

//Right-aligned broadcast rule: ranks may differ (missing leading axes
//read as size 1); aligned dims must be equal or one of them 1; the
//output takes the max. Raises naming both shapes (the §18.6 documented
//message — same text the old same-shape check raised).
inline int32_t BroadcastShapes(NativeHost* host, const NativeArrayView& a,
                               const NativeArrayView& b, int32_t* outDims) {
    const int32_t rank = a.rank > b.rank ? a.rank : b.rank;
    for (int32_t d = 0; d < rank; ++d) {
        const int32_t ax = d - (rank - a.rank);
        const int32_t bx = d - (rank - b.rank);
        const int32_t ad = ax < 0 ? 1 : a.dims[ax];
        const int32_t bd = bx < 0 ? 1 : b.dims[bx];
        if (ad != bd && ad != 1 && bd != 1)
            nlang::native::Raise(host, NEXC_Base, "tensor: shape mismatch ("
                + ShapeText(a) + " vs " + ShapeText(b) + ")");
        outDims[d] = ad > bd ? ad : bd;
    }
    return rank;
}

//The operand's strides mapped onto the broadcast output axes: padded
//leading axes and size-1 axes address with stride 0 (the cell repeats).
inline void BroadcastStrides(const NativeArrayView& v, int32_t outRank,
                             int32_t* out) {
    for (int32_t d = 0; d < outRank; ++d) {
        const int32_t ax = d - (outRank - v.rank);
        out[d] = ax < 0 || v.dims[ax] == 1 ? 0 : v.strides[ax];
    }
}

//Collect a rank-1 int array's cells in row-major order; returns the
//count. Raises for a non-int, non-rank-1, or over-capacity argument (a
//view rides its strides — rank-1 walk; every caller's buffer holds
//exactly kMaxArrayRank cells, so the walk is bounded by the same
//contract — never driven by the argument's own elemCount).
inline int32_t CollectIntCells(NativeHost* host, const NativeArrayView& v,
                               const char* who, int32_t* out) {
    if (v.elemKind != NEK_Int32 || v.rank != 1)
        nlang::native::Raise(host, NEXC_Base,
              std::string(who) + ": expects a rank-1 int array");
    const int32_t* cells = static_cast<const int32_t*>(v.data);
    int32_t at = 0;
    StrideWalk(v, [&](int64_t addr) {
        if (at >= nlang::kMaxArrayRank)
            nlang::native::Raise(host, NEXC_Base,
                  std::string(who) + ": shape list longer than "
                  + std::to_string(nlang::kMaxArrayRank) + " cells");
        out[at++] = cells[addr];
    });
    return at;
}

//ReadIntList: CollectIntCells plus an exact-length contract.
inline void ReadIntList(NativeHost* host, const uint8_t* args, int slot,
                        int32_t want, const char* who, int32_t* out) {
    NativeArrayView v;
    nlang::native::ArgArray(host, args, slot, &v);
    if (CollectIntCells(host, v, who, out) != want)
        nlang::native::Raise(host, NEXC_Base, std::string(who) + ": expects an int[] of "
            + std::to_string(want) + " cells");
}



//--- creation entries (tensor_native.cpp) --------------------------------------

void FullI(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullL(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullF(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullD(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullDimsI(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullDimsL(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullDimsF(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void FullDimsD(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ArangeI(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ArangeL(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ArangeF(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ArangeD(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
//ABI v10 exercise pair: mint a rank-1 view through newArrayView, and
//sum any rank-1 int array or view through getArrayView (both arms).
void StrideViewI(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void ViewSumI(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);

//--- elementwise binary entries (tensor_binary.cpp) -----------------------------
//The exp/log unary pair rides the same family: rank-preserving maps
//over float/double payloads.

void TensorAdd(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorSub(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorMul(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorDiv(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorExp(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorLog(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);

//--- reduction entries (tensor_reduce.cpp) --------------------------------------

void TensorSum(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorMax(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorMin(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorMean(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);
void TensorDot(NativeHost* host, uint8_t* ret, const uint8_t* args, int argc);

//--- matrix entries (tensor_matrix.cpp) ------------------------------------------
//reshape takes the whole target shape as its second argument (with the
//-1 inferred dim), so one entry serves every rank.

void TensorTranspose(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     int argc);
void TensorFlatten(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc);
void TensorReshape(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   int argc);
void TensorConcat(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc);
void TensorMatmul(NativeHost* host, uint8_t* ret, const uint8_t* args,
                  int argc);

//--- view entries (tensor_views.cpp, v0.12.7 Task 12) ---------------------------
//transpose dispatches on argc: 1 = full axis reversal, 2 = permutation
//(axes). flatten/reshape view contiguous sources and materialize the
//rest; slice is the all-axes function form; broadcastTo is the
//read-only zero-stride view.

void TensorSlice(NativeHost* host, uint8_t* ret, const uint8_t* args,
                 int argc);
void TensorBroadcastTo(NativeHost* host, uint8_t* ret, const uint8_t* args,
                       int argc);

//Scalar arms of the elementwise family: registered under their own
//names (addScalar/...) because the array and scalar forms share argc=2;
//the NLang surface wraps both as add/sub/mul/div overloads.
void TensorAddScalar(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     int argc);
void TensorSubScalar(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     int argc);
void TensorMulScalar(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     int argc);
void TensorDivScalar(NativeHost* host, uint8_t* ret, const uint8_t* args,
                     int argc);

} // namespace tensor
