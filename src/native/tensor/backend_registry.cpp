/*---
    backend_registry.cpp — the device-backend selection implementation
    (P2 T2.9-T2.11). See the header for the contract.
---*/
#include "backend_registry.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace tensor {

using nlang::native::Raise;

namespace {

DeviceBackendState g_backend;

//The mirror test backend's hooks: malloc-backed "device" memory. Every
//device path (creation, element transfer, GC release, staging) then
//exercises for real on host memory — P2's acceptance vehicle (plan
//C3 fix).
void* MirrorAlloc(size_t bytes) { return std::malloc(bytes); }
void MirrorZero(void* ptr, size_t bytes) { std::memset(ptr, 0, bytes); }
void MirrorFree(void* ptr) { std::free(ptr); }
void MirrorCopyH2D(void* dst, const void* src, size_t bytes) {
    std::memcpy(dst, src, bytes);
}
void MirrorCopyD2H(void* dst, const void* src, size_t bytes) {
    std::memcpy(dst, src, bytes);
}
void MirrorSync() {}

TensorDeviceHooks MakeMirrorHooks() {
    TensorDeviceHooks h;
    h.alloc = &MirrorAlloc;
    h.zero = &MirrorZero;
    h.free = &MirrorFree;
    h.copyH2D = &MirrorCopyH2D;
    h.copyD2H = &MirrorCopyD2H;
    h.sync = &MirrorSync;
    return h;
}

} // namespace

const DeviceBackendState& ActiveDeviceBackend() {
    return g_backend;
}

void InitBackendSelection(NativeHost* host) {
    const char* requested = std::getenv("NLANG_TENSOR_BACKEND");
    const std::string name = requested ? requested : "cpu";
    if (name == "cpu") {
        g_backend = DeviceBackendState{};
        return;
    }
    if (name == "mirror") {
        g_backend.hooks = MakeMirrorHooks();
        g_backend.device = true;
        g_backend.name = "mirror";
        host->setTensorDeviceHooks(host, &g_backend.hooks);
        return;
    }
    if (name == "cuda") {
        //T3.8 loads the plugin; until it lands the explicit selection
        //fails fast (spec D3a — silent cpu fallback would be lying).
        g_backend = DeviceBackendState{};
        host->writeError(host,
            "tensor: backend 'cuda' requested but the device plugin is "
            "not available in this build; falling back is disabled for "
            "an explicit request — run with NLANG_TENSOR_BACKEND=cpu\n");
        std::exit(3);
    }
    if (name == "auto") {
        //T3.8 probes the plugin list in priority order; without one,
        //cpu (spec D3a: absence is normal deployment, silent).
        g_backend = DeviceBackendState{};
        return;
    }
    //Unknown name: the historical selector contract resolves to cpu.
    g_backend = DeviceBackendState{};
}

int32_t MintArray(NativeHost* host, uint8_t elemKind, int32_t rank,
                  const int32_t* dims) {
    if (g_backend.device)
        return host->newArrayResident(host, elemKind, rank, dims);
    return host->newArrayShaped(host, elemKind, rank, dims);
}

int32_t MintArray1(NativeHost* host, uint8_t elemKind, int32_t length) {
    return MintArray(host, elemKind, 1, &length);
}

int32_t MintOutputLike(NativeHost* host, uint8_t elemKind, int32_t rank,
                       const int32_t* dims, const NativeArrayView& like) {
    if (like.reserved[0] == 1 && g_backend.device)
        return host->newArrayResident(host, elemKind, rank, dims);
    return host->newArrayShaped(host, elemKind, rank, dims);
}

NativeArrayView StageOperandToHost(NativeHost* host, NativeArrayView& v,
                                   std::vector<uint8_t>* buf) {
    if (!g_backend.device || v.reserved[0] != 1)
        return v;
    const size_t bytes = static_cast<size_t>(v.elemCount)
        * v.elemCellBytes;
    //The staged view's strides live in the buffer AFTER the payload:
    //a returned view copy's strides pointer would dangle into this
    //function's stack (NativeHost.h's copy warning) — the buffer is
    //caller-owned and outlives the call's reads.
    const size_t strideBytes = static_cast<size_t>(v.rank) * 4;
    buf->resize(bytes + strideBytes);
    auto* stridesOut = reinterpret_cast<int32_t*>(buf->data() + bytes);
    int32_t dense = 1;
    for (int32_t d = v.rank - 1; d >= 0; --d) {
        stridesOut[d] = dense;
        dense *= v.dims[d];
    }
    NativeArrayView staged = v;
    staged.data = buf->data();
    staged.offset = 0;   //the staging is a dense host copy
    staged.reserved[0] = 0;
    staged.strides = stridesOut;
    const TensorOps& ops = ActiveTensorOps();
    switch (v.elemKind) {
        case NEK_Int32:  ops.copyI32(host, v, staged); break;
        case NEK_Int64:  ops.copyI64(host, v, staged); break;
        case NEK_Float32: ops.copyF32(host, v, staged); break;
        case NEK_Float64: ops.copyF64(host, v, staged); break;
        default:
            Raise(host, NEXC_Base,
                  "tensor: unsupported staging element kind");
    }
    return staged;
}

void AssertSameResidency(NativeHost* host, const NativeArrayView* operands,
                         size_t count) {
    if (count < 2)
        return;
    const uint8_t first = operands[0].reserved[0];
    for (size_t i = 1; i < count; ++i) {
        if (operands[i].reserved[0] != first) {
            //The diagnostic shape is spec D6's: name the sides and the
            //fix, in one line.
            std::string msg =
                "tensor: operands on different devices (";
            msg += first == 1 ? "cuda" : "host";
            msg += " vs ";
            msg += operands[i].reserved[0] == 1 ? "cuda" : "host";
            msg += "); move them with tensor.toDevice/toHost [dbg: mixed pair "
                + std::to_string(i) + "/" + std::to_string(count) + "]";
            Raise(host, NEXC_Base, msg);
        }
    }
}

} // namespace tensor
