/*---
backend_transfer.cpp — the payload-transfer choke points (CLASS 2) and
the metadata staging (CLASS 3) over the active backend's device hooks:
the toDevice/toHost transfers, the staged host walks and the staged
int-list reads (design 2026-11-15-p3-routing-design). Device ABI v2:
every transfer names the payload's device (the view's reserved[1]).
Split from backend_registry.cpp for the source-size guard (the
registry file caps at 500 lines).
---*/
#include "backend_registry.h"

#include <string>
#include <vector>

namespace tensor {

using nlang::native::ArgInt;
using nlang::native::Raise;

namespace {

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

} // namespace

//--- CLASS 2: transfers --------------------------------------------------

void CopyHostToDevice(NativeHost* host, int32_t device, void* deviceDst,
                      const void* hostSrc, size_t bytes) {
    const DeviceBackendState& backend = ActiveDeviceBackend();
    if (!backend.device)
        Raise(host, NEXC_Base,
              "tensor: no device backend active (backend=cpu)");
    RaiseStatus(host, backend.hooks.copyH2D(device, deviceDst, hostSrc,
        bytes));
}

void CopyDeviceToHost(NativeHost* host, int32_t device, void* hostDst,
                      const void* deviceSrc, size_t bytes) {
    const DeviceBackendState& backend = ActiveDeviceBackend();
    if (!backend.device)
        Raise(host, NEXC_Base,
              "tensor: no device backend active (backend=cpu)");
    RaiseStatus(host, backend.hooks.copyD2H(device, hostDst, deviceSrc,
        bytes));
}

int32_t TransferToDevice(NativeHost* host, uint8_t* ret,
                         const uint8_t* args) {
    NativeArrayView v;
    host->getArrayView(host, ArgInt(args, 0), &v);
    //A same-device operand returns as-is (the array API standard's
    //to_device contract): under backend=cpu EVERY array is already on
    //the active device, so toDevice is a no-op there.
    if (!ActiveDeviceBackend().device || v.reserved[0] == 1) {
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
    //toDevice lands on the ACTIVE device — device 0 until the surface
    //names a target (the device argument lands with cross-device
    //copies); the mint pins the record's device entry to match.
    const int32_t handle = host->newArrayResident(host, v.elemKind,
        v.rank, v.dims, 0);
    NativeArrayView dst;
    host->getArrayView(host, handle, &dst);
    CopyHostToDevice(host, 0, dst.data,
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
        CopyDeviceToHost(host, v.reserved[1], dst.data,
            static_cast<const uint8_t*>(v.data)
                + static_cast<size_t>(v.offset) * v.elemCellBytes,
            static_cast<size_t>(v.elemCount) * v.elemCellBytes);
    } else {
        uint8_t* out = static_cast<uint8_t*>(dst.data);
        for (int64_t i = 0; i < v.elemCount; ++i) {
            CopyDeviceToHost(host, v.reserved[1],
                out + i * v.elemCellBytes,
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
    if (!ActiveDeviceBackend().device || v.reserved[0] != 1)
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
    staged.reserved[1] = 0;  //host payload — the device id is meaningless
    staged.strides = stridesOut;
    const uint8_t* srcBase = static_cast<const uint8_t*>(v.data);
    if (IsContiguous(v)) {
        //Single-block D2H: the whole payload in one hooks transfer
        //(large staged sources — gather's src — pay src bandwidth, not
        //per-element launch cost).
        CopyDeviceToHost(host, v.reserved[1], buf->data(),
            srcBase + static_cast<size_t>(v.offset) * v.elemCellBytes,
            bytes);
    } else {
        //Per-element affine reads: a strided view's logical cells are
        //not contiguous in the owner — each element lands at its own
        //affine address.
        for (int64_t i = 0; i < v.elemCount; ++i) {
            CopyDeviceToHost(host, v.reserved[1],
                buf->data() + i * v.elemCellBytes,
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

} // namespace tensor
