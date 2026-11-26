/*---
    cuda_kernels.cu — the CUDA backend's device kernels and launch
    entry points (P3 T3.3-T3.7).

    Design: the operand geometry rides a DevView passed BY VALUE as a
    kernel parameter (constant memory — the dims/strides inline arrays
    read directly from the parameter storage, no pointer indirection).
    The launcher folds the NativeArrayView into a DevView (dims/
    strides/offset/data/elemCount); the kernels fold the flat ordinal
    through the recorded strides (the strided-view discipline — views
    ride their layout without materializing).

    Differentiated sync discipline (spec 2026-11-16 §3): launchers
    whose kernels raise no cell errors are asynchronous — they enqueue
    on the device's current compute stream (row 0 of the per-device
    table until M-G2 routes kernels by operand device) and return
    cudaGetLastError() (launch-config
    errors attribute at this op; execution faults surface at the next
    host boundary). The four error-capable launcher families (int
    binary, int binary-scalar, gather, oneHot) keep the per-op
    clear-launch-sync-read drain so the cell-level raise stays at this
    op. The reduction family's blocking partials read is its host
    boundary. errorOut carries the offending cell index for the
    caller's named NLang raise; the launcher returns the cudaError for
    the CHECK-style wrapper.

    This TU compiles -fmad=false (spec 第8轮N2): the uniform mapping's
    low + span * u must not contract into FMA — the host's separated
    multiply-add rounds differently. Reductions ride block partials +
    the host finalize (the double-accumulator contract); argmax/extreme
    ride the (value, index) comparator with the first-wins tie rule.
---*/
#include "cuda_kernels.h"
#include "cuda_mt19937.cuh"
#include "../tensor/TensorOps.h"

#include <cuda_runtime.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tensor_cuda {

namespace {

constexpr int kThreads = 256;
constexpr uint32_t kErrNone = 0xFFFFFFFFu;
constexpr uint32_t kErrDivZero = 0;
constexpr uint32_t kErrOOB = 1;

//The device operand geometry: by-value kernel parameter (constant
//memory). The inline arrays read directly from the parameter storage.
//The rank arrays carry the full kMaxRank: the strided kernels fold a
//rank up to 64 (FlatAddress/IsDense walk every axis), and a truncated
//DevView would read the parameter storage past the fill.
struct DevView {
    int32_t rank;
    int64_t elemCount;
    int64_t offset;
    void* data;
    int32_t elemKind;
    int32_t dims[kMaxRank];
    int32_t strides[kMaxRank];
};

DevView ToDevView(const LaunchView& v) {
    DevView d;
    d.rank = v.rank;
    d.elemCount = v.elemCount;
    d.offset = v.offset;
    d.data = v.data;
    d.elemKind = v.elemKind;
    for (int32_t i = 0; i < v.rank && i < kMaxRank; ++i) {
        d.dims[i] = v.dims[i];
        d.strides[i] = v.strides[i];
    }
    return d;
}

//The row-major fold of the flat ordinal through the recorded strides
//plus the view's offset (element units).
__device__ int64_t FlatAddress(const DevView& v, int64_t flat) {
    int64_t addr = v.offset;
    int64_t rem = flat;
    for (int32_t d = v.rank - 1; d >= 0; --d) {
        const int64_t dim = v.dims[d];
        const int64_t coord = dim > 0 ? rem % dim : 0;
        rem /= (dim > 0 ? dim : 1);
        addr += coord * v.strides[d];
    }
    return addr;
}

//True when the view's strides equal the dense row-major strides of its
//dims (any offset allowed — the offset is folded separately). Mirrors the
//CPU IsContiguous semantics; the reduction launchers branch on it: dense
//views take the flat dense kernel (zero cost), strided views take the
//strided-aware kernel (FlatAddress per element).
__host__ __device__ bool IsDense(const DevView& v) {
    int64_t stride = 1;
    for (int32_t d = v.rank - 1; d >= 0; --d) {
        if (v.strides[d] != stride)
            return false;
        stride *= (v.dims[d] > 0 ? v.dims[d] : 1);
    }
    return true;
}

//The per-device compute streams (device ABI v2: one row per device;
//row 0 = device 0's main stream). Device ABI v3 routes by the
//operands' device: the entry's SetDeviceForCall pin selects the row
//whose stream, staging handles and cell-error flags ([0] = kind,
//[1] = cell) the entry body reads (CurrentStream() and friends).
//Devices at or above the shared census bound (the descriptor's
//kNativeMaxDevices) are rejected at selection — a plugin limit, not
//a CUDA one; no host today has 16.
cudaStream_t g_currentStream[tensor::kNativeMaxDevices] = {};

//The selected row index: the device the last SetDeviceForCall pinned,
//0 until the first pin (every boot path — the lazy init, the census —
//names device 0 first). Thread-local by contract: tensor natives run
//on the VM's blocking channel without the VM lock, so two carriers
//can be inside entries at once — each pin selects its own row, and
//the RAII save/restore nests per thread. Only the row tables (the
//streams, staging and cell-error flags) are shared across threads;
//CUDA's stream and memory-pool APIs serialize those.
thread_local int g_launchDevice = 0;

//The per-device state table (M-G2): the one-off singletons joined a
//row per device, addressed by the selector every device-selected
//entry point pins. Rows init lazily — the first call accepting device
//d runs CudaDeviceReady on it (the context touch, the streams, the
//cell-error flags, the default-pool attributes and the row's peer
//enablement) before that device serves anything.
struct CudaDeviceState {
    cudaStream_t stagingStream = nullptr; //the bulk-transfer overlap
    int32_t* error = nullptr;             //the kernel cell-error flags
    cudaMemPool_t memPool = nullptr;      //the device's default pool
    bool peerEnabled[tensor::kNativeMaxDevices] = {}; //the peer row
    bool ready = false;                   //this row's lazy init done
};
CudaDeviceState g_dev[tensor::kNativeMaxDevices] = {};

bool g_ready = false;
int g_deviceCount = 0;   //the lazy-init probe's result (0 before init)

//--- the stream pool (v0.14.9: the multi-stream orchestration; device
//ABI v2: one pool per device) ------------------------------------------
//K independent chunk streams for embarrassingly parallel workloads
//(Monte Carlo batches, ensembles). Push/Pop swap the device's current
//stream (the stack saves (device, stream) pairs — nestable); Join makes
//the device's current stream wait for a chunk stream's completion
//(event record + stream-wait). Pool slots are handed out round-robin
//per device and reused; slot 0 is each device's main stream (device 0's
//is created at lazy init), so pooled slots start at 1. The pool width
//and the push-nesting bound are the descriptor's shared constants —
//the registry's own stack is bounded by the same number.
cudaStream_t g_streamPool[tensor::kNativeMaxDevices][tensor::kNativeMaxStreams] = {};
bool g_streamSlotUsed[tensor::kNativeMaxDevices][tensor::kNativeMaxStreams] = {};
int g_streamNextSlot[tensor::kNativeMaxDevices] = {};
struct SavedStream {
    cudaStream_t stream = nullptr;
    int device = 0;
};
SavedStream g_streamStack[tensor::kNativeMaxStreamPushDepth] = {};
int g_streamStackDepth = 0;



} // namespace

//The selected row's handles (the header's contract: the entry's
//SetDeviceForCall pin chose the row; the stream, staging and error
//primitives read these). Defined outside the anonymous namespace so
//the header's declarations resolve to these definitions.
cudaStream_t CurrentStream() { return g_currentStream[g_launchDevice]; }
cudaStream_t CurrentStagingStream() { return g_dev[g_launchDevice].stagingStream; }
int32_t* CurrentErrorFlags() { return g_dev[g_launchDevice].error; }

//The RAII context pin AND row selection (the header's contract: CUDA
//calls act on the thread's current device, so every device-selected
//entry point pins it here, selects the row the entry body reads, and
//restores both on the way out). Callers run it only after
//CudaLazyInit succeeded and CudaDeviceAccepted gated the id, so the
//ids they pin address real contexts and rows inside the census;
//failures here are the sticky-error discipline's job.
SetDeviceForCall::SetDeviceForCall(int device) {
    cudaGetDevice(&m_savedDevice);
    m_savedRow = g_launchDevice;
    cudaSetDevice(device);
    g_launchDevice = device;
}

SetDeviceForCall::~SetDeviceForCall() {
    cudaSetDevice(m_savedDevice);
    g_launchDevice = m_savedRow;
}

int CudaStreamCreate(int device) {
    if (!CudaDeviceAccepted(device))
        return -1;   //no context, or outside the probed census:
                     //tensor.streamCreate's entry raises by name
    SetDeviceForCall pin(device);
    for (int probe = 0; probe < tensor::kNativeMaxStreams; ++probe) {
        const int slot =
            (g_streamNextSlot[device] + probe) % tensor::kNativeMaxStreams;
        if (slot == 0 || g_streamSlotUsed[device][slot])
            continue;   //slot 0 is this device's main stream, not pool
        g_streamNextSlot[device] = (slot + 1) % tensor::kNativeMaxStreams;
        if (cudaStreamCreate(&g_streamPool[device][slot]) != cudaSuccess)
            return -1;
        g_streamSlotUsed[device][slot] = true;
        return tensor::EncodeStreamHandle(device, slot);
    }
    return -1;   //this device's pool is exhausted
}

void CudaStreamDestroy(int handle) {
    //handle <= 0 covers the main-stream sentinel (0) and the -1 an
    //unusable create returned: nothing pooled to destroy either way.
    if (handle <= 0)
        return;
    const int device = tensor::DecodeStreamDevice(handle);
    const int slot = tensor::DecodeStreamSlot(handle);
    if (device >= tensor::kNativeMaxDevices || slot == 0 ||
        !g_streamSlotUsed[device][slot])
        return;
    SetDeviceForCall pin(device);
    cudaStreamDestroy(g_streamPool[device][slot]);
    g_streamPool[device][slot] = nullptr;
    g_streamSlotUsed[device][slot] = false;
}

void CudaStreamPush(int handle) {
    //One frame per call, in lockstep with the registry's push stack:
    //the registry records an entry for EVERY streamPush (cpu/mirror
    //included), so a save skipped here would desync the stacks and
    //the matching Pop would restore the wrong stream. A handle that
    //names no live pool slot — the main-stream sentinel, a -1, a
    //stale or foreign handle — still saves its frame (no swap), so
    //every Pop restores exactly what its Push saved.
    if (g_streamStackDepth >= tensor::kNativeMaxStreamPushDepth)
        return;   //unreachable: the registry raises at the same cap
                  //before calling (the shared bound keeps the two
                  //stacks from disagreeing about where it sits)
    //Init before the frame captures the stream: a first-ever Push
    //before any op would otherwise save the pre-init nullptr, and the
    //matching Pop after a mid-scope lazy init would restore it —
    //later launches would then ride the legacy default stream
    //implicit-synced against the created one (a silent overlap loss,
    //and the created stream orphaned). Idempotent, and the failure
    //changes nothing: without a device the frame and the live slot
    //both stay nullptr exactly as before, and the save below stays
    //unconditional so the registry lockstep holds either way.
    (void)CudaLazyInit(nullptr);
    bool pooled = false;
    int device = 0, slot = 0;
    if (handle > 0) {
        const int d = tensor::DecodeStreamDevice(handle);
        slot = tensor::DecodeStreamSlot(handle);
        if (d >= 0 && d < tensor::kNativeMaxDevices && slot != 0 &&
            g_streamSlotUsed[d][slot]) {
            device = d;
            pooled = true;
        }
    }
    SetDeviceForCall pin(device);
    g_streamStack[g_streamStackDepth++] = {g_currentStream[device], device};
    if (pooled)
        g_currentStream[device] = g_streamPool[device][slot];
}

void CudaStreamPop() {
    if (g_streamStackDepth == 0)
        return;   //the imbalance surfaces as the language-level
                  //unbalanced-Pop rejection (the caller's gate)
    const SavedStream saved = g_streamStack[--g_streamStackDepth];
    SetDeviceForCall pin(saved.device);
    g_currentStream[saved.device] = saved.stream;
}

void CudaStreamJoin(int handle) {
    if (handle <= 0)
        return;   //the main stream joins itself: nothing to wait for
    const int device = tensor::DecodeStreamDevice(handle);
    const int slot = tensor::DecodeStreamSlot(handle);
    if (device >= tensor::kNativeMaxDevices || !g_streamSlotUsed[device][slot])
        return;
    SetDeviceForCall pin(device);
    //The device's current stream waits for the chunk stream: an event
    //record on the chunk stream + a stream-wait on the current stream
    //(the join is stream-ordered — everything the chunk enqueued is
    //visible to the current stream's subsequent ops).
    cudaEvent_t done = nullptr;
    if (cudaEventCreate(&done) != cudaSuccess)
        return;
    cudaEventRecord(done, g_streamPool[device][slot]);
    cudaStreamWaitEvent(g_currentStream[device], done, 0);
    cudaEventDestroy(done);
}

bool CudaReady() { return g_ready; }

//The per-device row init (M-G2): the first call accepting device d
//touches the context (cudaFree(0) — the documented create idiom),
//builds the row (main stream, cell-error flags, staging stream/event
//pair, default-pool attributes) and enables the row's peer access.
//Idempotent in both directions: an already-ready row answers at one
//load, and a failed attempt leaves ready unset so the next accepted
//call retries — the retry RESUMES the row, never duplicating the
//resources a partial attempt already built (each step builds only
//what is still null, so no handle or allocation is overwritten and
//no leak accrues per retry). The entry/error-flag clears keep a
//retry or a transient fault (an OOM at flags allocation) from
//leaving a sticky error that a later operation's status fold would
//mis-attribute to itself. The body runs pinned; failures always name
//their reason.
static bool CudaDeviceReady(int device, std::string& error) {
    if (g_dev[device].ready)
        return true;
    SetDeviceForCall pin(device);
    (void)cudaGetLastError();   //a previous failed attempt's residue
    if (cudaFree(0) != cudaSuccess) {
        (void)cudaGetLastError(); //the reason rides `error`; the flag isn't ours
        error = "cudaFree(0) failed on device " + std::to_string(device);
        return false;
    }
    //The row's main stream — handle 0 names it everywhere; the launchers
    //and transfers reach it through the pin-selected CurrentStream().
    if (g_currentStream[device] == nullptr
        && cudaStreamCreate(&g_currentStream[device]) != cudaSuccess) {
        (void)cudaGetLastError(); //the reason rides `error`; the flag isn't ours
        error = "cudaStreamCreate failed on device "
            + std::to_string(device);
        return false;
    }
    if (g_dev[device].error == nullptr) {
        if (cudaMallocAsync((void**)&g_dev[device].error,
            2 * sizeof(int32_t), g_currentStream[device])
            != cudaSuccess) {
            (void)cudaGetLastError(); //the reason rides `error`; the flag isn't ours
            error = "device error-flag allocation failed on device "
                + std::to_string(device);
            return false;
        }
        const uint32_t none[2] = {kErrNone, 0};
        cudaMemcpy(g_dev[device].error, none, 2 * sizeof(int32_t),
            cudaMemcpyHostToDevice);
    }
    //The staging stream (bulk transfers overlap the compute stream's
    //already-enqueued kernels). The handshake events it pairs with are
    //per-call (the CopyBytes/CopyPeer pattern, like the stream pool's
    //join events), so the row carries no shared event state.
    if (g_dev[device].stagingStream == nullptr
        && cudaStreamCreate(&g_dev[device].stagingStream)
        != cudaSuccess) {
        (void)cudaGetLastError(); //the reason rides `error`; the flag isn't ours
        error = "cudaStreamCreate failed (staging) on device "
            + std::to_string(device);
        return false;
    }
    //The default mempool (the stream-ordered allocations land in it;
    //the handle is kept for the row's introspection). Its default
    //release threshold is zero: every synchronization releases unused
    //pool memory back to the OS, which would re-pay the allocation in
    //sync-heavy loops. UINT64_MAX makes the allocation cost
    //independent of size (NVIDIA's guidance for tensor workloads).
    if (cudaDeviceGetDefaultMemPool(&g_dev[device].memPool, device)
        == cudaSuccess) {
        unsigned long long threshold = UINT64_MAX;
        cudaMemPoolSetAttribute(g_dev[device].memPool,
            cudaMemPoolAttrReleaseThreshold, &threshold);
    }
    //The row's peer matrix (spec §6): query each ordered pair from
    //this device and enable what the hardware supports. The enable is
    //one-directional, so the reverse direction is the peer's row's
    //own job; a refusal (consumer boards ship P2P-disabled pairs) or
    //an AlreadyEnabled repeat clears the error flag and stays silent
    //— the staging-routed transfers serve those pairs anyway.
    for (int peer = 0; peer < g_deviceCount; ++peer) {
        if (peer == device)
            continue;
        int can = 0;
        if (cudaDeviceCanAccessPeer(&can, device, peer)
            != cudaSuccess) {
            (void)cudaGetLastError(); //a probe fault is not ours either
            continue;
        }
        if (!can)
            continue;
        if (cudaDeviceEnablePeerAccess(peer, 0) == cudaSuccess)
            g_dev[device].peerEnabled[peer] = true;
        (void)cudaGetLastError();   //the refusal flag is not ours to keep
    }
    //The best-effort arms above (the mempool attributes, the flags'
    //initial copy) may leave an error flag set; a ready row must not
    //hand one to the next operation's status fold.
    (void)cudaGetLastError();
    g_dev[device].ready = true;
    return true;
}

//The init body: always names its failure reason — the public wrapper
//below owns the null-error contract, so the arms never branch on it.
static bool CudaLazyInitReason(std::string& error) {
    //The device census first (device ABI v2): the visible devices
    //become the selector range the plugin accepts; devices past the
    //shared census bound (the descriptor's kNativeMaxDevices) stay
    //unreachable — a plugin limit, not a CUDA one. The census is the
    //only process-wide part (device enumeration); every row — device
    //0's included — builds itself lazily through CudaDeviceReady.
    int deviceCount = 0;
    if (cudaGetDeviceCount(&deviceCount) != cudaSuccess || deviceCount < 1) {
        error = "no CUDA device present (cudaGetDeviceCount)";
        return false;
    }
    g_deviceCount = deviceCount < tensor::kNativeMaxDevices
        ? deviceCount : tensor::kNativeMaxDevices;
    if (cudaSetDevice(0) != cudaSuccess) {
        error = "cudaSetDevice(0) failed";
        return false;
    }
    if (!CudaDeviceReady(0, error))
        return false;
    g_ready = true;
    return true;
}

//The public wrapper owns the null contract (PoolAlloc probes with a
//null error): the body always names a reason, the wrapper moves it
//out only when the caller wants one.
bool CudaLazyInit(std::string* error) {
    if (g_ready)
        return true;
    std::string reason;
    const bool ok = CudaLazyInitReason(reason);
    if (!ok && error != nullptr)
        *error = reason;
    return ok;
}

//The census gate (the header's contract): no context = no device is
//servable; otherwise the probed census decides, and an in-census
//device is only accepted once its row has initialized (the first
//accepted call on it runs the per-device lazy init; a row that
//cannot build its context stays unserviceable — reported through the
//same "unknown device" fault the callers fold, the effect the user
//sees either way). Device selectors arriving from the host gate here
//before any pin; decoded stream handles check the stream-table bound
//instead (memory safety — a different concern, see the stream
//entries).
bool CudaDeviceAccepted(int device) {
    std::string initError;
    if (!CudaLazyInit(&initError))
        return false;
    if (device < 0 || device >= g_deviceCount)
        return false;
    std::string rowError;
    return CudaDeviceReady(device, rowError);
}

//The bulk-transfer threshold: blocks at or above it route through the
//staging stream (the copy overlaps the compute stream's enqueued
//kernels); element-level transfers stay on the compute stream (the
//observation-point contract — they read the just-written cells).
constexpr size_t kStagingOverlapBytes = 64 * 1024;

cudaError_t CopyBytes(void* dst, const void* src, size_t bytes,
                      bool toDevice) {
    if (bytes < kStagingOverlapBytes) {
        cudaMemcpyAsync(dst, src, bytes,
            toDevice ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost,
            CurrentStream());
        return cudaStreamSynchronize(CurrentStream());
    }
    //The bulk path: the staging stream waits for the compute stream's
    //prior work (the event recorded on the compute stream after the
    //last enqueued kernel — an event record is itself stream-ordered),
    //runs the copy concurrently, then hands the result back: the
    //compute stream waits on the staging event before any consumer
    //kernel of the subsequent enqueue runs. The hook stays blocking —
    //the staging stream syncs before the status returns (the
    //observable-point contract). The handshake event is per-call (the
    //join-event pattern in the stream pool): entries can run
    //concurrently, and a row-shared event would interleave two
    //transfers' handoffs; an event costs microseconds against a bulk
    //copy, and destroying it once both streams have consumed its
    //records is safe (the wait keeps the fence without the handle).
    cudaEvent_t handoff = nullptr;
    if (cudaEventCreate(&handoff) != cudaSuccess) {
        (void)cudaGetLastError(); //fall back: a plain blocking copy
        cudaMemcpyAsync(dst, src, bytes,
            toDevice ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost,
            CurrentStream());
        return cudaStreamSynchronize(CurrentStream());
    }
    cudaEventRecord(handoff, CurrentStream());
    cudaStreamWaitEvent(CurrentStagingStream(), handoff, 0);
    cudaMemcpyAsync(dst, src, bytes,
        toDevice ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost,
        CurrentStagingStream());
    cudaEventRecord(handoff, CurrentStagingStream());
    cudaStreamWaitEvent(CurrentStream(), handoff, 0);
    const cudaError_t status = cudaStreamSynchronize(CurrentStagingStream());
    cudaEventDestroy(handoff);
    return status;
}

//The full-stream observation point (the sync hook's body): the
//async group's execution faults surface here.
cudaError_t SyncStream() {
    return cudaStreamSynchronize(CurrentStream());
}

//--- cross-device copies (device ABI v3's copyD2D, spec §6) --------------
//Both paths block — the transfer's observation contract: the payload is
//resident on the destination when the call returns — and both order
//after the work enqueued on the SOURCE device's current stream, then
//hand the payload into the destination's current stream so a consumer
//enqueued there afterwards sees it. The backend's status wrapper
//(cuda_backend.cpp) picks peer or staging by the row capability
//matrix; the same-device case never reaches here (the registry hands
//back the handle).

bool CudaPeerReady(int32_t srcDevice, int32_t dstDevice) {
    return tensor::CrossCopyUsesPeer(
        g_dev[srcDevice].peerEnabled[dstDevice]);
}

namespace {

//The staging path's pinned host bounce: THREAD-LOCAL, the same model as
//the thread-local row pin — tensor natives can be entered from two
//carriers at once, and this is raw host state the CUDA stream APIs do
//not serialize: two threads sharing one bounce would interleave their
//payloads, and one thread's growth could cudaFreeHost a buffer the
//other's transfer has not drained. Grown by doubling on demand, kept
//for the thread (the same keep-resources discipline as the row
//handles; within one thread the copies are blocking, so replacing a
//smaller buffer never races a drained one).
thread_local void* g_stageBuffer = nullptr;
thread_local size_t g_stageCapacity = 0;

//The pinned host bounce's first allocation: doubled from here until it
//covers the requested transfer.
constexpr size_t DEFAULT_STAGE_CAPACITY_BYTES = 1024 * 1024;

bool EnsureStageBuffer(size_t bytes) {
    if (g_stageCapacity >= bytes)
        return true;
    size_t capacity = g_stageCapacity != 0
        ? g_stageCapacity : DEFAULT_STAGE_CAPACITY_BYTES;
    while (capacity < bytes)
        capacity *= 2;
    void* buffer = nullptr;
    if (cudaMallocHost(&buffer, capacity) != cudaSuccess) {
        (void)cudaGetLastError();   //the failure rides the returned status
        return false;
    }
    if (g_stageBuffer != nullptr)
        cudaFreeHost(g_stageBuffer);
    g_stageBuffer = buffer;
    g_stageCapacity = capacity;
    return true;
}

} // namespace

cudaError_t CopyPeer(int32_t dstDevice, void* dst, int32_t srcDevice,
                     const void* src, size_t bytes) {
    //The copy rides the source's current stream (ordered after the
    //source kernels that wrote the payload), hands off through an
    //event into the destination's current stream, and blocks on that
    //event. The handoff event is per-call (the join-event pattern in
    //the stream pool): entries can run concurrently, so no row-shared
    //event is safe to reuse, and an event costs microseconds against a
    //peer copy.
    SetDeviceForCall sourceRow(srcDevice);
    cudaError_t err = cudaMemcpyPeerAsync(dst, dstDevice, src, srcDevice,
        bytes, CurrentStream());
    if (err != cudaSuccess)
        return err;
    cudaEvent_t handoff = nullptr;
    if (cudaEventCreate(&handoff) != cudaSuccess) {
        (void)cudaGetLastError(); //the failure rides the returned status
        return cudaErrorMemoryAllocation;
    }
    err = cudaEventRecord(handoff, CurrentStream());
    if (err != cudaSuccess) {
        cudaEventDestroy(handoff);
        return err;
    }
    {
        SetDeviceForCall destRow(dstDevice);
        err = cudaStreamWaitEvent(CurrentStream(), handoff, 0);
    }
    if (err != cudaSuccess) {
        cudaEventDestroy(handoff);
        return err;
    }
    err = cudaEventSynchronize(handoff);
    cudaEventDestroy(handoff);
    return err;
}

cudaError_t CopyStaged(int32_t dstDevice, void* dst, int32_t srcDevice,
                       const void* src, size_t bytes) {
    if (!EnsureStageBuffer(bytes))
        return cudaErrorMemoryAllocation;
    //D2H on the source's row (ordered after the source's enqueued
    //work), block; then H2D on the destination's row, block — the
    //payload is resident when the call returns.
    SetDeviceForCall sourceRow(srcDevice);
    cudaError_t err = cudaMemcpyAsync(g_stageBuffer, src, bytes,
        cudaMemcpyDeviceToHost, CurrentStream());
    if (err == cudaSuccess)
        err = cudaStreamSynchronize(CurrentStream());
    if (err != cudaSuccess)
        return err;
    SetDeviceForCall destRow(dstDevice);
    err = cudaMemcpyAsync(dst, g_stageBuffer, bytes, cudaMemcpyHostToDevice,
        CurrentStream());
    if (err == cudaSuccess)
        err = cudaStreamSynchronize(CurrentStream());
    return err;
}

//--- the device-record hooks (stream-ordered memory + the transfers) ----

//The stream-ordered allocation: cudaMallocAsync/cudaFreeAsync order
//the allocation and the release inside the stream — same-stream reuse
//is safe by the driver's own serialization (the hand-written pool's
//first-fit free-list retired; NVIDIA's guidance replaces it), and the
//release threshold keeps the pool resident across sync-heavy loops.
//An allocation failure (out of memory) returns synchronously — the
//caller reads the null.

//The stream-ordered allocation needs no pool handle — the stream
//names the device and the device's default mempool serves it; the
//row's one-time attribute set (CudaDeviceReady) is what makes the
//pool behave for tensor workloads. The callers gate through
//CudaDeviceAccepted, so the row of the pinned device is ready here.
void* PoolAlloc(size_t bytes, cudaStream_t stream) {
    if (!CudaLazyInit(nullptr))
        return nullptr;
    void* ptr = nullptr;
    if (cudaMallocAsync(&ptr, bytes, stream) != cudaSuccess)
        return nullptr;
    return ptr;
}

void* PoolAllocOnStream(size_t bytes) {
    return PoolAlloc(bytes, CurrentStream());
}

void PoolFreeOnStream(void* ptr) {
    PoolFree(ptr, CurrentStream());
}

void PoolZero(void* ptr, size_t bytes) {
    //Stream-ordered: subsequent kernels and the host-boundary copies
    //(CopyBytes) observe the zeroed cells; no per-op sync.
    cudaMemsetAsync(ptr, 0, bytes, CurrentStream());
}

void PoolFree(void* ptr, cudaStream_t stream) {
    if (ptr == nullptr)
        return;
    cudaFreeAsync(ptr, stream);
}

//--- elementwise ---------------------------------------------------------

__global__ void BinaryKernel(int op, DevView a, DevView b, DevView dst,
                             double scalar, bool scalarLeft,
                             bool hasScalar) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t aa = FlatAddress(a, i);
    const int64_t ad = FlatAddress(dst, i);
    if (dst.elemKind == 1) {
        const float av = ((const float*)a.data)[aa];
        const float bv = hasScalar
            ? (float)scalar
            : ((const float*)b.data)[FlatAddress(b, i)];
        float l = scalarLeft ? bv : av;
        float r = scalarLeft ? av : bv;
        float out = 0.0f;
        switch (op) {
            case 0: out = l + r; break;
            case 1: out = l - r; break;
            case 2: out = l * r; break;
            case 3: out = l / r; break;
            //Rsub/Rdiv are the scalar-left variants: l/r are already
            //swapped by scalarLeft, so they compute identically to Sub/Div.
            case 4: out = l - r; break;
            case 5: out = l / r; break;
            case 6: out = r > 0.0f ? l : 0.0f; break;
        }
        ((float*)dst.data)[ad] = out;
        return;
    }
    const double av = ((const double*)a.data)[aa];
    const double bv = hasScalar
        ? scalar
        : ((const double*)b.data)[FlatAddress(b, i)];
    double l = scalarLeft ? bv : av;
    double r = scalarLeft ? av : bv;
    double out = 0.0;
    switch (op) {
        case 0: out = l + r; break;
        case 1: out = l - r; break;
        case 2: out = l * r; break;
        case 3: out = l / r; break;
        //Rsub/Rdiv: l/r already swapped by scalarLeft (see the float arm).
        case 4: out = l - r; break;
        case 5: out = l / r; break;
        case 6: out = r > 0.0 ? l : 0.0; break;
    }
    ((double*)dst.data)[ad] = out;
}

//The int binary kernel, cell-typed: int32 arrays compute in int32
//(the language's wrap semantics — the CPU walk's ApplyBin contract,
//MIN/-1 wrapping through the unsigned carrier; the per-cell divisor
//check rides the error flags). The dispatcher's elemKind gate picks
//the cell width.
template <typename T>
__global__ void BinaryKernelInt(int op, DevView a, DevView b, DevView dst,
                                int64_t scalar, bool scalarLeft,
                                bool hasScalar, int32_t* errorOut) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t aa = FlatAddress(a, i);
    const int64_t ad = FlatAddress(dst, i);
    const T av = ((const T*)a.data)[aa];
    const T bv = hasScalar
        ? (T)scalar
        : ((const T*)b.data)[FlatAddress(b, i)];
    const T l = scalarLeft ? bv : av;
    const T r = scalarLeft ? av : bv;
    T out = 0;
    switch (op) {
        case 0: out = (T)(l + r); break;
        case 1: out = (T)(l - r); break;
        case 2: out = (T)(l * r); break;
        case 3:
            if (r == 0) {
                errorOut[0] = kErrDivZero;
                errorOut[1] = i;
                return;
            }
            if (r == -1)
                out = (T)(0ULL - (unsigned long long)l);
            else
                out = (T)(l / r);
            break;
        case 4: out = (T)(bv - av); break;
        case 5:
            if (av == 0) {
                errorOut[0] = kErrDivZero;
                errorOut[1] = i;
                return;
            }
            out = (T)(bv / av);
            break;
        case 6: out = bv > T(0) ? av : T(0); break;
    }
    ((T*)dst.data)[ad] = out;
}

cudaError_t LaunchBinary(int op, const LaunchView& a,
                         const LaunchView& b, const LaunchView& dst,
                         int32_t* errorOut) {
    SetDeviceForCall launchRow(a.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    //Differentiated sync (spec 2026-11-16 §3): the float arms raise no
    //cell errors — enqueue and go; the int arms write the per-cell
    //division-by-zero flag, so they keep the clear-launch-sync-read
    //drain that raises at this op.
    const DevView da = ToDevView(a);
    const DevView db = ToDevView(b);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (dst.elemKind == 1 || dst.elemKind == 17) {
        BinaryKernel<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, db, dd, 0.0, false, false);
        return cudaGetLastError();
    }
    const uint32_t none[2] = {kErrNone, 0};
    cudaMemcpy(CurrentErrorFlags(), none, 2 * sizeof(int32_t), cudaMemcpyHostToDevice);
    if (dst.elemKind == 0) {
        BinaryKernelInt<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, db, dd, 0, false, false, CurrentErrorFlags());
    } else {
        BinaryKernelInt<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, db, dd, 0, false, false, CurrentErrorFlags());
    }
    cudaError_t err = cudaStreamSynchronize(CurrentStream());
    if (err != cudaSuccess)
        return err;
    //Explicit cast: kErrNone is uint32 0xFFFFFFFF, narrowed to the int32
    //sentinel; the value is fully overwritten by the device copy below, so
    //this only needs to be well-formed (GCC enforces the narrowing constraint
    //that MSVC only warns about).
    int32_t flags[2] = {static_cast<int32_t>(kErrNone), 0};
    cudaMemcpy(flags, CurrentErrorFlags(), 2 * sizeof(int32_t), cudaMemcpyDeviceToHost);
    if (flags[0] != kErrNone)
        *errorOut = flags[1];
    return cudaSuccess;
}

cudaError_t LaunchBinaryScalar(int op, const LaunchView& a,
                               double scalar, bool scalarLeft,
                               const LaunchView& dst, int32_t* errorOut) {
    SetDeviceForCall launchRow(a.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    //Differentiated sync (see LaunchBinary): the reverse-division arm
    //divides by array cells, so the int arms drain the flag at this op.
    const DevView da = ToDevView(a);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (dst.elemKind == 1 || dst.elemKind == 17) {
        BinaryKernel<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, dd, dd, scalar, scalarLeft, true);
        return cudaGetLastError();
    }
    const uint32_t none[2] = {kErrNone, 0};
    cudaMemcpy(CurrentErrorFlags(), none, 2 * sizeof(int32_t), cudaMemcpyHostToDevice);
    if (dst.elemKind == 0) {
        BinaryKernelInt<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, dd, dd, (int64_t)scalar, scalarLeft, true, CurrentErrorFlags());
    } else {
        BinaryKernelInt<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, dd, dd, (int64_t)scalar, scalarLeft, true, CurrentErrorFlags());
    }
    cudaError_t err = cudaStreamSynchronize(CurrentStream());
    if (err != cudaSuccess)
        return err;
    //Explicit cast: kErrNone is uint32 0xFFFFFFFF, narrowed to the int32
    //sentinel; the value is fully overwritten by the device copy below, so
    //this only needs to be well-formed (GCC enforces the narrowing constraint
    //that MSVC only warns about).
    int32_t flags[2] = {static_cast<int32_t>(kErrNone), 0};
    cudaMemcpy(flags, CurrentErrorFlags(), 2 * sizeof(int32_t), cudaMemcpyDeviceToHost);
    if (flags[0] != kErrNone)
        *errorOut = flags[1];
    return cudaSuccess;
}

__global__ void UnaryKernel(int op, DevView src, DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t sa = FlatAddress(src, i);
    const int64_t sd = FlatAddress(dst, i);
    double v = ((const double*)src.data)[sa];
    double out = v;
    if (op == 12) out = -v;
    else if (op == 13) out = exp(v);
    else if (op == 14) out = log(v);
    else if (op == 15) out = tanh(v);
    ((double*)dst.data)[sd] = out;
}

__global__ void UnaryKernelF(int op, DevView src, DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t sa = FlatAddress(src, i);
    const int64_t sd = FlatAddress(dst, i);
    //The float lanes compute in double and narrow (the CPU walk's
    //rounding contract — ScalarUnaryLane's rule).
    double v = ((const float*)src.data)[sa];
    double out = v;
    if (op == 12) out = -v;
    else if (op == 13) out = exp(v);
    else if (op == 14) out = log(v);
    else if (op == 15) out = tanh(v);
    ((float*)dst.data)[sd] = (float)out;
}

__global__ void UnaryKernelI32(int op, DevView src, DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t sa = FlatAddress(src, i);
    const int64_t sd = FlatAddress(dst, i);
    //int neg wraps like language arithmetic.
    int32_t v = ((const int32_t*)src.data)[sa];
    ((int32_t*)dst.data)[sd] = -v;
}

__global__ void UnaryKernelI64(int op, DevView src, DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t sa = FlatAddress(src, i);
    const int64_t sd = FlatAddress(dst, i);
    //int neg wraps like language arithmetic.
    int64_t v = ((const int64_t*)src.data)[sa];
    ((int64_t*)dst.data)[sd] = -v;
}

cudaError_t LaunchUnary(int op, const LaunchView& src,
                        const LaunchView& dst) {
    SetDeviceForCall launchRow(src.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (dst.elemKind == 1)
        UnaryKernelF<<<(int)blocks, kThreads, 0, CurrentStream()>>>(op, ds, dd);
    else if (dst.elemKind == 17)
        UnaryKernel<<<(int)blocks, kThreads, 0, CurrentStream()>>>(op, ds, dd);
    else if (dst.elemKind == 0)
        UnaryKernelI32<<<(int)blocks, kThreads, 0, CurrentStream()>>>(op, ds, dd);
    else
        UnaryKernelI64<<<(int)blocks, kThreads, 0, CurrentStream()>>>(op, ds, dd);
    return cudaGetLastError();
}

__global__ void ClampKernel(bool isMax, DevView src, double bound,
                            DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t sa = FlatAddress(src, i);
    const int64_t sd = FlatAddress(dst, i);
    double v = ((const double*)src.data)[sa];
    double out = isMax ? (v > bound ? v : bound)
                       : (v < bound ? v : bound);
    ((double*)dst.data)[sd] = out;
}

__global__ void ClampKernelF(bool isMax, DevView src, float bound,
                             DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t sa = FlatAddress(src, i);
    const int64_t sd = FlatAddress(dst, i);
    float v = ((const float*)src.data)[sa];
    float out = isMax ? (v > bound ? v : bound)
                      : (v < bound ? v : bound);
    ((float*)dst.data)[sd] = out;
}

//The int clamp: the bound rides the exact int64 field (the slot's
//typed scalar — no double roundtrip, the 0.14.2 discipline); the
//FlatAddress addressing matches the float kernels.
template <typename T>
__global__ void ClampKernelInt(bool isMax, DevView src, T bound,
                               DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t sa = FlatAddress(src, i);
    const int64_t sd = FlatAddress(dst, i);
    const T v = ((const T*)src.data)[sa];
    ((T*)dst.data)[sd] = isMax ? (v > bound ? v : bound)
                               : (v < bound ? v : bound);
}

cudaError_t LaunchClamp(bool isMax, const LaunchView& src, double bound,
                        int64_t iBound, const LaunchView& dst) {
    SetDeviceForCall launchRow(src.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    //Async (no cell errors). The float kinds read the double bound; the
    //int kinds the exact int64 twin.
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (dst.elemKind == 1)
        ClampKernelF<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            isMax, ds, (float)bound, dd);
    else if (dst.elemKind == 17)
        ClampKernel<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            isMax, ds, bound, dd);
    else if (dst.elemKind == 0)
        ClampKernelInt<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            isMax, ds, static_cast<int32_t>(iBound), dd);
    else
        ClampKernelInt<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            isMax, ds, iBound, dd);
    return cudaGetLastError();
}

//--- reductions ----------------------------------------------------------

//Block partials: each block reduces its contiguous segment into one
//accumulator; the host finalizes over the partials (the double
//accumulator contract — order differs from the CPU walk, the parity
//tolerance covers it).
__global__ void SumPartialKernel(const double* src, int64_t count,
                                 double* partials) {
    __shared__ double local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0.0;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        local[tid] += src[i];
    }
    __syncthreads();
    if (tid == 0) {
        double acc = 0.0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

//The f32 partial: reads FLOAT cells, accumulates DOUBLE (the CPU
//walk's contract — order-free for the exact sums the suite pins).
__global__ void SumPartialKernelF32(const float* src, int64_t count,
                                    double* partials) {
    __shared__ double local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0.0;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        local[tid] += (double)src[i];
    }
    __syncthreads();
    if (tid == 0) {
        double acc = 0.0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

__global__ void SumPartialKernelI32(const int32_t* src, int64_t count,
                                    int64_t* partials) {
    __shared__ int64_t local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        local[tid] += src[i];
    }
    __syncthreads();
    if (tid == 0) {
        int64_t acc = 0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

__global__ void SumPartialKernelI64(const int64_t* src, int64_t count,
                                    int64_t* partials) {
    __shared__ int64_t local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        local[tid] += src[i];
    }
    __syncthreads();
    if (tid == 0) {
        int64_t acc = 0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

//--- strided-aware whole-array kernels (the strided branch of the
//reduction launchers) ------------------------------------------------
//The dense kernels above index data[] directly (a dense view). These read
//the element at FlatAddress(v, i) — the recorded strides plus the view
//offset folded per element — so a strided device view (transpose/slice)
//reduces the right elements with zero copy. Same block-partials + host
//finalize structure as the dense kernels; only the addressing differs.
//Per-dtype accumulator/identity mirror the dense kernels (the parity
//contract), so the templates carry the carrier type per family: the float
//kinds accumulate double, the int kinds int64.

//T = float or double; DOUBLE accumulator (the CPU walk's contract).
template <typename T>
__global__ void SumStridedKernel(DevView v, int64_t count, double* partials) {
    __shared__ double local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0.0;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        local[tid] += (double)((const T*)v.data)[FlatAddress(v, i)];
    }
    __syncthreads();
    if (tid == 0) {
        double acc = 0.0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

//T = int32_t or int64_t; INT64 accumulator (the wrap happens at the final
//narrow, matching the CPU's static_cast<T>(acc) exactly).
template <typename T>
__global__ void SumStridedKernelInt(DevView v, int64_t count,
                                   int64_t* partials) {
    __shared__ int64_t local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        local[tid] += (int64_t)((const T*)v.data)[FlatAddress(v, i)];
    }
    __syncthreads();
    if (tid == 0) {
        int64_t acc = 0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

//The reduction drain: the host-boundary sync of the block-partial
//family. A launch error must short-circuit before the copy (a failed
//launch leaves the scratch stale and the blocking read would succeed
//over garbage); an execution fault surfaces as the copy's return (the
//stream sync rides the blocking memcpy).
static cudaError_t ReadPartials(void* dPartial, void* dst, size_t bytes) {
    const cudaError_t launchErr = cudaGetLastError();
    if (launchErr != cudaSuccess) {
        PoolFree(dPartial, CurrentStream());
        return launchErr;
    }
    const cudaError_t copyErr = cudaMemcpy(dst, dPartial, bytes,
        cudaMemcpyDeviceToHost);
    PoolFree(dPartial, CurrentStream());
    return copyErr;
}

cudaError_t LaunchSumAll(const LaunchView& src, double* out,
                         int64_t* outI, bool isInt) {
    SetDeviceForCall launchRow(src.device);
    //The empty identity (the CPU walk's contract — the suite pins
    //sum(empty) == 0): no launch, no scratch. A zero-block scratch
    //request must not reach PoolAlloc — cudaMalloc(0) succeeds with a
    //NULL pointer, which the null check would misread as OOM.
    if (src.elemCount == 0) {
        if (isInt)
            *outI = 0;
        else
            *out = 0.0;
        return cudaSuccess;
    }
    //The offset-fold discipline (design D1): ToLaunch keeps the real
    //strides + offset; the launcher branches on IsDense and each branch
    //folds the offset exactly once — the dense branch folds it into the
    //base pointer (the dense kernels index data[] directly), the strided
    //branch keeps it in the DevView (FlatAddress folds it per element).
    const int64_t count = src.elemCount;
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    const DevView v = ToDevView(src);
    const bool dense = IsDense(v);
    if (isInt) {
        //int arrays accumulate in int64 (the CPU walk's Acc carrier —
        //the wrap happens at the final narrow, matching the CPU's
        //static_cast<T>(acc) exactly). The int32/int64 cells ride their
        //own partial kernel (reading them as doubles would corrupt).
        int64_t* dPartial = static_cast<int64_t*>(
            PoolAlloc(blocks * sizeof(int64_t), CurrentStream()));
        if (dPartial == nullptr)
            return cudaErrorMemoryAllocation;
        if (dense) {
            //The base pointer folds the offset (element units).
            if (src.elemKind == 0) {
                SumPartialKernelI32<<<(int)blocks, kThreads, 0,
                    CurrentStream()>>>((const int32_t*)src.data + src.offset, count,
                    dPartial);
            } else {
                SumPartialKernelI64<<<(int)blocks, kThreads, 0,
                    CurrentStream()>>>((const int64_t*)src.data + src.offset, count,
                    dPartial);
            }
        } else {
            if (src.elemKind == 0) {
                SumStridedKernelInt<int32_t><<<(int)blocks, kThreads, 0,
                    CurrentStream()>>>(v, count, dPartial);
            } else {
                SumStridedKernelInt<int64_t><<<(int)blocks, kThreads, 0,
                    CurrentStream()>>>(v, count, dPartial);
            }
        }
        std::vector<int64_t> partials(blocks);
        const cudaError_t readErr = ReadPartials(
            dPartial, partials.data(), blocks * sizeof(int64_t));
        if (readErr != cudaSuccess)
            return readErr;
        int64_t acc = 0;
        for (int64_t p : partials)
            acc += p;
        *outI = acc;
        return cudaSuccess;
    }
    double* dPartial = static_cast<double*>(
        PoolAlloc(blocks * sizeof(double), CurrentStream()));
    if (dPartial == nullptr)
        return cudaErrorMemoryAllocation;
    //The float kinds branch by cell width: an f32 array read as
    //doubles would consume two cells per element (garbage + OOB).
    if (dense) {
        if (src.elemKind == 1) {
            SumPartialKernelF32<<<(int)blocks, kThreads, 0,
                CurrentStream()>>>((const float*)src.data + src.offset, count,
                dPartial);
        } else {
            SumPartialKernel<<<(int)blocks, kThreads, 0,
                CurrentStream()>>>((const double*)src.data + src.offset, count,
                dPartial);
        }
    } else {
        if (src.elemKind == 1) {
            SumStridedKernel<float><<<(int)blocks, kThreads, 0,
                CurrentStream()>>>(v, count, dPartial);
        } else {
            SumStridedKernel<double><<<(int)blocks, kThreads, 0,
                CurrentStream()>>>(v, count, dPartial);
        }
    }
    std::vector<double> partials(blocks);
    const cudaError_t readErr = ReadPartials(
        dPartial, partials.data(), blocks * sizeof(double));
    if (readErr != cudaSuccess)
        return readErr;
    double acc = 0.0;
    for (double p : partials)
        acc += p;
    *out = acc;
    return cudaSuccess;
}

//--- axis reductions (v0.14.3: the general per-output-element scan;
//the specialized f32-only axis kernels retired) --------------------------

//One thread per output element: the element's row-major ordinal over
//the non-axis dims decomposes onto the source address (the odometer
//division skips the axis dim), and the scan walks the axis. Sum rides
//the CPU walk's carrier (the float kinds accumulate double, the int
//kinds int64 — the final narrow back to T); Max/Min keep the first-wins
//T compare. All kinds, all axes, async (no cell errors).
template <typename T, typename ACC>
__global__ void ReduceAxisScanKernel(DevView src, int32_t axis,
                                     int32_t op, DevView dst) {
    const int64_t p = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (p >= dst.elemCount)
        return;
    int64_t addr = src.offset;
    int64_t rem = p;
    for (int32_t d = src.rank - 1; d >= 0; --d) {
        if (d == axis)
            continue;
        const int64_t dim = src.dims[d];
        const int64_t coord = dim > 0 ? rem % dim : 0;
        rem /= (dim > 0 ? dim : 1);
        addr += coord * src.strides[d];
    }
    const int64_t along = src.dims[axis];
    ACC acc = ACC(0);
    T best = ((const T*)src.data)[addr];
    for (int64_t k = 0; k < along; ++k) {
        const T cur =
            ((const T*)src.data)[addr + k * src.strides[axis]];
        if (op == 0) {
            acc += static_cast<ACC>(cur);
        } else if (k > 0) {
            //Max=1 / Min=2: the first-wins compare (a later equal or
            //NaN cell never replaces the incumbent).
            if (op == 1 ? (cur > best) : (cur < best))
                best = cur;
        }
    }
    ((T*)dst.data)[dst.offset + p] =
        op == 0 ? static_cast<T>(acc) : best;
}

//The argmax/argmin scan: the same odometer addressing; the winner is
//the first max/min along the axis (ties keep the earliest index).
template <typename T>
__global__ void ArgExtremeScanKernel(DevView src, int32_t axis,
                                     bool isMax, DevView dst) {
    const int64_t p = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (p >= dst.elemCount)
        return;
    int64_t addr = src.offset;
    int64_t rem = p;
    for (int32_t d = src.rank - 1; d >= 0; --d) {
        if (d == axis)
            continue;
        const int64_t dim = src.dims[d];
        const int64_t coord = dim > 0 ? rem % dim : 0;
        rem /= (dim > 0 ? dim : 1);
        addr += coord * src.strides[d];
    }
    const int64_t along = src.dims[axis];
    int32_t bestAt = 0;
    T best = ((const T*)src.data)[addr];
    for (int64_t k = 1; k < along; ++k) {
        const T cur =
            ((const T*)src.data)[addr + k * src.strides[axis]];
        if (isMax ? (cur > best) : (cur < best)) {
            best = cur;
            bestAt = static_cast<int32_t>(k);
        }
    }
    static_cast<int32_t*>(dst.data)[dst.offset + p] = bestAt;
}

cudaError_t LaunchReduceAxis(int op, const LaunchView& src, int axis,
                             const LaunchView& dst) {
    SetDeviceForCall launchRow(src.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    //ReduceAxisOp: Sum=0, Max=1, Min=2 (the enum order). Async (no
    //cell errors); all kinds and axes (v0.14.3 — the f32-only scope
    //and the specialized axis kernels retired).
    if (op != 0 && op != 1 && op != 2)
        return cudaErrorNotSupported;
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (src.elemKind == 1) {
        ReduceAxisScanKernel<float, double><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(ds, axis, op, dd);
    } else if (src.elemKind == 17) {
        ReduceAxisScanKernel<double, double><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(ds, axis, op, dd);
    } else if (src.elemKind == 0) {
        ReduceAxisScanKernel<int32_t, int64_t><<<(int)blocks, kThreads,
            0, CurrentStream()>>>(ds, axis, op, dd);
    } else {
        ReduceAxisScanKernel<int64_t, int64_t><<<(int)blocks, kThreads,
            0, CurrentStream()>>>(ds, axis, op, dd);
    }
    return cudaGetLastError();
}

//The argmax/argmin launcher (int32 winner indices, first-wins ties):
//the kind gate retired with the f32-only scan kernel.
cudaError_t LaunchArgExtreme(bool isMax, const LaunchView& src,
                             int axis, const LaunchView& dst) {
    SetDeviceForCall launchRow(src.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    //Async (no cell errors).
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (src.elemKind == 1) {
        ArgExtremeScanKernel<float><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(ds, axis, isMax, dd);
    } else if (src.elemKind == 17) {
        ArgExtremeScanKernel<double><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(ds, axis, isMax, dd);
    } else if (src.elemKind == 0) {
        ArgExtremeScanKernel<int32_t><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(ds, axis, isMax, dd);
    } else {
        ArgExtremeScanKernel<int64_t><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(ds, axis, isMax, dd);
    }
    return cudaGetLastError();
}

__global__ void DotPartialKernel(const double* a, const double* b,
                                 int64_t count, double* partials) {
    __shared__ double local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0.0;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        local[tid] += a[i] * b[i];
    }
    __syncthreads();
    if (tid == 0) {
        double acc = 0.0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

//The f32/int dot partials: cell-typed reads with the carrier
//accumulator per kind (f32 accumulates float — the CPU contract; the
//int kinds accumulate int64).
__global__ void DotPartialKernelF32(const float* a, const float* b,
                                    int64_t count, float* partials) {
    __shared__ float local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0.0f;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        local[tid] += a[i] * b[i];
    }
    __syncthreads();
    if (tid == 0) {
        float acc = 0.0f;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

__global__ void DotPartialKernelI32(const int32_t* a, const int32_t* b,
                                    int64_t count, int64_t* partials) {
    __shared__ int64_t local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        local[tid] += (int64_t)a[i] * (int64_t)b[i];
    }
    __syncthreads();
    if (tid == 0) {
        int64_t acc = 0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

__global__ void DotPartialKernelI64(const int64_t* a, const int64_t* b,
                                    int64_t count, int64_t* partials) {
    __shared__ int64_t local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        local[tid] += a[i] * b[i];
    }
    __syncthreads();
    if (tid == 0) {
        int64_t acc = 0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

//--- strided-aware dot (both operands fold, each via its own DevView) ---

//T = float or double; T accumulator (the CPU carrier contract — f32
//accumulates float, f64 double).
template <typename T>
__global__ void DotStridedKernel(DevView va, DevView vb, int64_t count,
                                T* partials) {
    __shared__ T local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = T(0);
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        const T av = ((const T*)va.data)[FlatAddress(va, i)];
        const T bv = ((const T*)vb.data)[FlatAddress(vb, i)];
        local[tid] += av * bv;
    }
    __syncthreads();
    if (tid == 0) {
        T acc = T(0);
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

//T = int32_t or int64_t; INT64 accumulator (the CPU carrier contract).
template <typename T>
__global__ void DotStridedKernelInt(DevView va, DevView vb, int64_t count,
                                   int64_t* partials) {
    __shared__ int64_t local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    for (int64_t i = base + tid; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        const int64_t av = (int64_t)((const T*)va.data)[FlatAddress(va, i)];
        const int64_t bv = (int64_t)((const T*)vb.data)[FlatAddress(vb, i)];
        local[tid] += av * bv;
    }
    __syncthreads();
    if (tid == 0) {
        int64_t acc = 0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        partials[blockIdx.x] = acc;
    }
}

cudaError_t LaunchDot(const LaunchView& a, const LaunchView& b,
                      double* out, int64_t* outI, bool isInt) {
    SetDeviceForCall launchRow(a.device);
    (void)isInt;
    const int64_t count = a.elemCount;
    //The empty identity (the CPU walk's contract — dot(empty) == 0);
    //see LaunchSumAll for the zero-block scratch rationale.
    if (count == 0) {
        if (outI)
            *outI = 0;
        if (out)
            *out = 0.0;
        return cudaSuccess;
    }
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    //The offset-fold discipline (design D1): ToLaunch keeps the real
    //strides + offset; the launcher branches on IsDense and each branch
    //folds the offset exactly once. Both operands fold (dot is symmetric
    //in a and b) — either strided takes the strided kernel, which is
    //also correct for a dense operand (FlatAddress degenerates to the
    //dense address).
    const DevView va = ToDevView(a);
    const DevView vb = ToDevView(b);
    const bool dense = IsDense(va) && IsDense(vb);
    //The kind branches: f32 accumulates float (the CPU carrier
    //contract), the int kinds int64, f64 double. The old double-only
    //kernel mis-read every other kind's cells.
    if (a.elemKind == 1) {
        float* dPartial = static_cast<float*>(
            PoolAlloc(blocks * sizeof(float), CurrentStream()));
        if (dPartial == nullptr)
            return cudaErrorMemoryAllocation;
        if (dense) {
            DotPartialKernelF32<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
                (const float*)a.data + a.offset,
                (const float*)b.data + b.offset, count, dPartial);
        } else {
            DotStridedKernel<float><<<(int)blocks, kThreads, 0,
                CurrentStream()>>>(va, vb, count, dPartial);
        }
        std::vector<float> partials(blocks);
        const cudaError_t readErr = ReadPartials(
            dPartial, partials.data(), blocks * sizeof(float));
        if (readErr != cudaSuccess)
            return readErr;
        float acc = 0.0f;
        for (float p : partials)
            acc += p;
        if (outI)
            *outI = (int64_t)acc;
        if (out)
            *out = (double)acc;
        return cudaSuccess;
    }
    if (a.elemKind == 0 || a.elemKind == 15) {
        int64_t* dPartial = static_cast<int64_t*>(
            PoolAlloc(blocks * sizeof(int64_t), CurrentStream()));
        if (dPartial == nullptr)
            return cudaErrorMemoryAllocation;
        if (dense) {
            if (a.elemKind == 0)
                DotPartialKernelI32<<<(int)blocks, kThreads, 0,
                    CurrentStream()>>>(
                    (const int32_t*)a.data + a.offset,
                    (const int32_t*)b.data + b.offset, count, dPartial);
            else
                DotPartialKernelI64<<<(int)blocks, kThreads, 0,
                    CurrentStream()>>>(
                    (const int64_t*)a.data + a.offset,
                    (const int64_t*)b.data + b.offset, count, dPartial);
        } else {
            if (a.elemKind == 0)
                DotStridedKernelInt<int32_t><<<(int)blocks, kThreads, 0,
                    CurrentStream()>>>(va, vb, count, dPartial);
            else
                DotStridedKernelInt<int64_t><<<(int)blocks, kThreads, 0,
                    CurrentStream()>>>(va, vb, count, dPartial);
        }
        std::vector<int64_t> partials(blocks);
        const cudaError_t readErr = ReadPartials(
            dPartial, partials.data(), blocks * sizeof(int64_t));
        if (readErr != cudaSuccess)
            return readErr;
        int64_t acc = 0;
        for (int64_t p : partials)
            acc += p;
        *outI = acc;
        return cudaSuccess;
    }
    double* dPartial = static_cast<double*>(
        PoolAlloc(blocks * sizeof(double), CurrentStream()));
    if (dPartial == nullptr)
        return cudaErrorMemoryAllocation;
    if (dense) {
        DotPartialKernel<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            (const double*)a.data + a.offset,
            (const double*)b.data + b.offset, count, dPartial);
    } else {
        DotStridedKernel<double><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(va, vb, count, dPartial);
    }
    std::vector<double> partials(blocks);
    const cudaError_t readErr = ReadPartials(
        dPartial, partials.data(), blocks * sizeof(double));
    if (readErr != cudaSuccess)
        return readErr;
    {
        double acc = 0.0;
        for (double p : partials)
            acc += p;
        *out = acc;
    }
    return cudaSuccess;
}

//--- extreme (value reduction with the first-wins rule) ------------------

__global__ void ExtremePartialKernel(const double* src, int64_t count,
                                     bool isMax, double* partials) {
    __shared__ double local[kThreads];
    const int tid = threadIdx.x;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    //Tail-block threads past the end take the identity (-inf for max, +inf
    //for min) so they never win; a real first element still lands directly
    //first-wins (a later NaN in the strided range never replaces it). The
    //identity is ±inf, not a finite bound: a real element can be an
    //overflow-produced ±inf (IEEE 754, e.g. 1e200 * 1e200), and only ±inf
    //is beyond it, so a finite DBL_MAX would let an OOB thread beat a real
    //±inf. Constructed from the IEEE bit pattern (DBL_INFINITY is a __host__
    //limits value, undefined in device code), so no double→float conversion
    //and no nvcc 221-D warning — the double analog of the f32 kernel's
    //__int_as_float(0x7f800000).
    const double inf = isMax
        ? -__longlong_as_double(0x7ff0000000000000LL)
        :  __longlong_as_double(0x7ff0000000000000LL);
    if (base + tid < count)
        local[tid] = src[base + tid];
    else
        local[tid] = inf;
    for (int64_t i = base + tid + gridDim.x * kThreads; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        const double cur = src[i];
        if (isMax ? cur > local[tid] : cur < local[tid])
            local[tid] = cur;
    }
    __syncthreads();
    if (tid == 0) {
        double best = local[0];
        for (int t = 1; t < kThreads; ++t) {
            if (isMax ? local[t] > best : local[t] < best)
                best = local[t];
        }
        partials[blockIdx.x] = best;
    }
}

//The f32 extreme partial: FLOAT cells, first-wins compare (a NaN
//incumbent never loses — the scalar walk's rule).
__global__ void ExtremePartialKernelF32(const float* src, int64_t count,
                                        bool isMax, float* partials) {
    __shared__ float local[kThreads];
    const int tid = threadIdx.x;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    //Tail-block threads past the end take the identity (-inf for max, +inf
    //for min) so they never win; a real first element still lands directly
    //first-wins (a later NaN in the strided range never replaces it). The
    //float infinity is the IEEE bit pattern — a double INFINITY would
    //overflow to inf and trip the nvcc 221-D warning on the implicit
    //double-to-float, and a __host__ limits call is not allowed in a kernel.
    const float inf = __int_as_float(0x7f800000);
    if (base + tid < count)
        local[tid] = src[base + tid];
    else
        local[tid] = isMax ? -inf : inf;
    for (int64_t i = base + tid + gridDim.x * kThreads; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        const float cur = src[i];
        if (isMax ? cur > local[tid] : cur < local[tid])
            local[tid] = cur;
    }
    __syncthreads();
    if (tid == 0) {
        float best = local[0];
        for (int t = 1; t < kThreads; ++t) {
            if (isMax ? local[t] > best : local[t] < best)
                best = local[t];
        }
        partials[blockIdx.x] = best;
    }
}

//The int extreme partials: INT32/INT64 cells, first-wins compare, identity
//INT32_MIN/MAX and INT64_MIN/MAX for the tail-block OOB threads.
__global__ void ExtremePartialKernelI32(const int32_t* src, int64_t count,
                                        bool isMax, int32_t* partials) {
    __shared__ int32_t local[kThreads];
    const int tid = threadIdx.x;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    if (base + tid < count)
        local[tid] = src[base + tid];
    else
        local[tid] = isMax ? INT32_MIN : INT32_MAX;
    for (int64_t i = base + tid + gridDim.x * kThreads; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        const int32_t cur = src[i];
        if (isMax ? cur > local[tid] : cur < local[tid])
            local[tid] = cur;
    }
    __syncthreads();
    if (tid == 0) {
        int32_t best = local[0];
        for (int t = 1; t < kThreads; ++t) {
            if (isMax ? local[t] > best : local[t] < best)
                best = local[t];
        }
        partials[blockIdx.x] = best;
    }
}

__global__ void ExtremePartialKernelI64(const int64_t* src, int64_t count,
                                        bool isMax, int64_t* partials) {
    __shared__ int64_t local[kThreads];
    const int tid = threadIdx.x;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    if (base + tid < count)
        local[tid] = src[base + tid];
    else
        local[tid] = isMax ? INT64_MIN : INT64_MAX;
    for (int64_t i = base + tid + gridDim.x * kThreads; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        const int64_t cur = src[i];
        if (isMax ? cur > local[tid] : cur < local[tid])
            local[tid] = cur;
    }
    __syncthreads();
    if (tid == 0) {
        int64_t best = local[0];
        for (int t = 1; t < kThreads; ++t) {
            if (isMax ? local[t] > best : local[t] < best)
                best = local[t];
        }
        partials[blockIdx.x] = best;
    }
}

//--- strided-aware extreme (first-wins; the per-dtype tail-block identity
//mirrors the dense kernels: ±inf for the float kinds — beyond any real
//element, including an overflow-produced ±inf — INT_MIN/MAX for the int
//kinds) ----------------------------------------------------------------

__global__ void ExtremeStridedKernelF32(DevView v, int64_t count, bool isMax,
                                       float* partials) {
    __shared__ float local[kThreads];
    const int tid = threadIdx.x;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    const float inf = __int_as_float(0x7f800000);
    if (base + tid < count)
        local[tid] = ((const float*)v.data)[FlatAddress(v, base + tid)];
    else
        local[tid] = isMax ? -inf : inf;
    for (int64_t i = base + tid + gridDim.x * kThreads; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        const float cur = ((const float*)v.data)[FlatAddress(v, i)];
        if (isMax ? cur > local[tid] : cur < local[tid])
            local[tid] = cur;
    }
    __syncthreads();
    if (tid == 0) {
        float best = local[0];
        for (int t = 1; t < kThreads; ++t) {
            if (isMax ? local[t] > best : local[t] < best)
                best = local[t];
        }
        partials[blockIdx.x] = best;
    }
}

__global__ void ExtremeStridedKernelF64(DevView v, int64_t count, bool isMax,
                                       double* partials) {
    __shared__ double local[kThreads];
    const int tid = threadIdx.x;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    const double inf = __longlong_as_double(0x7ff0000000000000LL);
    if (base + tid < count)
        local[tid] = ((const double*)v.data)[FlatAddress(v, base + tid)];
    else
        local[tid] = isMax ? -inf : inf;
    for (int64_t i = base + tid + gridDim.x * kThreads; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        const double cur = ((const double*)v.data)[FlatAddress(v, i)];
        if (isMax ? cur > local[tid] : cur < local[tid])
            local[tid] = cur;
    }
    __syncthreads();
    if (tid == 0) {
        double best = local[0];
        for (int t = 1; t < kThreads; ++t) {
            if (isMax ? local[t] > best : local[t] < best)
                best = local[t];
        }
        partials[blockIdx.x] = best;
    }
}

__global__ void ExtremeStridedKernelI32(DevView v, int64_t count, bool isMax,
                                       int32_t* partials) {
    __shared__ int32_t local[kThreads];
    const int tid = threadIdx.x;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    if (base + tid < count)
        local[tid] = ((const int32_t*)v.data)[FlatAddress(v, base + tid)];
    else
        local[tid] = isMax ? INT32_MIN : INT32_MAX;
    for (int64_t i = base + tid + gridDim.x * kThreads; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        const int32_t cur = ((const int32_t*)v.data)[FlatAddress(v, i)];
        if (isMax ? cur > local[tid] : cur < local[tid])
            local[tid] = cur;
    }
    __syncthreads();
    if (tid == 0) {
        int32_t best = local[0];
        for (int t = 1; t < kThreads; ++t) {
            if (isMax ? local[t] > best : local[t] < best)
                best = local[t];
        }
        partials[blockIdx.x] = best;
    }
}

__global__ void ExtremeStridedKernelI64(DevView v, int64_t count, bool isMax,
                                       int64_t* partials) {
    __shared__ int64_t local[kThreads];
    const int tid = threadIdx.x;
    const int64_t base = blockIdx.x * (int64_t)kThreads;
    if (base + tid < count)
        local[tid] = ((const int64_t*)v.data)[FlatAddress(v, base + tid)];
    else
        local[tid] = isMax ? INT64_MIN : INT64_MAX;
    for (int64_t i = base + tid + gridDim.x * kThreads; i < count;
         i += (int64_t)gridDim.x * kThreads) {
        const int64_t cur = ((const int64_t*)v.data)[FlatAddress(v, i)];
        if (isMax ? cur > local[tid] : cur < local[tid])
            local[tid] = cur;
    }
    __syncthreads();
    if (tid == 0) {
        int64_t best = local[0];
        for (int t = 1; t < kThreads; ++t) {
            if (isMax ? local[t] > best : local[t] < best)
                best = local[t];
        }
        partials[blockIdx.x] = best;
    }
}

cudaError_t LaunchExtreme(bool isMax, const LaunchView& src,
                          double* outF, int64_t* outI, bool isInt) {
    SetDeviceForCall launchRow(src.device);
    const int64_t count = src.elemCount;
    if (count == 0) {
        //Match the CPU reduction's empty behavior (extreme → 0).
        if (isInt)
            *outI = 0;
        else
            *outF = 0.0;
        return cudaSuccess;
    }
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    //The offset-fold discipline (design D1): the dense branch folds the
    //offset into the base pointer; the strided branch keeps it in the
    //DevView (FlatAddress folds it).
    const DevView v = ToDevView(src);
    const bool dense = IsDense(v);
    if (src.elemKind == 1) {
        float* dPartial = static_cast<float*>(
            PoolAlloc(blocks * sizeof(float), CurrentStream()));
        if (dPartial == nullptr)
            return cudaErrorMemoryAllocation;
        if (dense) {
            ExtremePartialKernelF32<<<(int)blocks, kThreads, 0,
                CurrentStream()>>>((const float*)src.data + src.offset, count,
                isMax, dPartial);
        } else {
            ExtremeStridedKernelF32<<<(int)blocks, kThreads, 0,
                CurrentStream()>>>(v, count, isMax, dPartial);
        }
        std::vector<float> partials(blocks);
        const cudaError_t readErr = ReadPartials(
            dPartial, partials.data(), blocks * sizeof(float));
        if (readErr != cudaSuccess)
            return readErr;
        float best = partials[0];
        for (size_t i = 1; i < partials.size(); ++i) {
            if (isMax ? partials[i] > best : partials[i] < best)
                best = partials[i];
        }
        *outF = (double)best;
        return cudaSuccess;
    }
    if (src.elemKind == 0) {
        int32_t* dPartial = static_cast<int32_t*>(
            PoolAlloc(blocks * sizeof(int32_t), CurrentStream()));
        if (dPartial == nullptr)
            return cudaErrorMemoryAllocation;
        if (dense) {
            ExtremePartialKernelI32<<<(int)blocks, kThreads, 0,
                CurrentStream()>>>((const int32_t*)src.data + src.offset, count,
                isMax, dPartial);
        } else {
            ExtremeStridedKernelI32<<<(int)blocks, kThreads, 0,
                CurrentStream()>>>(v, count, isMax, dPartial);
        }
        std::vector<int32_t> partials(blocks);
        const cudaError_t readErr = ReadPartials(
            dPartial, partials.data(), blocks * sizeof(int32_t));
        if (readErr != cudaSuccess)
            return readErr;
        int64_t best = partials[0];
        for (size_t i = 1; i < partials.size(); ++i) {
            if (isMax ? (int64_t)partials[i] > best
                     : (int64_t)partials[i] < best)
                best = partials[i];
        }
        *outI = best;
        return cudaSuccess;
    }
    if (src.elemKind == 15) {
        int64_t* dPartial = static_cast<int64_t*>(
            PoolAlloc(blocks * sizeof(int64_t), CurrentStream()));
        if (dPartial == nullptr)
            return cudaErrorMemoryAllocation;
        if (dense) {
            ExtremePartialKernelI64<<<(int)blocks, kThreads, 0,
                CurrentStream()>>>((const int64_t*)src.data + src.offset, count,
                isMax, dPartial);
        } else {
            ExtremeStridedKernelI64<<<(int)blocks, kThreads, 0,
                CurrentStream()>>>(v, count, isMax, dPartial);
        }
        std::vector<int64_t> partials(blocks);
        const cudaError_t readErr = ReadPartials(
            dPartial, partials.data(), blocks * sizeof(int64_t));
        if (readErr != cudaSuccess)
            return readErr;
        int64_t best = partials[0];
        for (size_t i = 1; i < partials.size(); ++i) {
            if (isMax ? partials[i] > best : partials[i] < best)
                best = partials[i];
        }
        *outI = best;
        return cudaSuccess;
    }
    double* dPartial = static_cast<double*>(
        PoolAlloc(blocks * sizeof(double), CurrentStream()));
    if (dPartial == nullptr)
        return cudaErrorMemoryAllocation;
    if (dense) {
        ExtremePartialKernel<<<(int)blocks, kThreads, 0,
            CurrentStream()>>>((const double*)src.data + src.offset, count, isMax,
            dPartial);
    } else {
        ExtremeStridedKernelF64<<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(v, count, isMax, dPartial);
    }
    std::vector<double> partials(blocks);
    const cudaError_t readErr = ReadPartials(
        dPartial, partials.data(), blocks * sizeof(double));
    if (readErr != cudaSuccess)
        return readErr;
    double best = partials[0];
    for (size_t i = 1; i < partials.size(); ++i) {
        if (isMax ? partials[i] > best : partials[i] < best)
            best = partials[i];
    }
    if (isInt)
        *outI = (int64_t)best;
    else
        *outF = best;
    return cudaSuccess;
}

//--- matmul: shared-memory tiled kernel ----------------------------------

__global__ void MatmulKernelF(const float* a, const float* b, float* dst,
                              int32_t rows, int32_t inner, int32_t cols) {
    __shared__ float tileA[16][16];
    __shared__ float tileB[16][16];
    const int row = blockIdx.y * 16 + threadIdx.y;
    const int col = blockIdx.x * 16 + threadIdx.x;
    float acc = 0.0f;
    const int tiles = (inner + 15) / 16;
    for (int t = 0; t < tiles; ++t) {
        const int aCol = t * 16 + threadIdx.x;
        const int bRow = t * 16 + threadIdx.y;
        tileA[threadIdx.y][threadIdx.x] =
            row < rows && aCol < inner ? a[row * inner + aCol] : 0.0f;
        tileB[threadIdx.y][threadIdx.x] =
            bRow < inner && col < cols ? b[bRow * cols + col] : 0.0f;
        __syncthreads();
        for (int k = 0; k < 16; ++k)
            acc += tileA[threadIdx.y][k] * tileB[k][threadIdx.x];
        __syncthreads();
    }
    if (row < rows && col < cols)
        dst[row * cols + col] = acc;
}

//The f64 matmul: the same 16×16 shared-memory tiling as MatmulKernelF with
//double cells.
__global__ void MatmulKernelF64(const double* a, const double* b, double* dst,
                                int32_t rows, int32_t inner, int32_t cols) {
    __shared__ double tileA[16][16];
    __shared__ double tileB[16][16];
    const int row = blockIdx.y * 16 + threadIdx.y;
    const int col = blockIdx.x * 16 + threadIdx.x;
    double acc = 0.0;
    const int tiles = (inner + 15) / 16;
    for (int t = 0; t < tiles; ++t) {
        const int aCol = t * 16 + threadIdx.x;
        const int bRow = t * 16 + threadIdx.y;
        tileA[threadIdx.y][threadIdx.x] =
            row < rows && aCol < inner ? a[row * inner + aCol] : 0.0;
        tileB[threadIdx.y][threadIdx.x] =
            bRow < inner && col < cols ? b[bRow * cols + col] : 0.0;
        __syncthreads();
        for (int k = 0; k < 16; ++k)
            acc += tileA[threadIdx.y][k] * tileB[k][threadIdx.x];
        __syncthreads();
    }
    if (row < rows && col < cols)
        dst[row * cols + col] = acc;
}

//The int matmul: int32 cells with C++ int32 product/accumulate wrapping, or
//int64 cells with int64 product/accumulate (no wrap in the test range).
__global__ void MatmulKernelI32(const int32_t* a, const int32_t* b,
                                int32_t* dst, int32_t rows, int32_t inner,
                                int32_t cols) {
    __shared__ int32_t tileA[16][16];
    __shared__ int32_t tileB[16][16];
    const int row = blockIdx.y * 16 + threadIdx.y;
    const int col = blockIdx.x * 16 + threadIdx.x;
    int32_t acc = 0;
    const int tiles = (inner + 15) / 16;
    for (int t = 0; t < tiles; ++t) {
        const int aCol = t * 16 + threadIdx.x;
        const int bRow = t * 16 + threadIdx.y;
        tileA[threadIdx.y][threadIdx.x] =
            row < rows && aCol < inner ? a[row * inner + aCol] : 0;
        tileB[threadIdx.y][threadIdx.x] =
            bRow < inner && col < cols ? b[bRow * cols + col] : 0;
        __syncthreads();
        for (int k = 0; k < 16; ++k)
            acc += tileA[threadIdx.y][k] * tileB[k][threadIdx.x];
        __syncthreads();
    }
    if (row < rows && col < cols)
        dst[row * cols + col] = acc;
}

__global__ void MatmulKernelI64(const int64_t* a, const int64_t* b,
                                int64_t* dst, int32_t rows, int32_t inner,
                                int32_t cols) {
    __shared__ int64_t tileA[16][16];
    __shared__ int64_t tileB[16][16];
    const int row = blockIdx.y * 16 + threadIdx.y;
    const int col = blockIdx.x * 16 + threadIdx.x;
    int64_t acc = 0;
    const int tiles = (inner + 15) / 16;
    for (int t = 0; t < tiles; ++t) {
        const int aCol = t * 16 + threadIdx.x;
        const int bRow = t * 16 + threadIdx.y;
        tileA[threadIdx.y][threadIdx.x] =
            row < rows && aCol < inner ? a[row * inner + aCol] : 0;
        tileB[threadIdx.y][threadIdx.x] =
            bRow < inner && col < cols ? b[bRow * cols + col] : 0;
        __syncthreads();
        for (int k = 0; k < 16; ++k)
            acc += tileA[threadIdx.y][k] * tileB[k][threadIdx.x];
        __syncthreads();
    }
    if (row < rows && col < cols)
        dst[row * cols + col] = acc;
}

//The strided matmul: the same 16x16 tiling as MatmulKernelF, but A and B
//are read per load through the recorded rank-2 strides (offset +
//i*strides[0] + j*strides[1] — the two-term fold; rank-2 is the only
//matmul rank, so no odometer). Any strided view (transpose, step-slice)
//reads correctly without materializing; a dense operand folds to the
//flat index, so either operand strided takes this kernel. The write
//stays dense (the dispatcher mints the output).
template <typename T>
__global__ void MatmulKernelStrided(DevView a, DevView b, DevView dst,
                                   int32_t rows, int32_t inner,
                                   int32_t cols) {
    __shared__ T tileA[16][16];
    __shared__ T tileB[16][16];
    const int row = blockIdx.y * 16 + threadIdx.y;
    const int col = blockIdx.x * 16 + threadIdx.x;
    T acc = 0;
    const int tiles = (inner + 15) / 16;
    for (int t = 0; t < tiles; ++t) {
        const int aCol = t * 16 + threadIdx.x;
        const int bRow = t * 16 + threadIdx.y;
        tileA[threadIdx.y][threadIdx.x] =
            row < rows && aCol < inner
                ? ((const T*)a.data)[a.offset + (int64_t)row * a.strides[0]
                      + (int64_t)aCol * a.strides[1]]
                : T(0);
        tileB[threadIdx.y][threadIdx.x] =
            bRow < inner && col < cols
                ? ((const T*)b.data)[b.offset + (int64_t)bRow * b.strides[0]
                      + (int64_t)col * b.strides[1]]
                : T(0);
        __syncthreads();
        for (int k = 0; k < 16; ++k)
            acc += tileA[threadIdx.y][k] * tileB[k][threadIdx.x];
        __syncthreads();
    }
    if (row < rows && col < cols)
        ((T*)dst.data)[dst.offset + (int64_t)row * cols + col] = acc;
}

//The offset-fold discipline (spec D1): a dense A/B takes the flat dense
//kernel with the offset folded into the base pointer; either strided
//takes the strided kernel (per-load fold, correct for a dense operand
//too — the dense branch is only taken when BOTH are dense, so the flat
//index is exact there).
cudaError_t LaunchMatmul(const LaunchView& a, const LaunchView& b,
                         const LaunchView& dst, int elemKindCode) {
    SetDeviceForCall launchRow(a.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    const DevView va = ToDevView(a);
    const DevView vb = ToDevView(b);
    const DevView vd = ToDevView(dst);
    const bool dense = IsDense(va) && IsDense(vb);
    const int32_t rows = a.dims[0];
    const int32_t inner = a.dims[1];
    const int32_t cols = b.dims[1];
    dim3 grid((cols + 15) / 16, (rows + 15) / 16);
    dim3 block(16, 16);
    if (dense) {
        if (elemKindCode == 1)
            MatmulKernelF<<<grid, block, 0, CurrentStream()>>>(
                (const float*)a.data + a.offset,
                (const float*)b.data + b.offset,
                (float*)dst.data + dst.offset, rows, inner, cols);
        else if (elemKindCode == 17)
            MatmulKernelF64<<<grid, block, 0, CurrentStream()>>>(
                (const double*)a.data + a.offset,
                (const double*)b.data + b.offset,
                (double*)dst.data + dst.offset, rows, inner, cols);
        else if (elemKindCode == 0)
            MatmulKernelI32<<<grid, block, 0, CurrentStream()>>>(
                (const int32_t*)a.data + a.offset,
                (const int32_t*)b.data + b.offset,
                (int32_t*)dst.data + dst.offset, rows, inner, cols);
        else if (elemKindCode == 15)
            MatmulKernelI64<<<grid, block, 0, CurrentStream()>>>(
                (const int64_t*)a.data + a.offset,
                (const int64_t*)b.data + b.offset,
                (int64_t*)dst.data + dst.offset, rows, inner, cols);
        else
            return cudaErrorNotSupported;
    } else {
        if (elemKindCode == 1)
            MatmulKernelStrided<float><<<grid, block, 0, CurrentStream()>>>(
                va, vb, vd, rows, inner, cols);
        else if (elemKindCode == 17)
            MatmulKernelStrided<double><<<grid, block, 0, CurrentStream()>>>(
                va, vb, vd, rows, inner, cols);
        else if (elemKindCode == 0)
            MatmulKernelStrided<int32_t><<<grid, block, 0, CurrentStream()>>>(
                va, vb, vd, rows, inner, cols);
        else if (elemKindCode == 15)
            MatmulKernelStrided<int64_t><<<grid, block, 0, CurrentStream()>>>(
                va, vb, vd, rows, inner, cols);
        else
            return cudaErrorNotSupported;
    }
    return cudaGetLastError();
}

//--- gather / eq / oneHot ------------------------------------------------

//The where kernel: mask != 0 picks x else y (numpy's truthiness). The
//three operands ride FlatAddress strided reads; the scalar arms read
//the kernel parameters. Async (no cell errors) — see LaunchCompare.
template <typename T, typename M>
__global__ void WhereKernel(DevView mask, DevView x, DevView y, DevView dst,
                            bool xScalar, bool yScalar, double sx,
                            double sy, int64_t isx, int64_t isy) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t am = FlatAddress(mask, i);
    const int64_t ad = dst.offset + i;
    const M pick = ((const M*)mask.data)[am];
    //The scalar fields: the float kinds read the double; the int kinds
    //read the exact int64 twin (the double roundtrip would lose
    //precision above 2^53).
    const T xs = std::is_integral_v<T> ? static_cast<T>(isx)
                                       : static_cast<T>(sx);
    const T ys = std::is_integral_v<T> ? static_cast<T>(isy)
                                       : static_cast<T>(sy);
    T v;
    if (pick != 0) {
        v = xScalar ? xs : ((const T*)x.data)[FlatAddress(x, i)];
    } else {
        v = yScalar ? ys : ((const T*)y.data)[FlatAddress(y, i)];
    }
    ((T*)dst.data)[ad] = v;
}

cudaError_t LaunchWhere(const LaunchView& mask, const LaunchView& x,
                        const LaunchView& y, const LaunchView& dst,
                        int elemKindCode, bool maskLong, bool xScalar,
                        bool yScalar, double sx, double sy, int64_t isx,
                        int64_t isy) {
    SetDeviceForCall launchRow(mask.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    //Async (no cell errors) — see LaunchCompare. The kind rides the
    //explicit code: a scalar arm's view is empty and cannot carry it.
    const DevView dm = ToDevView(mask);
    const DevView dx = ToDevView(x);
    const DevView dy = ToDevView(y);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    #define NLANG_WHERE_LAUNCH(T, M) \
        WhereKernel<T, M><<<(int)blocks, kThreads, 0, CurrentStream()>>>(\
            dm, dx, dy, dd, xScalar, yScalar, sx, sy, isx, isy)
    if (elemKindCode == 1) {
        if (maskLong) NLANG_WHERE_LAUNCH(float, int64_t);
        else NLANG_WHERE_LAUNCH(float, int32_t);
    } else if (elemKindCode == 17) {
        if (maskLong) NLANG_WHERE_LAUNCH(double, int64_t);
        else NLANG_WHERE_LAUNCH(double, int32_t);
    } else if (elemKindCode == 0) {
        if (maskLong) NLANG_WHERE_LAUNCH(int32_t, int64_t);
        else NLANG_WHERE_LAUNCH(int32_t, int32_t);
    } else {
        if (maskLong) NLANG_WHERE_LAUNCH(int64_t, int64_t);
        else NLANG_WHERE_LAUNCH(int64_t, int32_t);
    }
    #undef NLANG_WHERE_LAUNCH
    return cudaGetLastError();
}

//The take-per-position gather (the PyTorch gather shape): one thread
//per output element — the element's row-major ordinal over the
//non-axis dims decomposes onto the source address (the axis dim reads
//from the gathered index). Rank-1 degenerates to flat fancy indexing
//(no non-axis dims; the address is offset + index*strides[0]). The
//index bounds were validated host-side over the staged indices — the
//kernel is a pure copy (async group).
template <typename T>
__global__ void GatherKernel(DevView src, const int64_t* indices,
                             int32_t axis, DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    //Decompose the output ordinal over the non-axis dims (the odometer
    //division skips the gather axis; row-major, innermost first).
    int64_t addr = src.offset;
    int64_t rem = i;
    for (int32_t d = src.rank - 1; d >= 0; --d) {
        if (d == axis)
            continue;
        const int64_t dim = src.dims[d];
        const int64_t coord = dim > 0 ? rem % dim : 0;
        rem /= (dim > 0 ? dim : 1);
        addr += coord * src.strides[d];
    }
    const int64_t index = indices[i];
    ((T*)dst.data)[dst.offset + i] =
        ((const T*)src.data)[addr + index * src.strides[axis]];
}

//takeAlongAxis (the argsort consumer): out[line, k] =
//src[line, indices[line, k]] — per matched axis line. The indices
//arrive as the dispatcher's DENSE int32 mirror (bounds already
//checked host-side), H2D via the pool scratch. ASYNC (no cell
//errors — the pure strided copy).
template <typename T>
__global__ void TakeAlongKernel(DevView src, int32_t axis,
                                const int32_t* indices, DevView dst) {
    const int64_t cell = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (cell >= dst.elemCount)
        return;
    //The output is a dense fresh mint, so the cell ordinal IS the
    //row-major position — decompose it over the dims: every non-axis
    //coord rides both strides, the axis coord comes from the mirror.
    int64_t srcAddr = src.offset;
    int64_t dstAddr = dst.offset;
    int64_t rem = cell;
    for (int32_t d = dst.rank - 1; d >= 0; --d) {
        const int64_t dim = dst.dims[d];
        const int64_t coord = dim > 0 ? rem % dim : 0;
        rem /= (dim > 0 ? dim : 1);
        dstAddr += coord * dst.strides[d];
        if (d != axis)
            srcAddr += coord * src.strides[d];
    }
    srcAddr += static_cast<int64_t>(indices[cell]) * src.strides[axis];
    ((T*)dst.data)[dstAddr] = ((const T*)src.data)[srcAddr];
}

//assignInto (the slice-assignment write half): one thread per view
//cell, each write routed through the view's strides to the owning
//record — a slice view writes through to its base. The values ride
//the pool scratch H2D (dense, count cells — the dispatcher stages
//them host-side first). ASYNC (no cell errors — the writeability
//gate rode the host check).
template <typename T>
__global__ void AssignIntoKernel(DevView view, const T* values,
                                 int64_t count) {
    const int64_t cell = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (cell >= count)
        return;
    int64_t addr = view.offset;
    int64_t rem = cell;
    for (int32_t d = view.rank - 1; d >= 0; --d) {
        const int64_t dim = view.dims[d];
        const int64_t coord = dim > 0 ? rem % dim : 0;
        rem /= (dim > 0 ? dim : 1);
        addr += coord * view.strides[d];
    }
    ((T*)view.data)[addr] = values[cell];
}

cudaError_t LaunchAssignInto(const LaunchView& view,
                             const void* values, int elemKindCode) {
    SetDeviceForCall launchRow(view.device);
    //ASYNC (no cell errors — the writeability gate rode the host
    //validation). Empty view: no cells — no launch, no scratch.
    if (view.elemCount == 0)
        return cudaSuccess;
    const int64_t count = view.elemCount;
    const int cellBytes = elemKindCode == 17 || elemKindCode == 15 ? 8 : 4;
    void* dVals = PoolAlloc(static_cast<size_t>(count) * cellBytes,
        CurrentStream());
    if (dVals == nullptr)
        return cudaErrorMemoryAllocation;
    cudaMemcpyAsync(dVals, values, static_cast<size_t>(count) * cellBytes,
        cudaMemcpyHostToDevice, CurrentStream());
    const DevView dv = ToDevView(view);
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    if (elemKindCode == 1)
        AssignIntoKernel<float><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            dv, static_cast<const float*>(dVals), count);
    else if (elemKindCode == 17)
        AssignIntoKernel<double><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            dv, static_cast<const double*>(dVals), count);
    else if (elemKindCode == 0)
        AssignIntoKernel<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            dv, static_cast<const int32_t*>(dVals), count);
    else
        AssignIntoKernel<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            dv, static_cast<const int64_t*>(dVals), count);
    PoolFree(dVals, CurrentStream());
    return cudaGetLastError();
}

//The scalar arm: the constant rides the launch parameters — no values
//staging at all (the array arm's H2D row would ship count cells for a
//one-number broadcast).
template <typename T>
__global__ void AssignIntoScalarKernel(DevView view, T value,
                                       int64_t count) {
    const int64_t cell = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (cell >= count)
        return;
    int64_t addr = view.offset;
    int64_t rem = cell;
    for (int32_t d = view.rank - 1; d >= 0; --d) {
        const int64_t dim = view.dims[d];
        const int64_t coord = dim > 0 ? rem % dim : 0;
        rem /= (dim > 0 ? dim : 1);
        addr += coord * view.strides[d];
    }
    ((T*)view.data)[addr] = value;
}

cudaError_t LaunchAssignIntoScalar(const LaunchView& view, double sv,
                                   int64_t isv, int elemKindCode) {
    SetDeviceForCall launchRow(view.device);
    if (view.elemCount == 0)
        return cudaSuccess;
    const DevView dv = ToDevView(view);
    const int64_t count = view.elemCount;
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    if (elemKindCode == 1)
        AssignIntoScalarKernel<float><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(dv, static_cast<float>(sv), count);
    else if (elemKindCode == 17)
        AssignIntoScalarKernel<double><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(dv, sv, count);
    else if (elemKindCode == 0)
        AssignIntoScalarKernel<int32_t><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(dv, static_cast<int32_t>(isv), count);
    else
        AssignIntoScalarKernel<int64_t><<<(int)blocks, kThreads, 0,
            CurrentStream()>>>(dv, isv, count);
    return cudaGetLastError();
}

cudaError_t LaunchTakeAlong(const LaunchView& src, int axis,
                            const int32_t* indices,
                            const LaunchView& dst) {
    SetDeviceForCall launchRow(src.device);
    //Async (no cell errors — the bounds rode the host validation).
    //Empty index set: the out has no cells — no launch, no scratch (a
    //zero-byte PoolAlloc would misread cudaMalloc(0)'s NULL as OOM).
    if (dst.elemCount == 0)
        return cudaSuccess;
    const int64_t count = dst.elemCount;
    int32_t* dIdx = static_cast<int32_t*>(
        PoolAlloc(count * sizeof(int32_t), CurrentStream()));
    if (dIdx == nullptr)
        return cudaErrorMemoryAllocation;
    cudaMemcpyAsync(dIdx, indices, count * sizeof(int32_t),
        cudaMemcpyHostToDevice, CurrentStream());
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    if (src.elemKind == 1)
        TakeAlongKernel<float><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, axis, dIdx, dd);
    else if (src.elemKind == 17)
        TakeAlongKernel<double><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, axis, dIdx, dd);
    else if (src.elemKind == 0)
        TakeAlongKernel<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, axis, dIdx, dd);
    else
        TakeAlongKernel<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, axis, dIdx, dd);
    PoolFree(dIdx, CurrentStream());
    return cudaGetLastError();
}

cudaError_t LaunchGather(const LaunchView& src, int axis,
                         const LaunchView& indices,
                         const LaunchView& dst) {
    SetDeviceForCall launchRow(src.device);
    //Async (no cell errors — the bounds rode the host validation).
    //Empty index set: the out has no cells — no launch, no scratch (a
    //zero-byte PoolAlloc would misread cudaMalloc(0)'s NULL as OOM).
    if (indices.elemCount == 0)
        return cudaSuccess;
    //The indices land host-side (the dispatcher staged them); widen
    //int32 to int64 and H2D via the pool scratch.
    const int64_t count = indices.elemCount;
    int64_t* dIdx = static_cast<int64_t*>(
        PoolAlloc(count * sizeof(int64_t), CurrentStream()));
    if (dIdx == nullptr)
        return cudaErrorMemoryAllocation;
    std::vector<int64_t> wide(count);
    const int32_t* cells = (const int32_t*)indices.data;
    const int64_t addr0 = indices.offset;
    for (int64_t i = 0; i < count; ++i)
        wide[i] = cells[addr0 + i];
    cudaMemcpyAsync(dIdx, wide.data(), count * sizeof(int64_t),
        cudaMemcpyHostToDevice, CurrentStream());
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (src.elemKind == 1)
        GatherKernel<float><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, dIdx, axis, dd);
    else if (src.elemKind == 17)
        GatherKernel<double><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, dIdx, axis, dd);
    else if (src.elemKind == 0)
        GatherKernel<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, dIdx, axis, dd);
    else
        GatherKernel<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, dIdx, axis, dd);
    PoolFree(dIdx, CurrentStream());
    return cudaGetLastError();
}

//The stable ascending line sort (the device arm): one thread per
//LINE — the line's elements gather into the thread's local buffer, an
//in-thread insertion sort orders them (stable; NaN to the end per the
//CPU walk's SortLess rule), and the line scatters back to the output's
//dense coordinates. The line length is capped: the dispatcher routes
//lines above kSortMaxLine to the host walk (the insertion sort is
//teaching-scale).
//The device-side line entry (the value plus the original ordinal —
//the ordinal feeds argsort and makes the insertion sort stable).
template <typename T>
struct SortLineEntry {
    T value;
    int32_t ordinal;
};

//The device-side comparison (the CPU walk's SortLess rule inlined).
template <typename T>
__device__ bool SortLessDev(T a, T b) {
    if constexpr (std::is_same_v<T, float> || std::is_same_v<T, double>) {
        const bool aNan = isnan(a);
        const bool bNan = isnan(b);
        if (aNan)
            return false;
        if (bNan)
            return true;
    }
    return a < b;
}

template <typename T>
__global__ void SortAxisKernel(DevView src, int32_t axis, DevView dst,
                               bool withIndices) {
    const int64_t line = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    const int64_t lineCount = dst.elemCount / dst.dims[axis];
    if (line >= lineCount)
        return;
    //The line base: the output ordinal decomposes over the non-axis
    //dims (the odometer division skips the axis dim).
    int64_t srcAddr = src.offset;
    int64_t dstAddr = dst.offset;
    int64_t rem = line;
    for (int32_t d = src.rank - 1; d >= 0; --d) {
        if (d == axis)
            continue;
        const int64_t dim = src.dims[d];
        const int64_t coord = dim > 0 ? rem % dim : 0;
        rem /= (dim > 0 ? dim : 1);
        srcAddr += coord * src.strides[d];
        dstAddr += coord * dst.strides[d];
    }
    const int64_t along = src.dims[axis];
    SortLineEntry<T> entries[tensor::TensorOps::kSortMaxLine];
    for (int64_t k = 0; k < along; ++k) {
        const T v = ((const T*)src.data)[
            srcAddr + k * src.strides[axis]];
        //The insertion position: after every strictly-smaller element
        //and every equal element (stable), after every non-NaN (a NaN
        //goes to the end), before another NaN.
        int64_t pos = 0;
        //The stable insertion point: the first element strictly greater
        //than v (equal elements keep the newcomer after them — the
        //original ordinals stay in order).
        while (pos < k && !SortLessDev(v, entries[pos].value))
            ++pos;
        for (int64_t m = k; m > pos; --m)
            entries[m] = entries[m - 1];
        entries[pos] = {v, static_cast<int32_t>(k)};
    }
    for (int64_t k = 0; k < along; ++k) {
        if (withIndices)
            static_cast<int32_t*>(dst.data)[
                dstAddr + k * dst.strides[axis]] = entries[k].ordinal;
        else
            ((T*)dst.data)[
                dstAddr + k * dst.strides[axis]] = entries[k].value;
    }
}

cudaError_t LaunchSort(bool withIndices, const LaunchView& src,
                       int axis, const LaunchView& dst) {
    SetDeviceForCall launchRow(src.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (src.elemCount == 0)
        return cudaSuccess;
    //Async (no cell errors). One thread per line; the line count is
    //the output's element count over the line length.
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t lineCount = dst.elemCount / ds.dims[axis];
    const int64_t blocks = (lineCount + kThreads - 1) / kThreads;
    if (src.elemKind == 1)
        SortAxisKernel<float><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, axis, dd, withIndices);
    else if (src.elemKind == 17)
        SortAxisKernel<double><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, axis, dd, withIndices);
    else if (src.elemKind == 0)
        SortAxisKernel<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, axis, dd, withIndices);
    else
        SortAxisKernel<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ds, axis, dd, withIndices);
    return cudaGetLastError();
}

//The per-point compare (six ops). IEEE ordering for the float kinds:
//the C++ comparison operators already give the NaN rule (false in every
//ordered compare and in Eq, true in Ne) on the device, matching the CPU
//walk's ApplyCompare.
__device__ int32_t DevCompare(CompareOp op, float a, float b) {
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

__device__ int32_t DevCompare(CompareOp op, double a, double b) {
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

__device__ int32_t DevCompare(CompareOp op, int32_t a, int32_t b) {
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

__device__ int32_t DevCompare(CompareOp op, int64_t a, int64_t b) {
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
__global__ void CompareKernel(CompareOp op, DevView a, DevView b,
                              DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t aa = FlatAddress(a, i);
    const int64_t ab = FlatAddress(b, i);
    const int64_t ad = dst.offset + i;
    ((int32_t*)dst.data)[ad] = DevCompare(op,
        ((const T*)a.data)[aa], ((const T*)b.data)[ab]);
}

cudaError_t LaunchCompare(CompareOp op, const LaunchView& a,
                          const LaunchView& b, const LaunchView& dst) {
    SetDeviceForCall launchRow(a.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    //Differentiated sync (spec 2026-11-16 §3): comparisons raise no
    //cell errors — enqueue and go.
    const DevView da = ToDevView(a);
    const DevView db = ToDevView(b);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (a.elemKind == 1)
        CompareKernel<float><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, db, dd);
    else if (a.elemKind == 17)
        CompareKernel<double><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, db, dd);
    else if (a.elemKind == 0)
        CompareKernel<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, db, dd);
    else
        CompareKernel<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, db, dd);
    return cudaGetLastError();
}

//The scalar arm: one odometer over the array (the scalar rides the
//kernel parameter — no second view).
template <typename T>
__global__ void CompareScalarKernel(CompareOp op, DevView a, double scalar,
                                    int64_t iScalar, DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t aa = FlatAddress(a, i);
    const int64_t ad = dst.offset + i;
    //The scalar fields: the float kinds read the double; the int kinds
    //read the exact int64 twin (the double roundtrip would lose
    //precision above 2^53).
    const T s = std::is_integral_v<T> ? static_cast<T>(iScalar)
                                      : static_cast<T>(scalar);
    ((int32_t*)dst.data)[ad] = DevCompare(op, ((const T*)a.data)[aa], s);
}

cudaError_t LaunchCompareScalar(CompareOp op, const LaunchView& a,
                                double scalar, int64_t iScalar,
                                const LaunchView& dst) {
    SetDeviceForCall launchRow(a.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    //Async (no cell errors) — see LaunchCompare.
    const DevView da = ToDevView(a);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (a.elemKind == 1)
        CompareScalarKernel<float><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, scalar, iScalar, dd);
    else if (a.elemKind == 17)
        CompareScalarKernel<double><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, scalar, iScalar, dd);
    else if (a.elemKind == 0)
        CompareScalarKernel<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, scalar, iScalar, dd);
    else
        CompareScalarKernel<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            op, da, scalar, iScalar, dd);
    return cudaGetLastError();
}

__global__ void OneHotKernel(const int32_t* labels, int64_t count,
                             int32_t n, float* dst, int32_t* errorOut) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    const int32_t label = labels[i];
    if (label < 0 || label >= n) {
        errorOut[0] = kErrOOB;
        errorOut[1] = i;
        return;
    }
    for (int32_t j = 0; j < n; ++j)
        dst[(int64_t)i * n + j] = j == label ? 1.0f : 0.0f;
}

cudaError_t LaunchOneHot(const LaunchView& labels, int32_t n,
                         const LaunchView& dst, int32_t* errorOut) {
    //Pin from the destination, not the labels: the dispatcher stages the
    //labels to host memory, so their device id names the staging side,
    //never the launch target. The minted output's residency is the
    //device that must run the kernel — it writes that output, and the
    //host-to-device label copy feeds it from host memory.
    SetDeviceForCall launchRow(dst.device);
    const int64_t count = labels.elemCount;
    //Empty labels: the [0, n) output has no cells — no launch, no
    //scratch (see LaunchSumAll for the zero-block rationale).
    if (count == 0)
        return cudaSuccess;
    int32_t* dLabels = static_cast<int32_t*>(
        PoolAlloc(count * sizeof(int32_t), CurrentStream()));
    if (dLabels == nullptr)
        return cudaErrorMemoryAllocation;
    cudaMemcpyAsync(dLabels, (const int32_t*)labels.data,
        count * sizeof(int32_t), cudaMemcpyHostToDevice, CurrentStream());
    const uint32_t none[2] = {kErrNone, 0};
    cudaMemcpyAsync(CurrentErrorFlags(), none, 2 * sizeof(int32_t),
        cudaMemcpyHostToDevice, CurrentStream());
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    OneHotKernel<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
        dLabels, count, n, (float*)dst.data, CurrentErrorFlags());
    cudaError_t err = cudaStreamSynchronize(CurrentStream());
    PoolFree(dLabels, CurrentStream());
    if (err != cudaSuccess)
        return err;
    //Explicit cast: kErrNone is uint32 0xFFFFFFFF, narrowed to the int32
    //sentinel; the value is fully overwritten by the device copy below, so
    //this only needs to be well-formed (GCC enforces the narrowing constraint
    //that MSVC only warns about).
    int32_t flags[2] = {static_cast<int32_t>(kErrNone), 0};
    cudaMemcpy(flags, CurrentErrorFlags(), 2 * sizeof(int32_t), cudaMemcpyDeviceToHost);
    if (flags[0] != kErrNone)
        *errorOut = flags[1];
    return cudaSuccess;
}

//--- uniform (the MT19937 snapshot replica) ------------------------------

__global__ void UniformKernel(double* dst, int64_t count, double low,
                              double span, uint32_t* mtState,
                              uint32_t position) {
    //Regenerate the block from the snapshot position: the words before
    //position were already tempered by the host; the twist regenerates
    //the whole block, then each thread tempers its assigned words.
    MtGenerateBlock(mtState);
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    const int64_t word = position + i;
    const uint32_t y = MtTemper(mtState[word % 624]);
    const double u = (double)(y >> 8) * (1.0 / 16777216.0);
    dst[i] = low + span * u;
}

__global__ void UniformKernelF(float* dst, int64_t count, float low,
                               float span, uint32_t* mtState,
                               uint32_t position) {
    MtGenerateBlock(mtState);
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    const int64_t word = position + i;
    const uint32_t y = MtTemper(mtState[word % 624]);
    const float u = (float)(y >> 8) * (1.0f / 16777216.0f);
    dst[i] = low + span * u;
}

cudaError_t LaunchUniform(const LaunchView& dst, double low,
                          double high, const uint32_t* mt624,
                          uint32_t position, bool isDouble) {
    SetDeviceForCall launchRow(dst.device);
    const int64_t count = dst.elemCount;
    //Empty draw: no cells to fill — no launch, no scratch (see
    //LaunchSumAll for the zero-block rationale).
    if (count == 0)
        return cudaSuccess;
    uint32_t* dState = static_cast<uint32_t*>(
        PoolAlloc(624 * sizeof(uint32_t), CurrentStream()));
    if (dState == nullptr)
        return cudaErrorMemoryAllocation;
    cudaMemcpyAsync(dState, mt624, 624 * sizeof(uint32_t),
        cudaMemcpyHostToDevice, CurrentStream());
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    double* dOut = (double*)dst.data;
    if (isDouble) {
        UniformKernel<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            dOut, count, low, high - low, dState, position);
    } else {
        UniformKernelF<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            (float*)dst.data, count, (float)low, (float)(high - low),
            dState, position);
    }
    const cudaError_t err = cudaGetLastError();
    PoolFree(dState, CurrentStream());
    return err;
}

//--- generation & movement ----------------------------------------------

//The scatter kernel (rank-1): one thread per index — the bounds check
//(both bounds, no negative wrap) rides the cell error flags; the value
//is the scalar arm's field or the values array's cell. SYNC-GROUP: the
//flags drain per op (the named OutOfBounds raise stays at this op).
template <typename T>
__global__ void ScatterKernel(DevView src, const int64_t* indices,
                              int64_t indexCount, DevView dst,
                              DevView values, double value, int64_t iValue,
                              bool valuesScalar, int64_t along,
                              int32_t* errorOut) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= indexCount)
        return;
    const int64_t index = indices[i];
    if (index < 0 || index >= along) {
        if (errorOut[0] == kErrNone) {
            errorOut[0] = kErrOOB;
            errorOut[1] = static_cast<int32_t>(i);
        }
        return;
    }
    //Rank-1 flat addressing: the scattered cell is dst.flat[index]
    //(the dispatcher gates rank-1; the copy phase already filled the
    //rest of the payload).
    //The scalar fields: the float kinds read the double; the int
    //kinds read the exact int64 twin (the double roundtrip would lose
    //precision above 2^53).
    const T v = valuesScalar
        ? (std::is_integral_v<T> ? static_cast<T>(iValue)
                                 : static_cast<T>(value))
        : ((const T*)values.data)[values.offset + i];
    ((T*)dst.data)[dst.offset + index] = v;
}

//scatterAdd's accumulate twin: per position, dst.flat[index] += v —
//duplicate indices SUM by the atomics' contract (the gradient
//spelling; PyTorch's scatter_add_ semantics). SYNC-GROUP like
//scatter: the error flags drain at this op. Float accumulation ORDER
//under contention is nondeterministic (ULP-level) — PyTorch documents
//the same.
template <typename T>
__global__ void ScatterAddKernel(DevView src, const int64_t* indices,
                                 int64_t indexCount, DevView dst,
                                 DevView values, double value,
                                 int64_t iValue, bool valuesScalar,
                                 int64_t along, int32_t* errorOut) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= indexCount)
        return;
    const int64_t index = indices[i];
    if (index < 0 || index >= along) {
        if (errorOut[0] == kErrNone) {
            errorOut[0] = kErrOOB;
            errorOut[1] = static_cast<int32_t>(i);
        }
        return;
    }
    //Rank-1 flat addressing (the dispatcher gates rank-1; the copy
    //phase already filled the rest of the payload). The accumulate:
    //atomicAdd has native overloads for float/double/int32; int64
    //goes through the unsigned long long overload — the addition is
    //two's-complement bit-identical, so the signed sum is exact.
    const T v = valuesScalar
        ? (std::is_integral_v<T> ? static_cast<T>(iValue)
                                 : static_cast<T>(value))
        : ((const T*)values.data)[values.offset + i];
    T* cell = ((T*)dst.data) + dst.offset + index;
    if constexpr (std::is_same_v<T, int64_t>) {
        atomicAdd(reinterpret_cast<unsigned long long*>(cell),
            static_cast<unsigned long long>(v));
    } else {
        atomicAdd(cell, v);
    }
}

cudaError_t LaunchScatter(const LaunchView& a, const LaunchView& indices,
                          const LaunchView& values, const LaunchView& dst,
                          bool valuesScalar, double sv, int64_t isv,
                          int32_t* errorOut) {
    SetDeviceForCall launchRow(a.device);
    const int cellBytes = a.elemKind == 17 || a.elemKind == 15 ? 8 : 4;
    //Zero indices: no index kernel (a zero-block grid is an invalid
    //configuration) and no index/values scratch — but the rank-1
    //value form still owes the dense span copy that seeds the result
    //(the host path returns a copy of `a`; the into form's copy is
    //the self-copy no-op). Skip the index work, not the copy.
    if (indices.elemCount == 0) {
        cudaMemcpyAsync(
            static_cast<uint8_t*>(dst.data)
                + static_cast<size_t>(dst.offset) * cellBytes,
            static_cast<const uint8_t*>(a.data)
                + static_cast<size_t>(a.offset) * cellBytes,
            static_cast<size_t>(a.elemCount) * cellBytes,
            cudaMemcpyDeviceToDevice, CurrentStream());
        return cudaSuccess;
    }
    //SYNC-GROUP (spec 2026-11-16 §3): scatter writes the per-cell
    //out-of-range flag — clear-launch-sync-read drains at this op.
    const int64_t count = indices.elemCount;
    const int64_t along = a.dims[0];
    int64_t* dIdx = static_cast<int64_t*>(
        PoolAlloc(count * sizeof(int64_t), CurrentStream()));
    if (dIdx == nullptr)
        return cudaErrorMemoryAllocation;
    //The indices view is int32 — read on the host and widen.
    std::vector<int64_t> wide(count);
    const int32_t* cells = static_cast<const int32_t*>(indices.data);
    const int64_t addr0 = indices.offset;
    for (int64_t i = 0; i < count; ++i)
        wide[i] = cells[addr0 + i];
    cudaMemcpyAsync(dIdx, wide.data(), count * sizeof(int64_t),
        cudaMemcpyHostToDevice, CurrentStream());
    //The values arm: the dispatcher stages a non-scalar values view to
    //HOST (the metadata pattern) — H2D it here so the kernel reads a
    //device pointer; a scalar arm carries the value in the parameter
    //and the view stays empty.
    void* dVals = nullptr;
    if (!valuesScalar) {
        dVals = PoolAlloc(static_cast<size_t>(count) * cellBytes, CurrentStream());
        if (dVals == nullptr) {
            PoolFree(dIdx, CurrentStream());
            return cudaErrorMemoryAllocation;
        }
        cudaMemcpyAsync(dVals,
            static_cast<const uint8_t*>(values.data)
                + static_cast<size_t>(values.offset) * cellBytes,
            static_cast<size_t>(count) * cellBytes,
            cudaMemcpyHostToDevice, CurrentStream());
    }
    //The copy phase: a dense rank-1 span copy (the dispatcher gates
    //rank-1; the offset folds into the base pointers).
    cudaMemcpyAsync(
        static_cast<uint8_t*>(dst.data)
            + static_cast<size_t>(dst.offset) * cellBytes,
        static_cast<const uint8_t*>(a.data)
            + static_cast<size_t>(a.offset) * cellBytes,
        static_cast<size_t>(a.elemCount) * cellBytes,
        cudaMemcpyDeviceToDevice, CurrentStream());
    const uint32_t none[2] = {kErrNone, 0};
    cudaMemcpy(CurrentErrorFlags(), none, 2 * sizeof(int32_t), cudaMemcpyHostToDevice);
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    //The kernel's values operand: the H2D scratch (array arm) or an
    //empty view (the scalar arm never reads it).
    LaunchView valuesDev = values;
    if (!valuesScalar)
        valuesDev.data = dVals;
    valuesDev.offset = 0;
    if (a.elemKind == 1)
        ScatterKernel<float><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ToDevView(a), dIdx, count, ToDevView(dst),
            ToDevView(valuesDev), sv, isv, valuesScalar, along, CurrentErrorFlags());
    else if (a.elemKind == 17)
        ScatterKernel<double><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ToDevView(a), dIdx, count, ToDevView(dst),
            ToDevView(valuesDev), sv, isv, valuesScalar, along, CurrentErrorFlags());
    else if (a.elemKind == 0)
        ScatterKernel<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ToDevView(a), dIdx, count, ToDevView(dst),
            ToDevView(valuesDev), sv, isv, valuesScalar, along, CurrentErrorFlags());
    else
        ScatterKernel<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ToDevView(a), dIdx, count, ToDevView(dst),
            ToDevView(valuesDev), sv, isv, valuesScalar, along, CurrentErrorFlags());
    cudaError_t err = cudaStreamSynchronize(CurrentStream());
    if (dVals != nullptr)
        PoolFree(dVals, CurrentStream());
    PoolFree(dIdx, CurrentStream());
    if (err != cudaSuccess)
        return err;
    //Explicit cast: kErrNone is uint32 0xFFFFFFFF, narrowed to the int32
    //sentinel; the value is fully overwritten by the device copy below, so
    //this only needs to be well-formed (GCC enforces the narrowing constraint
    //that MSVC only warns about).
    int32_t flags[2] = {static_cast<int32_t>(kErrNone), 0};
    cudaMemcpy(flags, CurrentErrorFlags(), 2 * sizeof(int32_t), cudaMemcpyDeviceToHost);
    if (flags[0] != kErrNone)
        *errorOut = flags[1];
    return cudaSuccess;
}

cudaError_t LaunchScatterAdd(const LaunchView& a, const LaunchView& indices,
                             const LaunchView& values, const LaunchView& dst,
                             bool valuesScalar, double sv, int64_t isv,
                             int32_t* errorOut) {
    SetDeviceForCall launchRow(a.device);
    const int cellBytes = a.elemKind == 17 || a.elemKind == 15 ? 8 : 4;
    //Zero indices: no index kernel (a zero-block grid is an invalid
    //configuration) and no index/values scratch — but the rank-1
    //value form still owes the dense span copy that seeds the result
    //(the host path returns a copy of `a`; the into form's copy is
    //the self-copy no-op). Skip the index work, not the copy.
    if (indices.elemCount == 0) {
        cudaMemcpyAsync(
            static_cast<uint8_t*>(dst.data)
                + static_cast<size_t>(dst.offset) * cellBytes,
            static_cast<const uint8_t*>(a.data)
                + static_cast<size_t>(a.offset) * cellBytes,
            static_cast<size_t>(a.elemCount) * cellBytes,
            cudaMemcpyDeviceToDevice, CurrentStream());
        return cudaSuccess;
    }
    //SYNC-GROUP (the scatter shape): scatterAdd writes the per-cell
    //out-of-range flag — clear-launch-sync-read drains at this op.
    const int64_t count = indices.elemCount;
    const int64_t along = a.dims[0];
    int64_t* dIdx = static_cast<int64_t*>(
        PoolAlloc(count * sizeof(int64_t), CurrentStream()));
    if (dIdx == nullptr)
        return cudaErrorMemoryAllocation;
    //The indices view is int32 — read on the host and widen.
    std::vector<int64_t> wide(count);
    const int32_t* cells = static_cast<const int32_t*>(indices.data);
    const int64_t addr0 = indices.offset;
    for (int64_t i = 0; i < count; ++i)
        wide[i] = cells[addr0 + i];
    cudaMemcpyAsync(dIdx, wide.data(), count * sizeof(int64_t),
        cudaMemcpyHostToDevice, CurrentStream());
    //The values arm: the dispatcher stages a non-scalar values view to
    //HOST (the metadata pattern) — H2D it here so the kernel reads a
    //device pointer; a scalar arm carries the value in the parameter
    //and the view stays empty.
    void* dVals = nullptr;
    if (!valuesScalar) {
        dVals = PoolAlloc(static_cast<size_t>(count) * cellBytes, CurrentStream());
        if (dVals == nullptr) {
            PoolFree(dIdx, CurrentStream());
            return cudaErrorMemoryAllocation;
        }
        cudaMemcpyAsync(dVals,
            static_cast<const uint8_t*>(values.data)
                + static_cast<size_t>(values.offset) * cellBytes,
            static_cast<size_t>(count) * cellBytes,
            cudaMemcpyHostToDevice, CurrentStream());
    }
    //The copy phase: a dense rank-1 span copy (the value-semantics
    //form fills the rest of the payload; the into form's dispatcher
    //passes dst = a — the self-copy is the scatterInto precedent's
    //harmless no-op).
    cudaMemcpyAsync(
        static_cast<uint8_t*>(dst.data)
            + static_cast<size_t>(dst.offset) * cellBytes,
        static_cast<const uint8_t*>(a.data)
            + static_cast<size_t>(a.offset) * cellBytes,
        static_cast<size_t>(a.elemCount) * cellBytes,
        cudaMemcpyDeviceToDevice, CurrentStream());
    const uint32_t none[2] = {kErrNone, 0};
    cudaMemcpy(CurrentErrorFlags(), none, 2 * sizeof(int32_t), cudaMemcpyHostToDevice);
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    //The kernel's values operand: the H2D scratch (array arm) or an
    //empty view (the scalar arm never reads it).
    LaunchView valuesDev = values;
    if (!valuesScalar)
        valuesDev.data = dVals;
    valuesDev.offset = 0;
    if (a.elemKind == 1)
        ScatterAddKernel<float><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ToDevView(a), dIdx, count, ToDevView(dst),
            ToDevView(valuesDev), sv, isv, valuesScalar, along, CurrentErrorFlags());
    else if (a.elemKind == 17)
        ScatterAddKernel<double><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ToDevView(a), dIdx, count, ToDevView(dst),
            ToDevView(valuesDev), sv, isv, valuesScalar, along, CurrentErrorFlags());
    else if (a.elemKind == 0)
        ScatterAddKernel<int32_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ToDevView(a), dIdx, count, ToDevView(dst),
            ToDevView(valuesDev), sv, isv, valuesScalar, along, CurrentErrorFlags());
    else
        ScatterAddKernel<int64_t><<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            ToDevView(a), dIdx, count, ToDevView(dst),
            ToDevView(valuesDev), sv, isv, valuesScalar, along, CurrentErrorFlags());
    cudaError_t err = cudaStreamSynchronize(CurrentStream());
    if (dVals != nullptr)
        PoolFree(dVals, CurrentStream());
    PoolFree(dIdx, CurrentStream());
    if (err != cudaSuccess)
        return err;
    //Explicit cast: kErrNone is uint32 0xFFFFFFFF, narrowed to the int32
    //sentinel; the value is fully overwritten by the device copy below, so
    //this only needs to be well-formed (GCC enforces the narrowing constraint
    //that MSVC only warns about).
    int32_t flags[2] = {static_cast<int32_t>(kErrNone), 0};
    cudaMemcpy(flags, CurrentErrorFlags(), 2 * sizeof(int32_t), cudaMemcpyDeviceToHost);
    if (flags[0] != kErrNone)
        *errorOut = flags[1];
    return cudaSuccess;
}

__global__ void FillKernel(int elemKindCode, DevView dst,
                           double value, int64_t iValue) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t addr = dst.offset + i;
    if (elemKindCode == 0)
        ((int32_t*)dst.data)[addr] = (int32_t)iValue;
    else if (elemKindCode == 15)
        ((int64_t*)dst.data)[addr] = iValue;
    else if (elemKindCode == 1)
        ((float*)dst.data)[addr] = (float)value;
    else
        ((double*)dst.data)[addr] = value;
}

cudaError_t LaunchFill(const LaunchView& dst, double value,
                       int64_t iValue, int elemKindCode) {
    SetDeviceForCall launchRow(dst.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    FillKernel<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
        elemKindCode, dd, value, iValue);
    return cudaGetLastError();
}

__global__ void ArangeKernelD(double* dst, int64_t count, double start,
                              double step) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    dst[i] = start + (double)i * step;
}

//The float arange: direct formula start + i*step (NumPy's approach, matching
//the double kernel above; the CPU's historical cumulative rounding differs
//by ULPs for i>=2 — the documented deviation).
__global__ void ArangeKernelF(float* dst, int64_t count, float start,
                              float step) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    dst[i] = start + (float)i * step;
}

__global__ void ArangeKernelI32(int32_t* dst, int64_t count,
                                int64_t start, int64_t step) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    dst[i] = (int32_t)(start + i * step);
}

__global__ void ArangeKernelI64(int64_t* dst, int64_t count,
                                int64_t start, int64_t step) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    dst[i] = start + i * step;
}

cudaError_t LaunchArange(const LaunchView& dst, double start,
                         double step, bool isInt) {
    SetDeviceForCall launchRow(dst.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output is already correct — launch
    //nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (isInt) {
        if (dst.elemKind == 0)
            ArangeKernelI32<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
                (int32_t*)dst.data, dst.elemCount, (int64_t)start,
                (int64_t)step);
        else
            ArangeKernelI64<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
                (int64_t*)dst.data, dst.elemCount, (int64_t)start,
                (int64_t)step);
    } else {
        if (dst.elemKind == 1)
            ArangeKernelF<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
                (float*)dst.data, dst.elemCount, (float)start, (float)step);
        else
            ArangeKernelD<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
                (double*)dst.data, dst.elemCount, start, step);
    }
    return cudaGetLastError();
}

__global__ void CopyKernel(int elemKindCode, DevView src, DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t sa = FlatAddress(src, i);
    const int64_t sd = dst.offset + i;
    if (elemKindCode == 1)
        ((float*)dst.data)[sd] = ((const float*)src.data)[sa];
    else if (elemKindCode == 17)
        ((double*)dst.data)[sd] = ((const double*)src.data)[sa];
    else if (elemKindCode == 0)
        ((int32_t*)dst.data)[sd] = ((const int32_t*)src.data)[sa];
    else
        ((int64_t*)dst.data)[sd] = ((const int64_t*)src.data)[sa];
}

cudaError_t LaunchCopy(const LaunchView& src, const LaunchView& dst,
                       int elemKindCode) {
    SetDeviceForCall launchRow(src.device);
    //Zero cells to launch: a zero-block grid is an invalid
    //configuration, and the minted output (or the no-op write) is
    //already correct — launch nothing.
    if (dst.elemCount == 0)
        return cudaSuccess;
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    CopyKernel<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
        elemKindCode, ds, dd);
    return cudaGetLastError();
}

//The per-cell kind write shared by the concat paths: both views arrive
//already folded to flat element addresses.
__device__ void WriteConcatCell(int elemKindCode, void* dstData,
                                int64_t sd, const void* srcData,
                                int64_t sa) {
    if (elemKindCode == 1)
        ((float*)dstData)[sd] = ((const float*)srcData)[sa];
    else if (elemKindCode == 17)
        ((double*)dstData)[sd] = ((const double*)srcData)[sa];
    else if (elemKindCode == 0)
        ((int32_t*)dstData)[sd] = ((const int32_t*)srcData)[sa];
    else
        ((int64_t*)dstData)[sd] = ((const int64_t*)srcData)[sa];
}

//The join kernel (one element per thread, any axis, any rank): the
//destination flat folds to logical coordinates (row-major over the
//destination dims); the join-axis coordinate picks the operand and
//re-bases into that operand's dims (the dispatcher checked that the
//dims agree everywhere but at the join axis); the coordinates then
//fold row-major into the source's flat ordinal and through the
//source's strides (FlatAddress), so strided arms ride their own
//layouts. The write is dense (the dispatcher mints dst). Axis 0 falls
//out as the degenerate case: the fold reproduces the flat halves.
__global__ void ConcatJoinKernel(int elemKindCode, DevView a, DevView b,
                                 DevView dst, int axis,
                                 int64_t aAxisDim) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    int64_t coord[kMaxRank];
    int64_t rem = i;
    for (int32_t d = dst.rank - 1; d >= 0; --d) {
        const int64_t dim = dst.dims[d];
        coord[d] = dim > 0 ? rem % dim : 0;
        rem /= (dim > 0 ? dim : 1);
    }
    const bool fromA = coord[axis] < aAxisDim;
    const DevView& src = fromA ? a : b;
    if (!fromA)
        coord[axis] -= aAxisDim;
    int64_t ord = 0;
    int64_t fold = 1;
    for (int32_t d = src.rank - 1; d >= 0; --d) {
        ord += coord[d] * fold;
        fold *= (src.dims[d] > 0 ? src.dims[d] : 1);
    }
    WriteConcatCell(elemKindCode, dst.data, dst.offset + i, src.data,
                    FlatAddress(src, ord));
}

//concat: dense operands joined along the outermost axis take the two
//D2D span copies (the offset folded into the base — a dense view's
//logical cells are a contiguous run from its offset); every other join
//— any axis, either operand strided — takes the join kernel. The host
//dispatcher gated the axis range and the non-join dims, so the kernel
//sees only well-shaped joins (the old axis-0-only gate raised
//NotSupported on inner axes the host path handles).
cudaError_t LaunchConcat(const LaunchView& a, const LaunchView& b,
                         int axis, const LaunchView& dst,
                         int elemKindCode) {
    SetDeviceForCall launchRow(a.device);
    const int64_t aCount = a.elemCount;
    const int64_t bCount = b.elemCount;
    //Empty join: the output is minted, no cells to write (a zero-block
    //launch would be an invalid configuration).
    if (dst.elemCount == 0)
        return cudaSuccess;
    //Cell width by KIND (the old 1/0 hard-coding copied half the
    //bytes for f64/i64).
    const int cellBytes = (elemKindCode == 17 || elemKindCode == 15)
        ? 8 : 4;
    const DevView da = ToDevView(a);
    const DevView db = ToDevView(b);
    if (axis == 0 && IsDense(da) && IsDense(db)) {
        const uint8_t* aBase =
            static_cast<const uint8_t*>(a.data)
            + static_cast<size_t>(a.offset) * cellBytes;
        const uint8_t* bBase =
            static_cast<const uint8_t*>(b.data)
            + static_cast<size_t>(b.offset) * cellBytes;
        cudaMemcpyAsync(dst.data, aBase, aCount * cellBytes,
            cudaMemcpyDeviceToDevice, CurrentStream());
        cudaMemcpyAsync((uint8_t*)dst.data + aCount * cellBytes,
            bBase, bCount * cellBytes, cudaMemcpyDeviceToDevice,
            CurrentStream());
    } else {
        const DevView dd = ToDevView(dst);
        const int64_t blocks =
            (dst.elemCount + kThreads - 1) / kThreads;
        ConcatJoinKernel<<<(int)blocks, kThreads, 0, CurrentStream()>>>(
            elemKindCode, da, db, dd, axis, da.dims[axis]);
    }
    return cudaGetLastError();
}

} // namespace tensor_cuda

