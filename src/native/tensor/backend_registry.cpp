/*---
    backend_registry.cpp — the device-backend selection, the compute
    routing and the residency-aware mints (design
    2026-11-15-p3-routing-design). See the header for the architectural
    contract; the CLASS 2/3 transfer and staging bodies live in
    backend_transfer.cpp (the registry file caps at 500 lines).
---*/
#include "backend_registry.h"
#include "../tensor_cuda/backend_descriptor.h"
#include "plugin_loader.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace tensor {

using nlang::native::ArgInt;
using nlang::native::Raise;

namespace {

DeviceBackendState g_backend;

//The mirror test backend's hooks: malloc-backed "device" memory. Every
//device path (creation, element transfer, GC release, staging) then
//exercises for real on host memory — P2's acceptance vehicle (plan
//C3 fix). The mirror owns exactly one host-backed device, so any
//selector other than 0 is a plumbing bug: it names the fault instead
//of quietly mis-routing a payload that does not exist.
TensorDeviceStatus MirrorAlloc(int32_t device, void** out, size_t bytes) {
    if (device != 0)
        return {DEVICE_FAULT, 0, "mirror: payload on unknown device"};
    void* ptr = std::malloc(bytes);
    if (ptr == nullptr)
        return {DEVICE_OUT_OF_MEMORY, 0,
                "mirror: host allocation failed"};
    *out = ptr;
    return {DEVICE_OK, 0, nullptr};
}
TensorDeviceStatus MirrorZero(int32_t device, void* ptr, size_t bytes) {
    if (device != 0)
        return {DEVICE_FAULT, 0, "mirror: payload on unknown device"};
    std::memset(ptr, 0, bytes);
    return {DEVICE_OK, 0, nullptr};
}
TensorDeviceStatus MirrorFree(int32_t device, void* ptr) {
    if (device != 0)
        return {DEVICE_FAULT, 0, "mirror: payload on unknown device"};
    std::free(ptr);
    return {DEVICE_OK, 0, nullptr};
}
TensorDeviceStatus MirrorCopyH2D(int32_t device, void* dst,
                                 const void* src, size_t bytes) {
    if (device != 0)
        return {DEVICE_FAULT, 0, "mirror: payload on unknown device"};
    std::memcpy(dst, src, bytes);
    return {DEVICE_OK, 0, nullptr};
}
TensorDeviceStatus MirrorCopyD2H(int32_t device, void* dst,
                                 const void* src, size_t bytes) {
    if (device != 0)
        return {DEVICE_FAULT, 0, "mirror: payload on unknown device"};
    std::memcpy(dst, src, bytes);
    return {DEVICE_OK, 0, nullptr};
}
TensorDeviceStatus MirrorSync() { return {DEVICE_OK, 0, nullptr}; }

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

//The CUDA plugin probe: the plugin sits next to this module (the tensor
//native module). pluginError means the plugin is absent (auto stays
//silent there); probeError means the plugin is present but the probe
//failed (auto says so on stderr). The errors ride std::string because
//the device-ABI rejection names the two versions.
struct CudaProbe {
    const DeviceBackendDescriptor* desc = nullptr;
    std::string pluginError;  //empty = none
    std::string probeError;   //empty = none
};

CudaProbe ProbeCudaPlugin() {
    const std::string pluginPath = OwnModuleDirectory() + kCudaPluginName;
    CudaProbe probe;
    void* plugin = OpenSharedPlugin(pluginPath);
    if (plugin == nullptr) {
        probe.pluginError = "the plugin is not in the native directory";
        return probe;
    }
    //Device ABI gate (device ABI v2, the multi-GPU line): resolve and
    //compare BEFORE init — an incompatible descriptor must never be
    //dereferenced, its layout is exactly what moved. The decision is
    //the header's pure DeviceAbiMatches (a unit test covers the
    //reject path without a stub plugin).
    auto* abiFn = (unsigned (*)(void))ResolvePluginSymbol(
        plugin, "nlang_device_abi_version");
    if (abiFn == nullptr) {
        CloseSharedPlugin(plugin);
        probe.probeError = "the plugin does not export "
            "nlang_device_abi_version";
        return probe;
    }
    const unsigned reported = abiFn();
    if (!DeviceAbiMatches(reported)) {
        CloseSharedPlugin(plugin);
        probe.probeError = "the plugin speaks device ABI v"
            + std::to_string(reported) + ", this registry speaks v"
            + std::to_string(NLANG_DEVICE_ABI_VERSION);
        return probe;
    }
    auto* initFn = (const void* (*)(void))ResolvePluginSymbol(
        plugin, "nlang_device_backend_init");
    if (initFn == nullptr) {
        CloseSharedPlugin(plugin);
        probe.probeError = "the plugin does not export "
            "nlang_device_backend_init";
        return probe;
    }
    probe.desc = (const DeviceBackendDescriptor*)initFn();
    if (probe.desc->deviceCount < 1)
        probe.probeError = "no CUDA device present";
    return probe;
}

} // namespace

const DeviceBackendState& ActiveDeviceBackend() {
    return g_backend;
}

//The cuda/auto arm (spec D3a): explicit cuda fails fast (exit 3) when
//the probe fails — a silent cpu fallback would be lying; auto says so
//on stderr when the plugin is present but broken, and stays silent
//when the plugin is absent (the normal deployment).
void ActivateCudaOrFallBack(NativeHost* host, bool explicitCuda) {
    const CudaProbe probe = ProbeCudaPlugin();
    const bool usable = probe.desc != nullptr
        && probe.desc->deviceCount >= 1;
    if (!usable) {
        const std::string& reason = !probe.probeError.empty()
            ? probe.probeError : probe.pluginError;
        if (explicitCuda) {
            std::string msg = "tensor: backend 'cuda' failed: ";
            msg += reason;
            host->writeError(host, (msg + "\n").c_str());
            std::exit(3);
        }
        if (!probe.probeError.empty())
            host->writeError(host,
                (std::string("tensor: cuda backend unavailable (")
                + probe.probeError + ") — running on cpu\n").c_str());
        g_backend = DeviceBackendState{};
        return;
    }
    g_backend.hooks = *(const TensorDeviceHooks*)probe.desc->hooks;
    g_backend.device = true;
    g_backend.name = "cuda";
    g_backend.deviceCount = probe.desc->deviceCount;
    g_backend.ops = probe.desc->ops;
    g_backend.streamCreate = probe.desc->streamCreate;
    g_backend.streamDestroy = probe.desc->streamDestroy;
    g_backend.streamPush = probe.desc->streamPush;
    g_backend.streamPop = probe.desc->streamPop;
    g_backend.streamJoin = probe.desc->streamJoin;
    host->setTensorDeviceHooks(host, &g_backend.hooks);
}

void InitBackendSelection(NativeHost* host) {
    //Unset selects auto (spec D3a): a shipped build uses an installed
    //device backend without any opt-in and falls back silently on a
    //machine without one — one build, every machine.
    const char* requested = std::getenv("NLANG_TENSOR_BACKEND");
    const std::string name = requested ? requested : "auto";
    if (name == "cpu") {
        g_backend = DeviceBackendState{};
        return;
    }
    if (name == "mirror") {
        g_backend.hooks = MakeMirrorHooks();
        g_backend.device = true;
        g_backend.name = "mirror";
        g_backend.deviceCount = 1;  //one host-backed "device"
        host->setTensorDeviceHooks(host, &g_backend.hooks);
        return;
    }
    if (name == "cuda" || name == "auto") {
        ActivateCudaOrFallBack(host, name == "cuda");
        return;
    }
    //Unknown name: run on cpu and say so. A typo in NLANG_TENSOR_BACKEND
    //must not silently lose the acceleration the user asked for.
    host->writeError(host, (std::string("tensor: unknown backend '")
        + name + "' — running on cpu\n").c_str());
    g_backend = DeviceBackendState{};
}

//--- CLASS 1: compute routing -------------------------------------------

const TensorOps& OpsForResidency(uint8_t residency) {
    if (residency == 1 && g_backend.device && g_backend.ops != nullptr)
        return *(const TensorOps*)g_backend.ops;
    //Host operands run the cpu table DIRECTLY — never the env-selected
    //table (a staged host walk routed through the cuda table was the
    //gather illegal-address; the mirror backend masked it).
    return CpuTensorOps();
}

//--- materialization by residency ---------------------------------------

int32_t MaterializeByResidency(NativeHost* host, int32_t handle,
                               NativeArrayView* view) {
    if (IsContiguous(*view))
        return handle;
    if (view->reserved[0] == 1 && g_backend.device) {
        //A strided DEVICE source materializes on-device: a device-dense
        //mint + the device table's copy (D2D). StrideCopy would read
        //the descriptor cells, and a host landing would break the
        //sibling operands' residency. The mint lands on the SOURCE's
        //device — same-table D2D, never a cross-device copy.
        const int32_t dense = host->newArrayResident(host,
            view->elemKind, view->rank, view->dims, view->reserved[1]);
        NativeArrayView dst;
        host->getArrayView(host, dense, &dst);
        const TensorOps& ops = OpsForResidency(1);
        switch (view->elemKind) {
            case NEK_Int32:  ops.copyI32(host, *view, dst); break;
            case NEK_Int64:  ops.copyI64(host, *view, dst); break;
            case NEK_Float32: ops.copyF32(host, *view, dst); break;
            case NEK_Float64: ops.copyF64(host, *view, dst); break;
            default:
                Raise(host, NEXC_Base,
                      "tensor: unsupported element kind");
        }
        view->elemKind = 0xEE;
        host->getArrayView(host, dense, view);
        return dense;
    }
    //A strided HOST source: the existing host materialization (host
    //memory walks — safe under every backend).
    const int32_t dense = Materialize(host, *view);
    view->elemKind = 0xEE;
    host->getArrayView(host, dense, view);
    return dense;
}

//--- creation mints ------------------------------------------------------

int32_t MintArray(NativeHost* host, uint8_t elemKind, int32_t rank,
                  const int32_t* dims) {
    //The creation family follows the CURRENT stream's device (handle 0
    //— the main stream — decodes to device 0; the PRNG-chunk streams
    //carry their own).
    if (g_backend.device)
        return host->newArrayResident(host, elemKind, rank, dims,
            CurrentScopeDevice());
    return host->newArrayShaped(host, elemKind, rank, dims);
}

int32_t MintArray1(NativeHost* host, uint8_t elemKind, int32_t length) {
    return MintArray(host, elemKind, 1, &length);
}

int32_t MintOutputLike(NativeHost* host, uint8_t elemKind, int32_t rank,
                       const int32_t* dims, const NativeArrayView& like) {
    //The output rides the `like` operand's device (same-device ops —
    //a cross-device split would have raised in AssertSameDevice).
    if (like.reserved[0] == 1 && g_backend.device)
        return host->newArrayResident(host, elemKind, rank, dims,
            like.reserved[1]);
    return host->newArrayShaped(host, elemKind, rank, dims);
}

void AssertSameDevice(NativeHost* host, const NativeArrayView* operands,
                      size_t count) {
    if (count < 2)
        return;
    const uint8_t first = operands[0].reserved[0];
    for (size_t i = 1; i < count; ++i) {
        //The agreement is the (residency, device) pair: two device
        //operands on different devices disagree just as loudly as a
        //device/host pair does.
        if (operands[i].reserved[0] != first
            || (first == 1 && operands[i].reserved[1]
                != operands[0].reserved[1])) {
            //The diagnostic shape is spec D6's: name the sides and the
            //fix, in one line. A device side names its id (the first
            //operand's).
            std::string msg =
                "tensor: operands on different devices (";
            msg += first == 1 ? "cuda:"
                + std::to_string(operands[0].reserved[1]) : "host";
            msg += " vs ";
            msg += operands[i].reserved[0] == 1
                ? "cuda:" + std::to_string(operands[i].reserved[1])
                : "host";
            msg += "); move them with tensor.toDevice/toHost";
            Raise(host, NEXC_Base, msg);
        }
    }
}

void PrepareStagedWalkOut(NativeHost* host, uint8_t elemKind, int32_t rank,
                          const int32_t* dims, const NativeArrayView& srcLike,
                          StagedWalkOut* landing) {
    //Filled in place: a returned copy would leave fill.strides dangling
    //into the destroyed temporary (NativeHost.h's view-copy warning).
    if (srcLike.reserved[0] == 1 && g_backend.device) {
        //Device-resident source: the walk fills a HOST twin mint and
        //Land copies it into the device mint (the twin order keeps the
        //walk's target a plain host buffer).
        const int32_t twinHandle = host->newArrayShaped(host, elemKind,
            rank, dims);
        host->getArrayView(host, twinHandle, &landing->fill);
        landing->handle = host->newArrayResident(host, elemKind, rank,
            dims, srcLike.reserved[1]);
        host->getArrayView(host, landing->handle, &landing->device);
        landing->deviceIs = true;
        return;
    }
    landing->handle = host->newArrayShaped(host, elemKind, rank, dims);
    host->getArrayView(host, landing->handle, &landing->fill);
}

void LandStagedWalkOut(NativeHost* host, StagedWalkOut* landing) {
    if (!landing->deviceIs)
        return;
    CopyHostToDevice(host, landing->device.reserved[1],
        landing->device.data, landing->fill.data,
        static_cast<size_t>(landing->fill.elemCount)
            * landing->fill.elemCellBytes);
}

void RejectAxisOutOfRange(NativeHost* host, const std::string& who,
                          const NativeArrayView& a, int32_t axis) {
    if (axis < 0 || axis >= a.rank)
        Raise(host, NEXC_Base, who + ": axis "
            + std::to_string(axis) + " out of range for rank "
            + std::to_string(a.rank));
}

void RejectZeroSizeSource(NativeHost* host, const std::string& who,
                          const NativeArrayView& a) {
    if (a.elemCount == 0)
        Raise(host, NEXC_Base, who + ": empty array");
}

} // namespace tensor
