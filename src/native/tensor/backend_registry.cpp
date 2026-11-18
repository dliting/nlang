/*---
    backend_registry.cpp — the device-backend selection and the THREE
    payload-transfer choke points (design 2026-11-15-p3-routing-design).
    See the header for the architectural contract.
---*/
#include "backend_registry.h"
#include "../tensor_cuda/backend_descriptor.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

namespace tensor {

using nlang::native::ArgInt;
using nlang::native::Raise;

namespace {

DeviceBackendState g_backend;

//The mirror test backend's hooks: malloc-backed "device" memory. Every
//device path (creation, element transfer, GC release, staging) then
//exercises for real on host memory — P2's acceptance vehicle (plan
//C3 fix).
TensorDeviceStatus MirrorAlloc(void** out, size_t bytes) {
    void* ptr = std::malloc(bytes);
    if (ptr == nullptr)
        return {DEVICE_OUT_OF_MEMORY, 0,
                "mirror: host allocation failed"};
    *out = ptr;
    return {DEVICE_OK, 0, nullptr};
}
TensorDeviceStatus MirrorZero(void* ptr, size_t bytes) {
    std::memset(ptr, 0, bytes);
    return {DEVICE_OK, 0, nullptr};
}
TensorDeviceStatus MirrorFree(void* ptr) {
    std::free(ptr);
    return {DEVICE_OK, 0, nullptr};
}
TensorDeviceStatus MirrorCopyH2D(void* dst, const void* src,
                                 size_t bytes) {
    std::memcpy(dst, src, bytes);
    return {DEVICE_OK, 0, nullptr};
}
TensorDeviceStatus MirrorCopyD2H(void* dst, const void* src,
                                 size_t bytes) {
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

//The registry-side copy wrappers convert a non-ok status into the
//named raise (they run on the blocking channel inside tensor natives
//with the host in hand).
void RaiseStatus(NativeHost* host, const TensorDeviceStatus& st) {
    if (st.category == DEVICE_OK)
        return;
    if (st.category == DEVICE_BAD_INDEX)
        Raise(host, NEXC_OutOfBounds,
            "device index " + std::to_string(st.cell)
            + " is out of bounds");
    Raise(host, NEXC_Base,
        st.detail ? std::string("tensor: ") + st.detail
                  : std::string("tensor: device operation failed"));
}

//The per-element affine source address of flat ordinal `flat` (element
//units) — the shared fold of the staging walks.
int64_t AffineElementAt(const NativeArrayView& v, int64_t flat) {
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

//The CUDA plugin probe: nlang_tensor_cuda.dll sits next to
//nlang_tensor.dll (GetModuleHandleEx from this DLL's address resolves
//the native dir without search-path games). pluginError means the
//plugin is absent (auto stays silent there); probeError means the
//plugin is present but the probe failed (auto says so on stderr).
struct CudaProbe {
    const DeviceBackendDescriptor* desc = nullptr;
    const char* pluginError = nullptr;
    const char* probeError = nullptr;
};

CudaProbe ProbeCudaPlugin() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&ProbeCudaPlugin, &self);
    char selfPath[MAX_PATH] = {};
    GetModuleFileNameA(self, selfPath, MAX_PATH);
    std::string dir = selfPath;
    const size_t slash = dir.find_last_of("\\/");
    dir = slash == std::string::npos ? "" : dir.substr(0, slash + 1);
    const std::string pluginPath = dir + "nlang_tensor_cuda.dll";
    CudaProbe probe;
    HMODULE plugin = LoadLibraryA(pluginPath.c_str());
    if (plugin == nullptr) {
        probe.pluginError = "the plugin DLL is not in the native directory";
        return probe;
    }
    auto* initFn = (const void* (*)(void))GetProcAddress(
        plugin, "nlang_device_backend_init");
    if (initFn == nullptr) {
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
        const char* reason = probe.probeError ? probe.probeError
            : probe.pluginError;
        if (explicitCuda) {
            std::string msg = "tensor: backend 'cuda' failed: ";
            msg += reason;
            host->writeError(host, (msg + "\n").c_str());
            std::exit(3);
        }
        if (probe.probeError != nullptr)
            host->writeError(host,
                (std::string("tensor: cuda backend unavailable (")
                + probe.probeError + ") — running on cpu\n").c_str());
        g_backend = DeviceBackendState{};
        return;
    }
    g_backend.hooks = *(const TensorDeviceHooks*)probe.desc->hooks;
    g_backend.device = true;
    g_backend.name = "cuda";
    g_backend.ops = probe.desc->ops;
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

//--- CLASS 2: transfers --------------------------------------------------

void CopyHostToDevice(NativeHost* host, void* deviceDst,
                      const void* hostSrc, size_t bytes) {
    if (!g_backend.device)
        Raise(host, NEXC_Base,
              "tensor: no device backend active (backend=cpu)");
    TensorDeviceStatus st = g_backend.hooks.copyH2D(deviceDst, hostSrc,
        bytes);
    RaiseStatus(host, st);
}

void CopyDeviceToHost(NativeHost* host, void* hostDst,
                      const void* deviceSrc, size_t bytes) {
    if (!g_backend.device)
        Raise(host, NEXC_Base,
              "tensor: no device backend active (backend=cpu)");
    TensorDeviceStatus st = g_backend.hooks.copyD2H(hostDst, deviceSrc,
        bytes);
    RaiseStatus(host, st);
}

int32_t TransferToDevice(NativeHost* host, uint8_t* ret,
                         const uint8_t* args) {
    NativeArrayView v;
    host->getArrayView(host, ArgInt(args, 0), &v);
    //A same-device operand returns as-is (the array API standard's
    //to_device contract): under backend=cpu EVERY array is already on
    //the active device, so toDevice is a no-op there.
    if (!g_backend.device || v.reserved[0] == 1) {
        nlang::native::ReturnInt(ret, ArgInt(args, 0));
        return ArgInt(args, 0);
    }
    //The source is HOST. A strided view materializes host-dense via
    //the cpu table (host memory); a contiguous one transfers straight
    //from its payload.
    if (!IsContiguous(v)) {
        const int32_t dense = Materialize(host, v);
        v.elemKind = 0xEE;
        host->getArrayView(host, dense, &v);
    }
    const int32_t handle = host->newArrayResident(host, v.elemKind,
        v.rank, v.dims);
    NativeArrayView dst;
    host->getArrayView(host, handle, &dst);
    CopyHostToDevice(host, dst.data,
        static_cast<const uint8_t*>(v.data)
            + static_cast<size_t>(v.offset) * v.elemCellBytes,
        static_cast<size_t>(v.elemCount) * v.elemCellBytes);
    nlang::native::ReturnInt(ret, handle);
    return handle;
}

int32_t TransferToHost(NativeHost* host, uint8_t* ret,
                       const uint8_t* args) {
    NativeArrayView v;
    host->getArrayView(host, ArgInt(args, 0), &v);
    if (v.reserved[0] != 1) {
        nlang::native::ReturnInt(ret, ArgInt(args, 0));
        return ArgInt(args, 0);
    }
    const int32_t handle = host->newArrayShaped(host, v.elemKind,
        v.rank, v.dims);
    NativeArrayView dst;
    host->getArrayView(host, handle, &dst);
    //A contiguous device source transfers in one block; a strided one
    //walks the affine map per element (the staging discipline — the
    //device table's copy would need a mixed-residency pair).
    if (IsContiguous(v)) {
        CopyDeviceToHost(host, dst.data,
            static_cast<const uint8_t*>(v.data)
                + static_cast<size_t>(v.offset) * v.elemCellBytes,
            static_cast<size_t>(v.elemCount) * v.elemCellBytes);
    } else {
        uint8_t* out = static_cast<uint8_t*>(dst.data);
        for (int64_t i = 0; i < v.elemCount; ++i) {
            CopyDeviceToHost(host, out + i * v.elemCellBytes,
                static_cast<const uint8_t*>(v.data)
                    + static_cast<size_t>(AffineElementAt(v, i))
                        * v.elemCellBytes,
                v.elemCellBytes);
        }
    }
    nlang::native::ReturnInt(ret, handle);
    return handle;
}

//--- CLASS 3: metadata staging -------------------------------------------

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
    const uint8_t* srcBase = static_cast<const uint8_t*>(v.data);
    if (IsContiguous(v)) {
        //Single-block D2H: the whole payload in one hooks transfer
        //(large staged sources — gather's src — pay src bandwidth, not
        //per-element launch cost).
        CopyDeviceToHost(host, buf->data(),
            srcBase + static_cast<size_t>(v.offset) * v.elemCellBytes,
            bytes);
    } else {
        //Per-element affine reads: a strided view's logical cells are
        //not contiguous in the owner — each element lands at its own
        //affine address.
        for (int64_t i = 0; i < v.elemCount; ++i) {
            CopyDeviceToHost(host, buf->data() + i * v.elemCellBytes,
                srcBase + static_cast<size_t>(AffineElementAt(v, i))
                    * v.elemCellBytes,
                v.elemCellBytes);
        }
    }
    return staged;
}

void ReadIntListStaged(NativeHost* host, const uint8_t* args, int slot,
                       int32_t want, const char* who, int32_t* out) {
    NativeArrayView v;
    host->getArrayView(host, ArgInt(args, slot), &v);
    std::vector<uint8_t> stage;
    v = StageOperandToHost(host, v, &stage);
    if (v.elemKind != NEK_Int32 || v.rank != 1)
        Raise(host, NEXC_Base, std::string(who)
            + ": expects a rank-1 int array");
    const int32_t* cells = static_cast<const int32_t*>(v.data);
    int32_t at = 0;
    int64_t idx[nlang::kMaxArrayRank] = {0};
    int64_t addr = v.offset;
    for (int64_t i = 0; i < v.elemCount; ++i) {
        if (at >= nlang::kMaxArrayRank)
            Raise(host, NEXC_Base, std::string(who)
                + ": shape list longer than "
                + std::to_string(nlang::kMaxArrayRank) + " cells");
        out[at++] = cells[addr];
        StrideWalkAdvance(v, idx, &addr);
    }
    if (at != want)
        Raise(host, NEXC_Base, std::string(who) + ": expects an int[] of "
            + std::to_string(want) + " cells");
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
        //sibling operands' residency.
        const int32_t dense = host->newArrayResident(host,
            view->elemKind, view->rank, view->dims);
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
            msg += "); move them with tensor.toDevice/toHost";
            Raise(host, NEXC_Base, msg);
        }
    }
}

StagedWalkOut PrepareStagedWalkOut(NativeHost* host, uint8_t elemKind,
                                   int32_t rank, const int32_t* dims,
                                   const NativeArrayView& srcLike) {
    StagedWalkOut landing;
    if (srcLike.reserved[0] == 1 && g_backend.device) {
        //Device-resident source: the walk fills a HOST twin mint and
        //Land copies it into the device mint (the twin order keeps the
        //walk's target a plain host buffer).
        const int32_t twinHandle = host->newArrayShaped(host, elemKind,
            rank, dims);
        host->getArrayView(host, twinHandle, &landing.fill);
        landing.handle = host->newArrayResident(host, elemKind, rank,
            dims);
        host->getArrayView(host, landing.handle, &landing.device);
        landing.deviceIs = true;
        return landing;
    }
    landing.handle = host->newArrayShaped(host, elemKind, rank, dims);
    host->getArrayView(host, landing.handle, &landing.fill);
    return landing;
}

void LandStagedWalkOut(NativeHost* host, StagedWalkOut* landing) {
    if (!landing->deviceIs)
        return;
    CopyHostToDevice(host, landing->device.data, landing->fill.data,
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
