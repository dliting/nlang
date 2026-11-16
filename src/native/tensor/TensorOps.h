/*---
    TensorOps.h — the whole-op backend contract of the tensor native
    module (v0.14.x device line, P1). Where KernelOps is the CPU-internal
    SIMD seam (flat spans), TensorOps is THE registration surface every
    backend implements: one slot per tensor operation, whole-op signature.
    The dispatchers reach a table through OpsForResidency (backend
    registry) and never learn what hardware is underneath.

    Dispatch layering (spec 2026-11-14 D1):
      dispatcher (kind checks, shape/broadcast validation, residency
      routing, output mint)
        → all-host operands  : host path (the existing driver code,
          reached THROUGH the CPU TensorOps slots)
        → all-device operands: the active backend's device slots
        → mixed              : named diagnostic (spec D2/D6)

    Operands ride NativeArrayView (the dispatcher's pinned views, valid
    for the call duration) — the CPU slots pass them straight to the
    existing Elementwise.h drivers + KernelOps (the SIMD stack, totally
    untouched); device kernels read the same fields (dims/strides/
    offset/data/elemKind, reserved[0] residency).

    Every slot takes NativeHost* first: the CPU lanes raise catchable
    exceptions mid-walk (integer division by zero), device lanes raise
    after their sync via the same callbacks — one calling convention for
    both error models (spec's device error-flag mechanism ends in the
    same host->raiseException).
---*/
#pragma once

#include "TensorBackends.h"   //UnaryKernelOp/BinaryKernelOp enums
#include "TensorKernels.h"    //NativeArrayView via NativeHost.h

#include <cstdint>

namespace tensor {

//ReduceAxisOp: the axis-reduction families that mint an output ARRAY
//(the scalar-returning full reductions have their own slots).
enum class ReduceAxisOp : int32_t { Sum, Max, Min, ArgMax, ArgMin };

struct TensorOps {
    //--- elementwise family (rank-preserving; dst is the fresh mint) ---
    void (*unaryF32)(NativeHost* host, UnaryKernelOp op,
                     NativeArrayView& src, NativeArrayView& dst);
    void (*unaryF64)(NativeHost* host, UnaryKernelOp op,
                     NativeArrayView& src, NativeArrayView& dst);
    //int unary: only Neg reaches here (exp/log/tanh are float/double
    //surfaces); int neg wraps like language arithmetic in the walk.
    void (*unaryI32)(NativeHost* host, UnaryKernelOp op,
                     NativeArrayView& src, NativeArrayView& dst);
    void (*unaryI64)(NativeHost* host, UnaryKernelOp op,
                     NativeArrayView& src, NativeArrayView& dst);
    void (*binaryF32)(NativeHost* host, BinaryKernelOp op,
                      NativeArrayView& a, NativeArrayView& b,
                      NativeArrayView& dst);
    void (*binaryF64)(NativeHost* host, BinaryKernelOp op,
                      NativeArrayView& a, NativeArrayView& b,
                      NativeArrayView& dst);
    //int array-array arms: the walk keeps wrap-and-raise semantics
    //(per-cell DivByZero on the divisor cell; MIN/-1 wraps unsigned).
    void (*binaryI32)(NativeHost* host, BinaryKernelOp op,
                      NativeArrayView& a, NativeArrayView& b,
                      NativeArrayView& dst);
    void (*binaryI64)(NativeHost* host, BinaryKernelOp op,
                      NativeArrayView& a, NativeArrayView& b,
                      NativeArrayView& dst);
    void (*binaryScalarF32)(NativeHost* host, BinaryKernelOp op,
                            NativeArrayView& a, float scalar,
                            NativeArrayView& dst);
    void (*binaryScalarF64)(NativeHost* host, BinaryKernelOp op,
                            NativeArrayView& a, double scalar,
                            NativeArrayView& dst);
    void (*binaryScalarI32)(NativeHost* host, BinaryKernelOp op,
                            NativeArrayView& a, int32_t scalar,
                            NativeArrayView& dst);
    void (*binaryScalarI64)(NativeHost* host, BinaryKernelOp op,
                            NativeArrayView& a, int64_t scalar,
                            NativeArrayView& dst);
    //Clamp family (max(a, bound)/min(a, bound) — relu rides max).
    void (*clampF32)(NativeHost* host, bool isMax,
                     NativeArrayView& src, float bound,
                     NativeArrayView& dst);
    void (*clampF64)(NativeHost* host, bool isMax,
                     NativeArrayView& src, double bound,
                     NativeArrayView& dst);
    void (*clampI32)(NativeHost* host, bool isMax,
                     NativeArrayView& src, int32_t bound,
                     NativeArrayView& dst);
    void (*clampI64)(NativeHost* host, bool isMax,
                     NativeArrayView& src, int64_t bound,
                     NativeArrayView& dst);

    //--- scalar-returning reductions (the native syncs/copies back; the
    //slot writes the out scalar) ---
    void (*sumAllF32)(NativeHost* host, NativeArrayView& src,
                      double* out);
    void (*sumAllF64)(NativeHost* host, NativeArrayView& src,
                      double* out);
    void (*sumAllI32)(NativeHost* host, NativeArrayView& src,
                      int64_t* out);   //wrap add
    void (*sumAllI64)(NativeHost* host, NativeArrayView& src,
                      int64_t* out);
    void (*dotF32)(NativeHost* host, NativeArrayView& a,
                   NativeArrayView& b, float* out);
    void (*dotF64)(NativeHost* host, NativeArrayView& a,
                   NativeArrayView& b, double* out);
    //int dot: 64-bit accumulation, declared long return.
    void (*dotI32)(NativeHost* host, NativeArrayView& a,
                   NativeArrayView& b, int64_t* out);
    void (*dotI64)(NativeHost* host, NativeArrayView& a,
                   NativeArrayView& b, int64_t* out);
    //mean: double accumulation, double return, every element kind
    //(int kinds accumulate double — never wraps).
    void (*meanF32)(NativeHost* host, NativeArrayView& src, double* out);
    void (*meanF64)(NativeHost* host, NativeArrayView& src, double* out);
    void (*meanI32)(NativeHost* host, NativeArrayView& src, double* out);
    void (*meanI64)(NativeHost* host, NativeArrayView& src, double* out);
    void (*extremeF32)(NativeHost* host, bool isMax,
                       NativeArrayView& src, float* out);
    void (*extremeF64)(NativeHost* host, bool isMax,
                       NativeArrayView& src, double* out);
    void (*extremeI32)(NativeHost* host, bool isMax,
                       NativeArrayView& src, int32_t* out);
    void (*extremeI64)(NativeHost* host, bool isMax,
                       NativeArrayView& src, int64_t* out);

    //--- axis reductions (dst drops the axis dim; ArgMax/ArgMin mint
    //int32 indices with the first-wins y>x comparator) ---
    void (*reduceAxisI32)(NativeHost* host, ReduceAxisOp op,
                          NativeArrayView& src, int32_t axis,
                          NativeArrayView& dst);
    void (*reduceAxisI64)(NativeHost* host, ReduceAxisOp op,
                          NativeArrayView& src, int32_t axis,
                          NativeArrayView& dst);
    void (*reduceAxisF32)(NativeHost* host, ReduceAxisOp op,
                          NativeArrayView& src, int32_t axis,
                          NativeArrayView& dst);
    void (*reduceAxisF64)(NativeHost* host, ReduceAxisOp op,
                          NativeArrayView& src, int32_t axis,
                          NativeArrayView& dst);

    //--- matmul: C[rows, cols] accumulated into zeroed dst (the caller
    //mints fresh arrays; the k order is the scalar i-k-j contract).
    //Non-contiguous operands are the IMPLEMENTATION's problem — the
    //host path materializes first, device kernels may take strides.
    void (*matmulF32)(NativeHost* host, NativeArrayView& a,
                      NativeArrayView& b, NativeArrayView& dst);
    void (*matmulF64)(NativeHost* host, NativeArrayView& a,
                      NativeArrayView& b, NativeArrayView& dst);
    void (*matmulI32)(NativeHost* host, NativeArrayView& a,
                      NativeArrayView& b, NativeArrayView& dst);
    void (*matmulI64)(NativeHost* host, NativeArrayView& a,
                      NativeArrayView& b, NativeArrayView& dst);

    //--- phase 2 family ---
    //gather: take-per-position along axis (indices rank-1 int32,
    //length == non-axis count; rank-1 src degenerates to fancy
    //indexing). Out-of-range raises per the cell-level error mechanism.
    void (*gatherF32)(NativeHost* host, NativeArrayView& src,
                      int32_t axis, NativeArrayView& indices,
                      NativeArrayView& dst);
    void (*gatherF64)(NativeHost* host, NativeArrayView& src,
                      int32_t axis, NativeArrayView& indices,
                      NativeArrayView& dst);
    void (*gatherI32)(NativeHost* host, NativeArrayView& src,
                      int32_t axis, NativeArrayView& indices,
                      NativeArrayView& dst);
    void (*gatherI64)(NativeHost* host, NativeArrayView& src,
                      int32_t axis, NativeArrayView& indices,
                      NativeArrayView& dst);
    //argmax/argmin along axis: int32 winner indices, first-wins tie.
    void (*argmaxI32)(NativeHost* host, bool isMax,
                      NativeArrayView& src, int32_t axis,
                      NativeArrayView& dst);
    //eq: broadcast elementwise same-kind compare, int32 0/1 out.
    void (*eqF32)(NativeHost* host, NativeArrayView& a,
                  NativeArrayView& b, NativeArrayView& dst);
    void (*eqF64)(NativeHost* host, NativeArrayView& a,
                  NativeArrayView& b, NativeArrayView& dst);
    void (*eqI32)(NativeHost* host, NativeArrayView& a,
                  NativeArrayView& b, NativeArrayView& dst);
    void (*eqI64)(NativeHost* host, NativeArrayView& a,
                  NativeArrayView& b, NativeArrayView& dst);
    //oneHot: int32 labels → float32 [len, n] rows.
    void (*oneHotI32)(NativeHost* host, NativeArrayView& labels,
                      int32_t n, NativeArrayView& dst);
    //uniform: fills dst with low + (high-low)*u. mtState/position carry
    //the snapshotRngForDevice snapshot (the host already discarded n);
    //the CPU slot draws via the host PRNG stream instead and ignores
    //them.
    void (*uniformF32)(NativeHost* host, NativeArrayView& dst,
                       float low, float high, const uint32_t* mtState,
                       uint32_t position);
    void (*uniformF64)(NativeHost* host, NativeArrayView& dst,
                       double low, double high, const uint32_t* mtState,
                       uint32_t position);

    //--- data generation & movement family ---
    void (*fillF32)(NativeHost* host, NativeArrayView& dst,
                    double value);
    void (*fillF64)(NativeHost* host, NativeArrayView& dst,
                    double value);
    void (*fillI32)(NativeHost* host, NativeArrayView& dst,
                    int64_t value);
    void (*fillI64)(NativeHost* host, NativeArrayView& dst,
                    int64_t value);
    //arange: dst[i] = start + i*step (half-open contract checked by the
    //dispatcher before minting).
    void (*arangeI32)(NativeHost* host, NativeArrayView& dst,
                      int64_t start, int64_t step);
    void (*arangeI64)(NativeHost* host, NativeArrayView& dst,
                      int64_t start, int64_t step);
    void (*arangeF32)(NativeHost* host, NativeArrayView& dst,
                      double start, double step);
    void (*arangeF64)(NativeHost* host, NativeArrayView& dst,
                      double start, double step);
    //concat along axis (dims equal off-axis, sum on axis).
    void (*concatF32)(NativeHost* host, NativeArrayView& a,
                      NativeArrayView& b, int32_t axis,
                      NativeArrayView& dst);
    void (*concatF64)(NativeHost* host, NativeArrayView& a,
                      NativeArrayView& b, int32_t axis,
                      NativeArrayView& dst);
    void (*concatI32)(NativeHost* host, NativeArrayView& a,
                      NativeArrayView& b, int32_t axis,
                      NativeArrayView& dst);
    void (*concatI64)(NativeHost* host, NativeArrayView& a,
                      NativeArrayView& b, int32_t axis,
                      NativeArrayView& dst);
    //copy: elementwise materialization (same shape, dense dst) — the
    //shared primitive of view materialization, toDevice/toHost and
    //serialization staging.
    void (*copyF32)(NativeHost* host, NativeArrayView& src,
                    NativeArrayView& dst);
    void (*copyF64)(NativeHost* host, NativeArrayView& src,
                    NativeArrayView& dst);
    void (*copyI32)(NativeHost* host, NativeArrayView& src,
                    NativeArrayView& dst);
    void (*copyI64)(NativeHost* host, NativeArrayView& src,
                    NativeArrayView& dst);
};

//The cpu table's direct lookup (design 2026-11-15 R2): the host arm of
//OpsForResidency and every "run this on host" site (staged walks,
//transfer twins) — never the env-selected backend's table.
const TensorOps& CpuTensorOps();

} // namespace tensor
