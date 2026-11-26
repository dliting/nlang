/*---
    cuda_kernels.h — the host-side launch entry points of the CUDA
    backend's device kernels (P3 T3.3-T3.7). The TensorOps slot
    implementations in cuda_backend.cpp call these; each launcher
    builds the operand geometry and enqueues on the compute stream of
    its operands' device (device ABI v3's operand-driven routing: the
    LaunchView's device entry — folded from the view's reserved[1] —
    pins the thread's context and selects the stream row; the registry
    has agreed the operands onto one device and refused device-foreign
    operands inside a Push scope).

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
#include "backend_descriptor.h"

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
    //The owning device (device ABI v3, operand-driven routing): the
    //launcher pins this device for the whole op and enqueues on its
    //current stream. The registry agreed the operands on one device
    //before the table call, so every view of one launch agrees.
    int32_t device = 0;
    int32_t dims[kMaxRank] = {};
    int32_t strides[kMaxRank] = {};
};

//True when the CUDA context is initialized (lazy — spec D4).
bool CudaReady();

//--- the stream pool (v0.14.9: the multi-stream orchestration; device
//ABI v2: per-device pools) ----------------------------------------------
//Create/destroy pool streams; Push/Pop swap the device's compute stream
//(the launchers read the swapped-in stream); Join makes the device's
//current stream wait for a chunk stream's completion (event +
//stream-wait). A handle encodes (device, slot) — backend_descriptor.h's
//contract — and slot 0 is reserved per device (that device's main
//stream), so pooled slots run 1..kNativeMaxStreams-1. Create returns -1
//when the device is unusable or its pool is full (tensor.streamCreate's
//entry raises by name).
int CudaStreamCreate(int device);
void CudaStreamDestroy(int handle);
void CudaStreamPush(int handle);
void CudaStreamPop();
void CudaStreamJoin(int handle);

//RAII device-context pin AND device-row selection (device ABI v3):
//CUDA calls act on the thread's CURRENT device, so every entry point
//handed a device selector pins it for the call and restores the
//thread's previous device on the way out. The pin also selects the
//per-device row the stream/staging/error primitives read (the current
//compute stream, the staging stream and event, the cell-error flags),
//so one pin makes the whole entry body device-correct — launchers pin
//from the primary LaunchView's device, the hooks from their device
//argument. This guard is the only spelling that changes the thread's
//device or the selected row; callers run it only after the device was
//accepted, so the ids they pin address live contexts.
class SetDeviceForCall {
public:
    explicit SetDeviceForCall(int device);
    ~SetDeviceForCall();
    SetDeviceForCall(const SetDeviceForCall&) = delete;
    SetDeviceForCall& operator=(const SetDeviceForCall&) = delete;

private:
    int m_savedDevice = 0;
    int m_savedRow = 0;
};

//The currently selected device's compute stream (the row the last
//SetDeviceForCall chose; device ABI v3's operand-driven routing).
//Entry bodies read the stream through this — never a row-0 alias.
cudaStream_t CurrentStream();
//The selected row's staging stream / cell-error flags (the same
//selection; the bulk-transfer and error-drain paths read these — the
//handshake events those paths pair with are per-call, created and
//destroyed inside the transfer).
cudaStream_t CurrentStagingStream();
int32_t* CurrentErrorFlags();

//True when srcDevice can reach dstDevice over the peer fabric (the
//row-init capability matrix; CudaDeviceAccepted gates both ids).
bool CudaPeerReady(int32_t srcDevice, int32_t dstDevice);
//The two cross-device copy paths (the copyD2D entry's body, spec §6),
//each blocking — the payload is resident on the target when the call
//returns (the transfer's observation contract). Both order after the
//SOURCE device's current stream's work; the caller (the backend's
//status wrapper) attributes the returned code to a row.
//Peer: cudaMemcpyPeerAsync enqueued on the source's current stream,
//handing off into the destination's current stream through an event.
cudaError_t CopyPeer(int32_t dstDevice, void* dst, int32_t srcDevice,
                     const void* src, size_t bytes);
//Staging: a pinned host bounce (thread-local, grown by doubling on
//demand and kept for the thread — the same per-thread contract as the
//row pin), D2H on the source row then H2D on the destination row.
cudaError_t CopyStaged(int32_t dstDevice, void* dst, int32_t srcDevice,
                       const void* src, size_t bytes);

//Lazy init: the device table + device 0's main stream + the error
//flags. On failure a non-null error receives the message (the caller
//raises the NLang exception with it); a null error asks for the status
//only (the pool primitives probe without a reason string).
bool CudaLazyInit(std::string* error);

//The single authority on whether this plugin can serve `device`: lazy
//init included (no context = no device is servable), then the probed
//census (clamped to the descriptor's kNativeMaxDevices). Device
//selectors arriving from the host gate on this before pinning;
//decoded stream handles are bounded by the stream tables (a memory-
//safety check, a different concern).
bool CudaDeviceAccepted(int device);

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
//scatterAdd (the accumulate twin): SYNC — per position,
//dst.flat[index] += v with atomicAdd (duplicate indices SUM);
//clear-launch-sync-read drains the per-cell out-of-range flag.
cudaError_t LaunchScatterAdd(const LaunchView& a, const LaunchView& indices,
                             const LaunchView& values, const LaunchView& dst,
                             bool valuesScalar, double sv, int64_t isv,
                             int32_t* errorOut);
cudaError_t LaunchMatmul(const LaunchView& a, const LaunchView& b,
                         const LaunchView& dst, int elemKindCode);
//assignInto (the slice-assignment write half): ASYNC — one thread per
//view cell, each write routed through the view's strides (a slice
//view writes through to its base). The values ride the pool scratch
//H2D; the writeability gate rode the host check.
cudaError_t LaunchAssignInto(const LaunchView& view,
                             const void* values, int elemKindCode);
//The scalar arm: the constant rides the launch parameters (no values
//staging — the array arm's H2D row would ship count cells for a
//one-number broadcast).
cudaError_t LaunchAssignIntoScalar(const LaunchView& view, double sv,
                                   int64_t isv, int elemKindCode);
//sort/argsort (stable ascending per axis line, NaN to the end): ASYNC
//— one thread per line, an in-thread insertion sort. Lines above
//kSortMaxLine (declared in TensorOps.h — both dispatchers read it)
//must NOT reach this launcher (the kernel's fixed in-thread buffer);
//the dispatcher routes them to the host walk.
cudaError_t LaunchSort(bool withIndices, const LaunchView& src,
                       int axis, const LaunchView& dst);
//takeAlongAxis (the argsort consumer): ASYNC — the index bounds ride
//the dispatcher's host-side validation over the dense int32 mirror;
//the kernel is a pure strided copy (the mirror lands H2D via the
//pool scratch).
cudaError_t LaunchTakeAlong(const LaunchView& src, int axis,
                            const int32_t* indices,
                            const LaunchView& dst);
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
