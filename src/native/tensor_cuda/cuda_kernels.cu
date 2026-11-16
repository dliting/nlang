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

    Every launcher: launch on the single stream, cudaStreamSynchronize
    (one op one sync — spec D4), check the device error flags (the
    cell-level raise mechanism — errorOut carries the offending cell
    index for the caller's named NLang raise), return the cudaError for
    the CHECK-style wrapper.

    This TU compiles -fmad=false (spec 第8轮N2): the uniform mapping's
    low + span * u must not contract into FMA — the host's separated
    multiply-add rounds differently. Reductions ride block partials +
    the host finalize (the double-accumulator contract); argmax/extreme
    ride the (value, index) comparator with the first-wins tie rule.
---*/
#include "cuda_kernels.h"
#include "cuda_mt19937.cuh"

#include <cuda_runtime.h>

#include <cmath>
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
struct DevView {
    int32_t rank;
    int64_t elemCount;
    int64_t offset;
    void* data;
    int32_t elemKind;
    int32_t dims[16];
    int32_t strides[16];
};

DevView ToDevView(const LaunchView& v) {
    DevView d;
    d.rank = v.rank;
    d.elemCount = v.elemCount;
    d.offset = v.offset;
    d.data = v.data;
    d.elemKind = v.elemKind;
    for (int32_t i = 0; i < v.rank && i < 16; ++i) {
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

//The single stream + the cell-error flags ([0] = kind, [1] = cell).
cudaStream_t g_stream = nullptr;
int32_t* g_error = nullptr;
bool g_ready = false;

cudaError_t Finish() {
    return cudaStreamSynchronize(g_stream);
}

} // namespace

bool CudaReady() { return g_ready; }

bool CudaLazyInit(std::string* error) {
    if (g_ready)
        return true;
    if (cudaSetDevice(0) != cudaSuccess) {
        *error = "cudaSetDevice failed (no CUDA device present?)";
        return false;
    }
    if (cudaStreamCreate(&g_stream) != cudaSuccess) {
        *error = "cudaStreamCreate failed";
        return false;
    }
    if (cudaMalloc((void**)&g_error, 2 * sizeof(int32_t)) != cudaSuccess) {
        *error = "device error-flag allocation failed";
        return false;
    }
    const uint32_t none[2] = {kErrNone, 0};
    cudaMemcpy(g_error, none, 2 * sizeof(int32_t), cudaMemcpyHostToDevice);
    g_ready = true;
    return true;
}

cudaError_t CopyBytes(void* dst, const void* src, size_t bytes,
                      bool toDevice) {
    cudaMemcpyAsync(dst, src, bytes,
        toDevice ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost,
        g_stream);
    return cudaStreamSynchronize(g_stream);
}

//--- the device-record hooks (the pool + the byte transfers) ------------

namespace {

class DevicePool {
public:
    void* Alloc(size_t bytes) {
        for (auto& b : blocks_) {
            if (b.free && b.bytes >= bytes) {
                b.free = false;
                return b.ptr;
            }
        }
        void* ptr = nullptr;
        if (cudaMalloc(&ptr, bytes) != cudaSuccess)
            return nullptr;
        blocks_.push_back(Block{ptr, bytes, false});
        return ptr;
    }

    void Free(void* ptr) {
        for (auto& b : blocks_) {
            if (b.ptr == ptr) {
                b.free = true;
                return;
            }
        }
    }

private:
    struct Block {
        void* ptr;
        size_t bytes;
        bool free;
    };
    std::vector<Block> blocks_;
};

DevicePool g_pool;

} // namespace

void* PoolAlloc(size_t bytes) {
    if (!CudaLazyInit(nullptr))
        return nullptr;
    return g_pool.Alloc(bytes);
}

void PoolZero(void* ptr, size_t bytes) {
    cudaMemsetAsync(ptr, 0, bytes, g_stream);
    cudaStreamSynchronize(g_stream);
}

void PoolFree(void* ptr) {
    g_pool.Free(ptr);
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
    const DevView da = ToDevView(a);
    const DevView db = ToDevView(b);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    const uint32_t none[2] = {kErrNone, 0};
    cudaMemcpy(g_error, none, 2 * sizeof(int32_t), cudaMemcpyHostToDevice);
    if (dst.elemKind == 1) {
        BinaryKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
            op, da, db, dd, 0.0, false, false);
    } else if (dst.elemKind == 17) {
        BinaryKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
            op, da, db, dd, 0.0, false, false);
    } else if (dst.elemKind == 0) {
        BinaryKernelInt<int32_t><<<(int)blocks, kThreads, 0, g_stream>>>(
            op, da, db, dd, 0, false, false, g_error);
    } else {
        BinaryKernelInt<int64_t><<<(int)blocks, kThreads, 0, g_stream>>>(
            op, da, db, dd, 0, false, false, g_error);
    }
    cudaError_t err = cudaStreamSynchronize(g_stream);
    if (err != cudaSuccess)
        return err;
    int32_t flags[2] = {kErrNone, 0};
    cudaMemcpy(flags, g_error, 2 * sizeof(int32_t), cudaMemcpyDeviceToHost);
    if (flags[0] != kErrNone)
        *errorOut = flags[1];
    return cudaSuccess;
}

cudaError_t LaunchBinaryScalar(int op, const LaunchView& a,
                               double scalar, bool scalarLeft,
                               const LaunchView& dst, int32_t* errorOut) {
    const DevView da = ToDevView(a);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    const uint32_t none[2] = {kErrNone, 0};
    cudaMemcpy(g_error, none, 2 * sizeof(int32_t), cudaMemcpyHostToDevice);
    if (dst.elemKind == 1) {
        BinaryKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
            op, da, dd, dd, scalar, scalarLeft, true);
    } else if (dst.elemKind == 17) {
        BinaryKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
            op, da, dd, dd, scalar, scalarLeft, true);
    } else if (dst.elemKind == 0) {
        BinaryKernelInt<int32_t><<<(int)blocks, kThreads, 0, g_stream>>>(
            op, da, dd, dd, (int64_t)scalar, scalarLeft, true, g_error);
    } else {
        BinaryKernelInt<int64_t><<<(int)blocks, kThreads, 0, g_stream>>>(
            op, da, dd, dd, (int64_t)scalar, scalarLeft, true, g_error);
    }
    cudaError_t err = cudaStreamSynchronize(g_stream);
    if (err != cudaSuccess)
        return err;
    int32_t flags[2] = {kErrNone, 0};
    cudaMemcpy(flags, g_error, 2 * sizeof(int32_t), cudaMemcpyDeviceToHost);
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

__global__ void UnaryKernelI(int op, DevView src, DevView dst) {
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
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (dst.elemKind == 1)
        UnaryKernelF<<<(int)blocks, kThreads, 0, g_stream>>>(op, ds, dd);
    else if (dst.elemKind == 17)
        UnaryKernel<<<(int)blocks, kThreads, 0, g_stream>>>(op, ds, dd);
    else
        UnaryKernelI<<<(int)blocks, kThreads, 0, g_stream>>>(op, ds, dd);
    return cudaStreamSynchronize(g_stream);
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

cudaError_t LaunchClamp(bool isMax, const LaunchView& src, double bound,
                        const LaunchView& dst) {
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (dst.elemKind == 1)
        ClampKernelF<<<(int)blocks, kThreads, 0, g_stream>>>(
            isMax, ds, (float)bound, dd);
    else
        ClampKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
            isMax, ds, bound, dd);
    return cudaStreamSynchronize(g_stream);
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

cudaError_t LaunchSumAll(const LaunchView& src, double* out,
                         int64_t* outI, bool isInt) {
    //The contiguous flat sum (owned device records are dense).
    const int64_t count = src.elemCount;
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    if (isInt) {
        //int arrays accumulate in int64 (the CPU walk's Acc carrier —
        //the wrap happens at the final narrow, matching the CPU's
        //static_cast<T>(acc) exactly). The int32/int64 cells ride their
        //own partial kernel (reading them as doubles would corrupt).
        int64_t* dPartial = nullptr;
        if (cudaMalloc((void**)&dPartial,
            blocks * sizeof(int64_t)) != cudaSuccess)
            return cudaErrorMemoryAllocation;
        if (src.elemKind == 0) {
            SumPartialKernelI32<<<(int)blocks, kThreads, 0, g_stream>>>(
                (const int32_t*)src.data, count, dPartial);
        } else {
            SumPartialKernelI64<<<(int)blocks, kThreads, 0, g_stream>>>(
                (const int64_t*)src.data, count, dPartial);
        }
        std::vector<int64_t> partials(blocks);
        cudaMemcpy(partials.data(), dPartial, blocks * sizeof(int64_t),
            cudaMemcpyDeviceToHost);
        cudaFree(dPartial);
        int64_t acc = 0;
        for (int64_t p : partials)
            acc += p;
        *outI = acc;
        return cudaStreamSynchronize(g_stream);
    }
    double* dPartial = nullptr;
    if (cudaMalloc((void**)&dPartial,
        blocks * sizeof(double)) != cudaSuccess)
        return cudaErrorMemoryAllocation;
    //The float kinds branch by cell width: an f32 array read as
    //doubles would consume two cells per element (garbage + OOB).
    if (src.elemKind == 1)
        SumPartialKernelF32<<<(int)blocks, kThreads, 0, g_stream>>>(
            (const float*)src.data, count, dPartial);
    else
        SumPartialKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
            (const double*)src.data, count, dPartial);
    std::vector<double> partials(blocks);
    cudaMemcpy(partials.data(), dPartial, blocks * sizeof(double),
        cudaMemcpyDeviceToHost);
    cudaFree(dPartial);
    double acc = 0.0;
    for (double p : partials)
        acc += p;
    *out = acc;
    return cudaStreamSynchronize(g_stream);
}

//--- axis reductions -----------------------------------------------------

//The f32 axis sums: FLOAT cells, DOUBLE accumulators (the CPU walk's
//contract — softmax's sum(e, 1) rides these).
__global__ void SumAxis0KernelF32(const float* src, int64_t rows,
                                  int64_t inner, float* dst) {
    const int64_t col = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (col >= inner)
        return;
    double acc = 0.0;
    for (int64_t r = 0; r < rows; ++r)
        acc += (double)src[r * inner + col];
    dst[col] = (float)acc;
}

__global__ void SumAxisLastKernelF32(const float* src, int64_t rows,
                                     int64_t seg, float* dst) {
    const int tid = threadIdx.x;
    __shared__ double local[kThreads];
    local[tid] = 0.0;
    const int64_t row = blockIdx.x;
    const int64_t base = row * (int64_t)seg;
    for (int64_t i = base + tid; i < base + seg;
         i += (int64_t)blockDim.x) {
        local[tid] += (double)src[i];
    }
    __syncthreads();
    if (tid == 0) {
        double acc = 0.0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        dst[row] = (float)acc;
    }
}

//Axis-0 sum: one thread per output column, accumulating down the rows
//(the double accumulator contract).
__global__ void SumAxis0Kernel(const double* src, int64_t rows,
                               int64_t inner, double* dst) {
    const int64_t col = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (col >= inner)
        return;
    double acc = 0.0;
    for (int64_t r = 0; r < rows; ++r)
        acc += src[r * inner + col];
    dst[col] = acc;
}

//Axis-last sum: one thread per output row, summing the contiguous
//segment (the block-partial form — kThreads-wide segments with the
//shared-memory reduce).
__global__ void SumAxisLastKernel(const double* src, int64_t rows,
                                  int64_t seg, double* dst) {
    __shared__ double local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = 0.0;
    const int64_t row = blockIdx.x;
    const int64_t base = row * (int64_t)seg;
    for (int64_t i = base + tid; i < base + seg;
         i += (int64_t)blockDim.x) {
        local[tid] += src[i];
    }
    __syncthreads();
    if (tid == 0) {
        double acc = 0.0;
        for (int t = 0; t < kThreads; ++t)
            acc += local[t];
        dst[row] = acc;
    }
}

//Max/Min along the LAST axis (f32): one thread per output row, the
//first-wins scan over the contiguous segment (softmax's
//max(logits, 1)).
__global__ void ExtremeAxisLastKernelF32(const float* src, int64_t rows,
                                         int64_t seg, bool isMax,
                                         float* dst) {
    const int64_t row = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (row >= rows)
        return;
    const int64_t base = row * seg;
    float best = src[base];
    for (int64_t s = 1; s < seg; ++s) {
        const float cur = src[base + s];
        if (isMax ? cur > best : cur < best)
            best = cur;
    }
    dst[row] = best;
}

//Max/Min along axis 0 (f32): one thread per output column, the
//first-wins scan down the rows.
__global__ void ExtremeAxis0KernelF32(const float* src, int64_t rows,
                                      int64_t inner, bool isMax,
                                      float* dst) {
    const int64_t col = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (col >= inner)
        return;
    float best = src[col];
    for (int64_t r = 1; r < rows; ++r) {
        const float cur = src[r * inner + col];
        if (isMax ? cur > best : cur < best)
            best = cur;
    }
    dst[col] = best;
}

cudaError_t LaunchReduceAxis(int op, const LaunchView& src, int axis,
                             const LaunchView& dst, int32_t* errorOut) {
    //P3 scope (design 2026-11-15 §R6): Sum and Max/Min on the
    //first/last axes of f32 (mnist's whole surface); middle axes and
    //the int/f64 kinds land with the parity suite (plan T3.9).
    //ReduceAxisOp: Sum=0, Max=1, Min=2 (the enum order).
    if (op != 0 && op != 1 && op != 2)
        return cudaErrorNotSupported;
    if (src.elemKind != 1)
        return cudaErrorNotSupported;
    const int32_t rank = src.rank;
    const int64_t along = src.dims[axis];
    const int64_t inner = rank == 2 ? src.dims[1] : 1;
    const int64_t outer = rank == 2 ? src.dims[0] : 1;
    //The gate above pins f32 — FLOAT cells, DOUBLE accumulators (the
    //CPU walk's contract).
    const float* fSrc = (const float*)src.data + src.offset;
    float* fDst = (float*)dst.data + dst.offset;
    const bool isExtreme = op == 1 || op == 2;
    if (isExtreme) {
        const bool isMax = op == 1;
        if (axis == rank - 1) {
            const int64_t rows = dst.elemCount;
            ExtremeAxisLastKernelF32<<<(int)rows, 1, 0, g_stream>>>(
                fSrc, rows, along, isMax, fDst);
        } else if (axis == 0 && rank == 2) {
            const int64_t blocks = (inner + kThreads - 1) / kThreads;
            ExtremeAxis0KernelF32<<<(int)blocks, kThreads, 0, g_stream>>>(
                fSrc, outer, inner, isMax, fDst);
        } else {
            return cudaErrorNotSupported;
        }
        return cudaStreamSynchronize(g_stream);
    }
    if (axis == rank - 1) {
        const int64_t rows = dst.elemCount;
        SumAxisLastKernelF32<<<(int)rows, kThreads, 0, g_stream>>>(
            fSrc, rows, along, fDst);
    } else if (axis == 0 && rank == 2) {
        const int64_t blocks = (inner + kThreads - 1) / kThreads;
        SumAxis0KernelF32<<<(int)blocks, kThreads, 0, g_stream>>>(
            fSrc, outer, inner, fDst);
    } else {
        return cudaErrorNotSupported;
    }
    return cudaStreamSynchronize(g_stream);
}

//--- argmax/argmin (the axis-last winner-index kernel) -------------------

__global__ void ArgExtremeLastKernel(const float* src, int64_t rows,
                                     int64_t seg, bool isMax,
                                     int32_t* dst) {
    const int64_t row = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (row >= rows)
        return;
    const int64_t base = row * seg;
    int32_t best = 0;
    float bestVal = src[base];
    for (int64_t s = 1; s < seg; ++s) {
        const float cur = src[base + s];
        if (isMax ? cur > bestVal : cur < bestVal) {
            bestVal = cur;
            best = (int32_t)s;
        }
    }
    dst[row] = best;
}

cudaError_t LaunchArgExtreme(bool isMax, const LaunchView& src,
                             int axis, const LaunchView& dst) {
    //The axis-last arm (accuracy's argmax(probs, 1)); other axes land
    //with the parity suite.
    if (axis != src.rank - 1 || src.elemKind != 1)
        return cudaErrorNotSupported;
    const int64_t rows = dst.elemCount;
    const int64_t seg = src.dims[axis];
    const int64_t blocks = (rows + kThreads - 1) / kThreads;
    ArgExtremeLastKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
        (const float*)src.data, rows, seg, isMax, (int32_t*)dst.data);
    return cudaStreamSynchronize(g_stream);
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

cudaError_t LaunchDot(const LaunchView& a, const LaunchView& b,
                      double* out, int64_t* outI, bool isInt) {
    const int64_t count = a.elemCount;
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    //The kind branches: f32 accumulates float (the CPU carrier
    //contract), the int kinds int64, f64 double. The old double-only
    //kernel mis-read every other kind's cells.
    if (a.elemKind == 1) {
        float* dPartial = nullptr;
        if (cudaMalloc((void**)&dPartial,
            blocks * sizeof(float)) != cudaSuccess)
            return cudaErrorMemoryAllocation;
        DotPartialKernelF32<<<(int)blocks, kThreads, 0, g_stream>>>(
            (const float*)a.data, (const float*)b.data, count, dPartial);
        std::vector<float> partials(blocks);
        cudaMemcpy(partials.data(), dPartial, blocks * sizeof(float),
            cudaMemcpyDeviceToHost);
        cudaFree(dPartial);
        float acc = 0.0f;
        for (float p : partials)
            acc += p;
        if (outI)
            *outI = (int64_t)acc;
        if (out)
            *out = (double)acc;
        return cudaStreamSynchronize(g_stream);
    }
    if (a.elemKind == 0 || a.elemKind == 15) {
        int64_t* dPartial = nullptr;
        if (cudaMalloc((void**)&dPartial,
            blocks * sizeof(int64_t)) != cudaSuccess)
            return cudaErrorMemoryAllocation;
        if (a.elemKind == 0)
            DotPartialKernelI32<<<(int)blocks, kThreads, 0, g_stream>>>(
                (const int32_t*)a.data, (const int32_t*)b.data, count,
                dPartial);
        else
            DotPartialKernelI64<<<(int)blocks, kThreads, 0, g_stream>>>(
                (const int64_t*)a.data, (const int64_t*)b.data, count,
                dPartial);
        std::vector<int64_t> partials(blocks);
        cudaMemcpy(partials.data(), dPartial, blocks * sizeof(int64_t),
            cudaMemcpyDeviceToHost);
        cudaFree(dPartial);
        int64_t acc = 0;
        for (int64_t p : partials)
            acc += p;
        *outI = acc;
        return cudaStreamSynchronize(g_stream);
    }
    double* dPartial = nullptr;
    if (cudaMalloc((void**)&dPartial,
        blocks * sizeof(double)) != cudaSuccess)
        return cudaErrorMemoryAllocation;
    DotPartialKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
        (const double*)a.data, (const double*)b.data, count, dPartial);
    std::vector<double> partials(blocks);
    cudaMemcpy(partials.data(), dPartial, blocks * sizeof(double),
        cudaMemcpyDeviceToHost);
    cudaFree(dPartial);
    {
        double acc = 0.0;
        for (double p : partials)
            acc += p;
        *out = acc;
    }
    return cudaStreamSynchronize(g_stream);
}

//--- extreme (value reduction with the first-wins rule) ------------------

__global__ void ExtremePartialKernel(const double* src, int64_t count,
                                     bool isMax, double* partials) {
    __shared__ double local[kThreads];
    const int tid = threadIdx.x;
    local[tid] = src[blockIdx.x * (int64_t)kThreads + tid];
    const int64_t base = blockIdx.x * (int64_t)kThreads;
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
    local[tid] = src[blockIdx.x * (int64_t)kThreads + tid];
    const int64_t base = blockIdx.x * (int64_t)kThreads;
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

cudaError_t LaunchExtreme(bool isMax, const LaunchView& src,
                          double* outF, int64_t* outI, bool isInt) {
    const int64_t count = src.elemCount;
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    if (src.elemKind == 1) {
        float* dPartial = nullptr;
        if (cudaMalloc((void**)&dPartial,
            blocks * sizeof(float)) != cudaSuccess)
            return cudaErrorMemoryAllocation;
        ExtremePartialKernelF32<<<(int)blocks, kThreads, 0, g_stream>>>(
            (const float*)src.data, count, isMax, dPartial);
        std::vector<float> partials(blocks);
        cudaMemcpy(partials.data(), dPartial, blocks * sizeof(float),
            cudaMemcpyDeviceToHost);
        cudaFree(dPartial);
        float best = partials[0];
        for (size_t i = 1; i < partials.size(); ++i) {
            if (isMax ? partials[i] > best : partials[i] < best)
                best = partials[i];
        }
        *outF = (double)best;
        return cudaStreamSynchronize(g_stream);
    }
    double* dPartial = nullptr;
    if (cudaMalloc((void**)&dPartial,
        blocks * sizeof(double)) != cudaSuccess)
        return cudaErrorMemoryAllocation;
    ExtremePartialKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
        (const double*)src.data, count, isMax, dPartial);
    std::vector<double> partials(blocks);
    cudaMemcpy(partials.data(), dPartial, blocks * sizeof(double),
        cudaMemcpyDeviceToHost);
    cudaFree(dPartial);
    double best = partials[0];
    for (size_t i = 1; i < partials.size(); ++i) {
        if (isMax ? partials[i] > best : partials[i] < best)
            best = partials[i];
    }
    if (isInt)
        *outI = (int64_t)best;
    else
        *outF = best;
    return cudaStreamSynchronize(g_stream);
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

cudaError_t LaunchMatmul(const LaunchView& a, const LaunchView& b,
                         const LaunchView& dst, int elemKindCode) {
    //Dense rank-2 operands (the dispatcher materialized); f32 kernel —
    //the f64/i32/i64 variants land with the parity suite (T3.5).
    if (elemKindCode != 1)
        return cudaErrorNotSupported;
    const int32_t rows = a.dims[0];
    const int32_t inner = a.dims[1];
    const int32_t cols = b.dims[1];
    dim3 grid((cols + 15) / 16, (rows + 15) / 16);
    dim3 block(16, 16);
    MatmulKernelF<<<grid, block, 0, g_stream>>>(
        (const float*)a.data, (const float*)b.data, (float*)dst.data,
        rows, inner, cols);
    return cudaStreamSynchronize(g_stream);
}

//--- gather / eq / oneHot ------------------------------------------------

__global__ void GatherKernel(int elemKindCode, DevView src,
                             const int64_t* indices, int64_t indexCount,
                             DevView dst, int32_t* errorOut) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= indexCount)
        return;
    const int64_t idx = indices[i];
    if (idx < 0) {
        errorOut[0] = kErrOOB;
        errorOut[1] = i;
        return;
    }
    //The per-index bounds check rides the caller (the count/axis gates)
    //— the kernel copies element idx from the source's flat storage.
    const int64_t sa = src.offset + idx;
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

cudaError_t LaunchGather(const LaunchView& src, int axis,
                         const LaunchView& indices,
                         const LaunchView& dst, int32_t* errorOut) {
    (void)axis;   //rank-1 fancy indexing (the dispatcher flattens)
    //The indices land host-side (the dispatcher staged them); H2D here.
    const int64_t count = indices.elemCount;
    int64_t* dIdx = nullptr;
    if (cudaMalloc((void**)&dIdx,
        count * sizeof(int64_t)) != cudaSuccess)
        return cudaErrorMemoryAllocation;
    //The indices view is int32 — read them on the host and widen.
    std::vector<int64_t> wide(count);
    const int32_t* cells = (const int32_t*)indices.data;
    const int64_t addr0 = indices.offset;
    for (int64_t i = 0; i < count; ++i)
        wide[i] = cells[addr0 + i];
    cudaMemcpyAsync(dIdx, wide.data(), count * sizeof(int64_t),
        cudaMemcpyHostToDevice, g_stream);
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const uint32_t none[2] = {kErrNone, 0};
    cudaMemcpyAsync(g_error, none, 2 * sizeof(int32_t),
        cudaMemcpyHostToDevice, g_stream);
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    GatherKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
        dst.elemKind, ds, dIdx, count, dd, g_error);
    cudaError_t err = cudaStreamSynchronize(g_stream);
    cudaFree(dIdx);
    if (err != cudaSuccess)
        return err;
    int32_t flags[2] = {kErrNone, 0};
    cudaMemcpy(flags, g_error, 2 * sizeof(int32_t), cudaMemcpyDeviceToHost);
    if (flags[0] != kErrNone)
        *errorOut = flags[1];
    return cudaSuccess;
}

__global__ void EqKernel(int elemKindCode, DevView a,
                         DevView b, DevView dst) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= dst.elemCount)
        return;
    const int64_t aa = FlatAddress(a, i);
    const int64_t ab = FlatAddress(b, i);
    const int64_t ad = dst.offset + i;
    int32_t out = 0;
    if (elemKindCode == 1)
        out = ((const float*)a.data)[aa]
            == ((const float*)b.data)[ab] ? 1 : 0;
    else if (elemKindCode == 17)
        out = ((const double*)a.data)[aa]
            == ((const double*)b.data)[ab] ? 1 : 0;
    else if (elemKindCode == 0)
        out = ((const int32_t*)a.data)[aa]
            == ((const int32_t*)b.data)[ab] ? 1 : 0;
    else
        out = ((const int64_t*)a.data)[aa]
            == ((const int64_t*)b.data)[ab] ? 1 : 0;
    ((int32_t*)dst.data)[ad] = out;
}

cudaError_t LaunchEq(const LaunchView& a, const LaunchView& b,
                     const LaunchView& dst, int elemKindCode) {
    const DevView da = ToDevView(a);
    const DevView db = ToDevView(b);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    EqKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
        elemKindCode, da, db, dd);
    return cudaStreamSynchronize(g_stream);
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
    const int64_t count = labels.elemCount;
    int32_t* dLabels = nullptr;
    if (cudaMalloc((void**)&dLabels,
        count * sizeof(int32_t)) != cudaSuccess)
        return cudaErrorMemoryAllocation;
    cudaMemcpyAsync(dLabels, (const int32_t*)labels.data,
        count * sizeof(int32_t), cudaMemcpyHostToDevice, g_stream);
    const uint32_t none[2] = {kErrNone, 0};
    cudaMemcpyAsync(g_error, none, 2 * sizeof(int32_t),
        cudaMemcpyHostToDevice, g_stream);
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    OneHotKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
        dLabels, count, n, (float*)dst.data, g_error);
    cudaError_t err = cudaStreamSynchronize(g_stream);
    cudaFree(dLabels);
    if (err != cudaSuccess)
        return err;
    int32_t flags[2] = {kErrNone, 0};
    cudaMemcpy(flags, g_error, 2 * sizeof(int32_t), cudaMemcpyDeviceToHost);
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
    const int64_t count = dst.elemCount;
    uint32_t* dState = nullptr;
    if (cudaMalloc((void**)&dState, 624 * sizeof(uint32_t))
        != cudaSuccess)
        return cudaErrorMemoryAllocation;
    cudaMemcpyAsync(dState, mt624, 624 * sizeof(uint32_t),
        cudaMemcpyHostToDevice, g_stream);
    const int64_t blocks = (count + kThreads - 1) / kThreads;
    double* dOut = (double*)dst.data;
    if (isDouble) {
        UniformKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
            dOut, count, low, high - low, dState, position);
    } else {
        UniformKernelF<<<(int)blocks, kThreads, 0, g_stream>>>(
            (float*)dst.data, count, (float)low, (float)(high - low),
            dState, position);
    }
    cudaError_t err = cudaStreamSynchronize(g_stream);
    cudaFree(dState);
    return err;
}

//--- generation & movement ----------------------------------------------

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
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    FillKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
        elemKindCode, dd, value, iValue);
    return cudaStreamSynchronize(g_stream);
}

__global__ void ArangeKernelD(double* dst, int64_t count, double start,
                              double step) {
    const int64_t i = blockIdx.x * (int64_t)blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    dst[i] = start + (double)i * step;
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
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    if (isInt) {
        if (dst.elemKind == 0)
            ArangeKernelI32<<<(int)blocks, kThreads, 0, g_stream>>>(
                (int32_t*)dst.data, dst.elemCount, (int64_t)start,
                (int64_t)step);
        else
            ArangeKernelI64<<<(int)blocks, kThreads, 0, g_stream>>>(
                (int64_t*)dst.data, dst.elemCount, (int64_t)start,
                (int64_t)step);
    } else {
        ArangeKernelD<<<(int)blocks, kThreads, 0, g_stream>>>(
            (double*)dst.data, dst.elemCount, start, step);
    }
    return cudaStreamSynchronize(g_stream);
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
    const DevView ds = ToDevView(src);
    const DevView dd = ToDevView(dst);
    const int64_t blocks = (dst.elemCount + kThreads - 1) / kThreads;
    CopyKernel<<<(int)blocks, kThreads, 0, g_stream>>>(
        elemKindCode, ds, dd);
    return cudaStreamSynchronize(g_stream);
}

//concat: the axis-0/last dense-halves copy (the concat geometry's two
//source windows are contiguous spans of the destination when the axis
//is 0 of rank-2 or the last dim of rank-1 — the e2e concat shapes).
//Other axis positions land with the parity suite.
cudaError_t LaunchConcat(const LaunchView& a, const LaunchView& b,
                         int axis, const LaunchView& dst,
                         int elemKindCode) {
    //Geometry gate (design §R6): only the layouts whose dst halves are
    //contiguous spans — rank 1 (axis 0) and rank-2 axis 0. Other axis
    //positions raise NotSupported (the parity suite adds them); a
    //tiled two-half copy there would interleave.
    const bool contiguousHalves =
        a.rank == 1 || (a.rank == 2 && axis == 0);
    if (!contiguousHalves)
        return cudaErrorNotSupported;
    const int64_t aCount = a.elemCount;
    const int64_t bCount = b.elemCount;
    //Cell width by KIND (the old 1/0 hard-coding copied half the
    //bytes for f64/i64).
    const int cellBytes = (elemKindCode == 17 || elemKindCode == 15)
        ? 8 : 4;
    //Both spans are device-dense (the dispatcher materialized the
    //operands) — D2D for both; the first span's host-signed copy read
    //device memory as a host source.
    cudaMemcpyAsync(dst.data, a.data, aCount * cellBytes,
        cudaMemcpyDeviceToDevice, g_stream);
    cudaMemcpyAsync((uint8_t*)dst.data + aCount * cellBytes,
        b.data, bCount * cellBytes, cudaMemcpyDeviceToDevice,
        g_stream);
    return cudaStreamSynchronize(g_stream);
}

} // namespace tensor_cuda

