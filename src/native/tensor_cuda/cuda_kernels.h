/*---
    cuda_kernels.h — the host-side launch entry points of the CUDA
    backend's device kernels (P3 T3.3-T3.7). The TensorOps slot
    implementations in cuda_backend.cpp call these; each launcher
    builds the operand geometry and enqueues on the single stream.

    Differentiated sync discipline (spec 2026-11-16 §3): launchers
    whose kernels raise no cell errors are asynchronous — they return
    cudaGetLastError() (launch-config errors attribute at this op;
    execution faults surface at the next host boundary, which is any
    CopyBytes transfer or a reduction's blocking partials read):
      unary / clamp / compare / compare-scalar / matmul / reduceAxis /
      argExtreme / eq / where / fill / arange / copy / concat / uniform
    The error-capable families keep the per-op clear-launch-sync-read
    drain and raise at this op:
      int binary / int binary-scalar  -> DivByZero (per-cell divisor)
      gather                          -> OutOfBounds (negative index)
      oneHot                          -> OutOfBounds (label range)
      scatter                         -> OutOfBounds (index, both bounds)
    errorOut carries the offending cell index for the caller's named
    NLang raise; every launcher returns a cudaError for the CHECK-style
    wrapper.
---*/
#pragma once

#include <nlang/vm/NativeHost.h>

#include "../tensor/TensorBackends.h"

#include <cuda_runtime.h>

#include <cstdint>
#include <string>

using tensor::CompareOp;

namespace tensor_cuda {

//The operand geometry for a launch: the pinned view's fields folded
//into a flat struct (the device kernels read the inline arrays via
//the by-value DevView parameter — constant memory, no indirection).
constexpr int kMaxRank = 64;

struct LaunchView {
    int32_t rank = 0;
    int64_t elemCount = 0;
    int64_t offset = 0;
    void* data = nullptr;
    uint8_t elemKind = 0;
    int32_t dims[kMaxRank] = {};
    int32_t strides[kMaxRank] = {};
};

//True when the CUDA context is initialized (lazy — spec D4).
bool CudaReady();

//Lazy init: device 0 + the stream + the error flags. Returns false + a
//message on failure (the caller raises the NLang exception with it).
bool CudaLazyInit(std::string* error);

//The byte staging (toDevice/toHost/serializer): rides the stream.
cudaError_t CopyBytes(void* dst, const void* src, size_t bytes,
                      bool toDevice);

//The full-stream observation point (the sync hook's body).
cudaError_t SyncStream();

//The device-record hooks (the VM routes device records through them —
//spec D3): pool alloc/zero/free + the byte transfers. alloc returns
//nullptr on failure (the caller raises by name); the rest are void
//(failures surface at the next sync/transfer through the CHECK).
void* PoolAlloc(size_t bytes, cudaStream_t stream);
void PoolZero(void* ptr, size_t bytes);
void PoolFree(void* ptr, cudaStream_t stream);
//The compute-stream wrappers (the backend TU's hooks; the stream
//is the kernels TU's compute stream).
void* PoolAllocOnStream(size_t bytes);
void PoolFreeOnStream(void* ptr);

//--- kernel launches (each returns cudaSuccess, or the error code the
//CHECK-style wrapper raises; errorOut carries the offending cell index
//for the cell-level raise mechanism) ---

cudaError_t LaunchUnary(int op, const LaunchView& src,
                        const LaunchView& dst);
cudaError_t LaunchBinary(int op, const LaunchView& a,
                         const LaunchView& b, const LaunchView& dst,
                         int32_t* errorOut);
cudaError_t LaunchBinaryScalar(int op, const LaunchView& a,
                               double scalar, bool scalarLeft,
                               const LaunchView& dst, int32_t* errorOut);
cudaError_t LaunchClamp(bool isMax, const LaunchView& src, double bound,
                        int64_t iBound, const LaunchView& dst);
cudaError_t LaunchSumAll(const LaunchView& src, double* out,
                         int64_t* outI, bool isInt);
cudaError_t LaunchDot(const LaunchView& a, const LaunchView& b,
                      double* out, int64_t* outI, bool isInt);
cudaError_t LaunchExtreme(bool isMax, const LaunchView& src,
                          double* outF, int64_t* outI, bool isInt);
cudaError_t LaunchReduceAxis(int op, const LaunchView& src, int axis,
                             const LaunchView& dst);
//The compare family (six ops, array + scalar arms): asynchronous — no
//cell error flags; IEEE ordering on the device matches the CPU walk.
cudaError_t LaunchCompare(CompareOp op, const LaunchView& a,
                          const LaunchView& b, const LaunchView& dst);
cudaError_t LaunchCompareScalar(CompareOp op, const LaunchView& a,
                                double scalar, int64_t iScalar,
                                const LaunchView& dst);
//where (three-way broadcast, scalar arms): asynchronous — no cell
//error flags.
cudaError_t LaunchWhere(const LaunchView& mask, const LaunchView& x,
                        const LaunchView& y, const LaunchView& dst,
                        int elemKindCode, bool maskLong, bool xScalar,
                        bool yScalar, double sx, double sy, int64_t isx,
                        int64_t isy);
//scatter (rank-1): SYNC-GROUP — the per-cell out-of-range flag drains
//per op (errorOut carries the offending index ordinal).
cudaError_t LaunchScatter(const LaunchView& a, const LaunchView& indices,
                          const LaunchView& values, const LaunchView& dst,
                          bool valuesScalar, double sv, int64_t isv,
                          int32_t* errorOut);
cudaError_t LaunchMatmul(const LaunchView& a, const LaunchView& b,
                         const LaunchView& dst, int elemKindCode);
//gather (take-per-position, all kinds, all axes): ASYNC — the index
//bounds ride the dispatcher's host-side validation over the staged
//indices; the kernel is a pure strided copy.
cudaError_t LaunchGather(const LaunchView& src, int axis,
                         const LaunchView& indices,
                         const LaunchView& dst);
cudaError_t LaunchArgExtreme(bool isMax, const LaunchView& src,
                             int axis, const LaunchView& dst);
cudaError_t LaunchEq(const LaunchView& a, const LaunchView& b,
                     const LaunchView& dst, int elemKindCode);
cudaError_t LaunchOneHot(const LaunchView& labels, int32_t n,
                         const LaunchView& dst, int32_t* errorOut);
cudaError_t LaunchUniform(const LaunchView& dst, double low,
                          double high, const uint32_t* mt624,
                          uint32_t position, bool isDouble);
cudaError_t LaunchFill(const LaunchView& dst, double value,
                       int64_t iValue, int elemKindCode);
cudaError_t LaunchArange(const LaunchView& dst, double start,
                         double step, bool isInt);
cudaError_t LaunchConcat(const LaunchView& a, const LaunchView& b,
                         int axis, const LaunchView& dst,
                         int elemKindCode);
cudaError_t LaunchCopy(const LaunchView& src, const LaunchView& dst,
                       int elemKindCode);

} // namespace tensor_cuda
