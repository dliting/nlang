/*---
    tensor_random.cpp — whole-array random fill.

    uniform(dims, low, high): creates the dims-shaped array and fills it
    row-major with low + (high - low) * u, u in [0, 1) drawn from the
    host PRNG (the same seeded stream math.random draws; the VM owns
    the state). The draw is inherently sequential — the win here is
    killing the per-element fill loops, not lane math; a device
    backend slots a counter-based generator at this entry.

    Multi-stream PRNG windows (v0.14.9): a uniform enqueued inside a
    Push/Pop scope (a chunk stream) takes a DISJOINT window of the global
    sequence — a snapshot of the engine advanced by a per-chunk offset
    (the words assigned to earlier chunks), drawn without advancing the
    live engine. A uniform on the main stream rides the live engine.
    The offset is fixed at enqueue time (host-thread order), so the
    windows are deterministic and independent of the streams' execution
    order; the live engine stays coherent for the main stream because
    chunk draws never touch it.
---*/
#include "TensorKernels.h"

#include "backend_registry.h"
#include <vector>



using nlang::native::ArgArray;
using nlang::native::ArgDouble;
using nlang::native::NewArrayShaped;
using nlang::native::Raise;
using nlang::native::ReturnInt;

namespace tensor {

namespace {

//The host hands out raw 32-bit draws; the [0, 1) mapping takes the top
//24 bits (the same conversion math.random uses, so a seeded run draws
//the identical sequence through either surface).
constexpr unsigned kRngUnitShift = 8u;
constexpr double kRngUnitScale = 1.0 / 16777216.0;

double ToUnit(uint32_t word) {
    return static_cast<double>(word >> kRngUnitShift) * kRngUnitScale;
}

double NextUnit(NativeHost* host) {
    return ToUnit(host->nextRandom(host));
}

//The chunk-word cursor: the total number of words assigned to chunk
//uniforms so far. Each chunk uniform takes its window at this offset
//and advances it by its element count. Main-stream uniforms never touch
//it (they ride the live engine), so the chunk windows are disjoint by
//construction and the main stream stays coherent.
static int64_t g_chunkWordCursor = 0;

//Fill `count` cells with low + span * u. chunk=true draws a disjoint
//snapshot window (the multi-stream PRNG contract); chunk=false rides
//the live engine one draw per cell.
template <typename T>
void FillUniform(NativeHost* host, T* cells, int64_t count, double span,
                 double low, bool chunk) {
    if (chunk) {
        const int64_t offset = g_chunkWordCursor;
        std::vector<uint32_t> words(count);
        host->snapshotWindowRng(host, words.data(), count, offset);
        g_chunkWordCursor += count;
        for (int64_t i = 0; i < count; ++i)
            cells[i] = static_cast<T>(low + span * ToUnit(words[i]));
    } else {
        for (int64_t i = 0; i < count; ++i)
            cells[i] = static_cast<T>(low + span * NextUnit(host));
    }
}

//uniform(dims, low, high): the dims vector shapes the creation (the
//full(dims, ...) creation path — shared rank-1 collector, negative-dim
//and element-count checks inside the creation), then one draw per element
//in row-major order. Computed in double, stored at the element width.
//low/high are read by the entry (the float cell packs into the low
//half, so the F arm reads a float, not a double) and widened here.
template <typename T>
void UniformKernel(NativeHost* host, uint8_t* ret, const uint8_t* args,
                   uint8_t kind, T lowArg, T highArg) {
    NativeArrayView dims;
    ArgArray(host, args, 0, &dims);
    std::vector<uint8_t> dimsStage;
    dims = StageOperandToHost(host, dims, &dimsStage);
    int32_t shape[nlang::kMaxArrayRank];
    const int32_t rank =
        CollectIntCells(host, dims, "tensor.uniform", shape);
    if (rank < 1)
        Raise(host, NEXC_Base, "tensor.uniform: dims length out of range");
    for (int32_t d = 0; d < rank; ++d) {
        if (shape[d] < 0)
            Raise(host, NEXC_OutOfBounds,
                  "tensor.uniform: dims dim is negative");
    }
    const double low = static_cast<double>(lowArg);
    const double high = static_cast<double>(highArg);
    if (low > high)
        Raise(host, NEXC_Base,
              "tensor.uniform: low is greater than high");
    const int32_t handle = MintArray(host, kind, rank, shape);
    const double span = high - low;
    const bool chunk = StreamPushDepth() > 0;
    if (ActiveDeviceBackend().device) {
        //The device mint lands through a ForceHost staging twin: the
        //host PRNG fills the twin, one block H2D lands it (the P2
        //spelling — T3.7 replaces the fill with the MT19937 snapshot
        //kernel, same landing).
        const int32_t staging = NewArrayShaped(host, kind, rank, shape);
        NativeArrayView sv;
        host->getArrayView(host, staging, &sv);
        T* cells = CellsOf<T>(sv);
        FillUniform(host, cells, sv.elemCount, span, low, chunk);
        NativeArrayView dv;
        host->getArrayView(host, handle, &dv);
        CopyHostToDevice(host, dv.data, CellsOf<const T>(sv),
            static_cast<size_t>(sv.elemCount) * sv.elemCellBytes);
        ReturnInt(ret, handle);
        return;
    }
    NativeArrayView out;
    host->getArrayView(host, handle, &out);
    T* cells = CellsOf<T>(out);
    FillUniform(host, cells, out.elemCount, span, low, chunk);
    ReturnInt(ret, handle);
}

} //namespace

//tensor.seed's companion: reseed resets the window cursor with the
//engine, so a seeded program reproduces the same chunk windows (and
//therefore the same draws) on every run. External linkage — the seed
//entry in tensor_native.cpp calls it (declared in backend_registry.h).
void ResetChunkPrngCursor() {
    g_chunkWordCursor = 0;
}

void TensorUniformF(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    int argc) {
    (void)argc;
    UniformKernel<float>(host, ret, args, NEK_Float32,
                         nlang::native::ArgFloat(args, 1),
                         nlang::native::ArgFloat(args, 2));
}

void TensorUniformD(NativeHost* host, uint8_t* ret, const uint8_t* args,
                    int argc) {
    (void)argc;
    UniformKernel<double>(host, ret, args, NEK_Float64,
                          nlang::native::ArgDouble(args, 1),
                          nlang::native::ArgDouble(args, 2));
}

} //namespace tensor
