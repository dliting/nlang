/*---
    TensorOps.h — the whole-op backend contract of the tensor native
    module (v0.14.x device line, P1). Where KernelOps is the CPU-internal
    SIMD seam (flat spans), TensorOps is THE registration surface every
    backend implements: one slot per tensor operation, whole-op signature
    (dims/broadcast-strides/offset/pointer per operand). Callers look up
    ActiveTensorOps() once per module call and never learn what hardware
    is underneath.

    Dispatch layering (spec 2026-11-14 D1):
      dispatcher (kind checks, shape/broadcast validation, residency
      routing, output mint)
        → all-host operands  : host path (the existing driver code,
          unchanged — reached THROUGH the CPU TensorOps slots)
        → all-device operands: the active backend's device slots
        → mixed              : named diagnostic (spec D2/D6)

    The CPU backend's TensorOps slots are thin wrappers over the existing
    Elementwise.h drivers + KernelOps (the SIMD stack is untouched);
    device backends (cuda first) implement the same slots as device
    kernels. Operand strides are the BROADCAST-CONTRACT strides the
    dispatcher computed (BroadcastStrides semantics) in out-shape space;
    data/offset address the record's real storage in element units —
    element (i0..) lives at data + (offset + sum(ik*strides[k])) * cell.
---*/
#pragma once

#include "TensorBackends.h"   //UnaryKernelOp/BinaryKernelOp enums
#include "TensorKernels.h"    //kMaxArrayRank via NativeArrayView include

#include <cstdint>

namespace tensor {

//ReduceAxisOp: the axis-reduction families that mint an output ARRAY
//(the scalar-returning full reductions have their own slots).
enum class ReduceAxisOp : int32_t { Sum, Max, Min, ArgMax, ArgMin };

//Whole-op operand: dims run in out-shape space, strides are the
//per-operand broadcast strides (0-stride = broadcast repeat), offset in
//element units of the real payload. elemCount is the OPERAND's own
//element count (product of its dims); the caller validates shapes.
struct TensorOperand {
    int32_t rank;
    const int32_t* dims;
    const int32_t* strides;
    int64_t offset;
    void* data;
    int64_t elemCount;
};

struct TensorOps {
    //--- elementwise family (rank-preserving; count from dst) ---
    void (*unaryF32)(UnaryKernelOp op, const TensorOperand& src,
                     const TensorOperand& dst);
    void (*unaryF64)(UnaryKernelOp op, const TensorOperand& src,
                     const TensorOperand& dst);
    void (*binaryF32)(BinaryKernelOp op, const TensorOperand& a,
                      const TensorOperand& b, const TensorOperand& dst);
    void (*binaryF64)(BinaryKernelOp op, const TensorOperand& a,
                      const TensorOperand& b, const TensorOperand& dst);
    void (*binaryScalarF32)(BinaryKernelOp op, const TensorOperand& a,
                            float scalar, const TensorOperand& dst);
    void (*binaryScalarF64)(BinaryKernelOp op, const TensorOperand& a,
                            double scalar, const TensorOperand& dst);
    //Clamp family (max(a, bound)/min(a, bound) — relu rides max).
    void (*clampF32)(bool isMax, const TensorOperand& src, float bound,
                     const TensorOperand& dst);
    void (*clampF64)(bool isMax, const TensorOperand& src, double bound,
                     const TensorOperand& dst);

    //--- scalar-returning reductions (sync copy-back is the native's
    //business; the slot writes the out scalar) ---
    void (*sumAllF32)(const TensorOperand& src, double* out);
    void (*sumAllF64)(const TensorOperand& src, double* out);
    void (*sumAllI32)(const TensorOperand& src, int64_t* out);  //wrap add
    void (*sumAllI64)(const TensorOperand& src, int64_t* out);
    void (*dotF32)(const TensorOperand& a, const TensorOperand& b,
                   float* out);
    void (*dotF64)(const TensorOperand& a, const TensorOperand& b,
                   double* out);
    void (*extremeF32)(bool isMax, const TensorOperand& src, float* out);
    void (*extremeF64)(bool isMax, const TensorOperand& src, double* out);
    void (*extremeI32)(bool isMax, const TensorOperand& src, int32_t* out);
    void (*extremeI64)(bool isMax, const TensorOperand& src, int64_t* out);

    //--- axis reductions (dst drops the axis dim; ArgMax/ArgMin mint
    //int32 indices, first-wins tie via the y>x comparator) ---
    void (*reduceAxisI32)(ReduceAxisOp op, const TensorOperand& src,
                          int32_t axis, const TensorOperand& dst);
    void (*reduceAxisI64)(ReduceAxisOp op, const TensorOperand& src,
                          int32_t axis, const TensorOperand& dst);
    void (*reduceAxisF32)(ReduceAxisOp op, const TensorOperand& src,
                          int32_t axis, const TensorOperand& dst);
    void (*reduceAxisF64)(ReduceAxisOp op, const TensorOperand& src,
                          int32_t axis, const TensorOperand& dst);

    //--- matmul: C[rows, cols] accumulated into zeroed dst (the caller
    //mints fresh arrays; the k order is the scalar i-k-j contract).
    //Non-contiguous operands are the IMPLEMENTATION's problem — the
    //host path materializes first, device kernels may take strides.
    void (*matmulF32)(const TensorOperand& a, const TensorOperand& b,
                      const TensorOperand& dst);
    void (*matmulF64)(const TensorOperand& a, const TensorOperand& b,
                      const TensorOperand& dst);
    void (*matmulI32)(const TensorOperand& a, const TensorOperand& b,
                      const TensorOperand& dst);
    void (*matmulI64)(const TensorOperand& a, const TensorOperand& b,
                      const TensorOperand& dst);

    //--- phase 2 family ---
    //gather: take-per-position along axis (indices rank-1 int32,
    //length == non-axis count; rank-1 src degenerates to fancy
    //indexing). Out-of-range raises per the cell-level error mechanism.
    void (*gatherF32)(const TensorOperand& src, int32_t axis,
                      const TensorOperand& indices,
                      const TensorOperand& dst);
    void (*gatherF64)(const TensorOperand& src, int32_t axis,
                      const TensorOperand& indices,
                      const TensorOperand& dst);
    void (*gatherI32)(const TensorOperand& src, int32_t axis,
                      const TensorOperand& indices,
                      const TensorOperand& dst);
    void (*gatherI64)(const TensorOperand& src, int32_t axis,
                      const TensorOperand& indices,
                      const TensorOperand& dst);
    //argmax/argmin along axis: int32 winner indices, first-wins tie.
    void (*argmaxI32)(bool isMax, const TensorOperand& src, int32_t axis,
                      const TensorOperand& dst);
    //eq: broadcast elementwise same-kind compare, int32 0/1 out.
    void (*eqF32)(const TensorOperand& a, const TensorOperand& b,
                  const TensorOperand& dst);
    void (*eqF64)(const TensorOperand& a, const TensorOperand& b,
                  const TensorOperand& dst);
    void (*eqI32)(const TensorOperand& a, const TensorOperand& b,
                  const TensorOperand& dst);
    void (*eqI64)(const TensorOperand& a, const TensorOperand& b,
                  const TensorOperand& dst);
    //oneHot: int32 labels → float32 [len, n] rows.
    void (*oneHotI32)(const TensorOperand& labels, int32_t n,
                      const TensorOperand& dst);
    //uniform: fills dst with low + (high-low)*u. mtState/position carry
    //the snapshotRngForDevice snapshot (the host already discarded n);
    //CPU draws via the host PRNG stream instead and may ignore them.
    void (*uniformF32)(const TensorOperand& dst, float low, float high,
                       const uint32_t* mtState, uint32_t position);
    void (*uniformF64)(const TensorOperand& dst, double low, double high,
                       const uint32_t* mtState, uint32_t position);

    //--- data generation & movement family ---
    void (*fillF32)(const TensorOperand& dst, double value);
    void (*fillF64)(const TensorOperand& dst, double value);
    void (*fillI32)(const TensorOperand& dst, int64_t value);
    void (*fillI64)(const TensorOperand& dst, int64_t value);
    //arange: dst[i] = start + i*step (half-open contract checked by the
    //dispatcher before minting).
    void (*arangeI32)(const TensorOperand& dst, int64_t start,
                      int64_t step);
    void (*arangeI64)(const TensorOperand& dst, int64_t start,
                      int64_t step);
    void (*arangeF32)(const TensorOperand& dst, double start,
                      double step);
    void (*arangeF64)(const TensorOperand& dst, double start,
                      double step);
    //concat along axis (dims equal off-axis, sum on axis).
    void (*concatF32)(const TensorOperand& a, const TensorOperand& b,
                      int32_t axis, const TensorOperand& dst);
    void (*concatF64)(const TensorOperand& a, const TensorOperand& b,
                      int32_t axis, const TensorOperand& dst);
    void (*concatI32)(const TensorOperand& a, const TensorOperand& b,
                      int32_t axis, const TensorOperand& dst);
    void (*concatI64)(const TensorOperand& a, const TensorOperand& b,
                      int32_t axis, const TensorOperand& dst);
    //copy: elementwise materialization (same shape, dense dst) — the
    //shared primitive of view materialization, toDevice/toHost and
    //serialization staging.
    void (*copyF32)(const TensorOperand& src, const TensorOperand& dst);
    void (*copyF64)(const TensorOperand& src, const TensorOperand& dst);
    void (*copyI32)(const TensorOperand& src, const TensorOperand& dst);
    void (*copyI64)(const TensorOperand& src, const TensorOperand& dst);
};

//The active table: NLANG_TENSOR_BACKEND names the backend (cpu default;
//mirror = the test backend of the P2 residency work; cuda = the device
//plugin). Selection details live in the backend registry (T3.8).
const TensorOps& ActiveTensorOps();

//Test seam: register a whole table by name (the cpu table registers at
//static init; backends defined outside this TU — the cuda plugin, the
//P2 mirror registration — register through this hook). Name must stay
//alive.
void RegisterTensorOps(const char* name, const TensorOps* ops);

} // namespace tensor
