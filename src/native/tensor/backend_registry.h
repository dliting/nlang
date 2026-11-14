/*---
    backend_registry.h — the tensor module's device-backend selection
    and staging helpers (P2 T2.9-T2.11).

    NLANG_TENSOR_BACKEND resolves at module init: cpu (default, no
    hooks), mirror (the P2 test backend — malloc hooks + the cpu table,
    so every device path runs on host memory), cuda (the P3 plugin).
    InitBackendSelection registers the hooks into the VM through
    setTensorDeviceHooks when the backend is device; the tensor
    dispatchers read ActiveDeviceBackend() for the residency gate and
    the metadata staging transfers.
---*/
#pragma once

#include "TensorOps.h"

#include <nlang/vm/NativeHost.h>

#include <string>
#include <vector>

namespace tensor {

struct DeviceBackendState {
    TensorDeviceHooks hooks = {};
    bool device = false;      //hooks registered into the VM
    std::string name = "cpu"; //"cpu" / "mirror" / "cuda"
};

//Resolved once at module init; immutable afterwards (the hooks
//discipline — plan M7).
const DeviceBackendState& ActiveDeviceBackend();

//Called once from nlang_native_init (before any registration use):
//parses NLANG_TENSOR_BACKEND, prepares the hooks, and registers them
//into the VM when the backend is device. Explicit cuda with a missing
//plugin raises by name (spec D3a fail-fast); unknown names fall back
//to cpu (the KernelOps-era selector contract).
void InitBackendSelection(NativeHost* host);

//Metadata staging (plan C4): if v is device-resident, copies its
//payload into buf (through the hooks) and returns a HOST view over
//the staging — shape-determining reads (dims/indices) never
//dereference device pointers. Host operands return unchanged (buf
//stays empty). The staging lives as long as buf.
NativeArrayView StageOperandToHost(NativeHost* host, NativeArrayView& v,
                                   std::vector<uint8_t>* buf);

//The creation family's residency dispatch (spec D2): the tensor
//creation family follows the active device — device backends mint via
//newArrayResident, cpu keeps newArrayShaped.
int32_t MintArray(NativeHost* host, uint8_t elemKind, int32_t rank,
                  const int32_t* dims);
int32_t MintArray1(NativeHost* host, uint8_t elemKind, int32_t length);

//The output mint follows the operands' residency (plan T2.11): a
//device-resident `like` operand mints device (newArrayResident), a
//host one mints host (newArrayShaped).
int32_t MintOutputLike(NativeHost* host, uint8_t elemKind, int32_t rank,
                       const int32_t* dims, const NativeArrayView& like);

//The mixed-residency diagnostic (spec D6's format): raises when the
//operands disagree on residency.
void AssertSameResidency(NativeHost* host, const NativeArrayView* operands,
                         size_t count);

} // namespace tensor
