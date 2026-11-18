/*---
    backend_registry.h — the tensor module's device-backend selection
    and the THREE payload-transfer choke points (P2/P3 consolidation,
    design 2026-11-15-p3-routing-design.md).

    NLANG_TENSOR_BACKEND resolves at module init: auto (default —
    probe the plugin list, first working device wins, silent cpu
    fallback), cpu, mirror (the P2 test backend — malloc hooks + the
    cpu table), cuda (the P3 plugin). InitBackendSelection registers
    the hooks into the VM.

    Architectural ruling (the mirror backend masked these classes — its
    table IS the cpu table and its "device" memory IS host memory, so
    any mis-routing silently worked):

    Every payload access in the tensor dispatchers is exactly ONE of
    three classes, each with ONE sanctioned entry point here:

    1. COMPUTE — same-residency operands. Route PER CALL by the primary
       operand's residency: host operands run the cpu table even under
       a device backend (PyTorch's discipline — CPU tensors run CPU
       kernels), device operands run the active device table.
       → OpsForResidency(residency).

    2. TRANSFER — host↔device (toDevice/toHost/uniform's landing).
       Never a compute slot (either table dereferences the wrong side);
       the hooks' byte copies are the mechanism. Strided sources
       materialize by SOURCE residency first (MaterializeByResidency).
       → CopyHostToDevice / CopyDeviceToHost / TransferToDevice /
         TransferToHost.

    3. METADATA — shape-determining reads (dims/indices/labels/axes).
       A device-resident operand stages to host FIRST (single-block
       D2H when contiguous, per-element affine reads when strided);
       the staged readers are the only sanctioned spelling.
       → StageOperandToHost / ReadIntListStaged.

    ActiveTensorOps() is gone (the P3 routing design): the dispatchers'
    compute paths use OpsForResidency alone, so a toHost result under
    cuda runs host kernels.
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
    const void* ops = nullptr; //the device TensorOps table (cuda)
};

//Resolved once at module init; immutable afterwards (the hooks
//discipline — plan M7).
const DeviceBackendState& ActiveDeviceBackend();

//Called once from nlang_native_init (before any registration use):
//parses NLANG_TENSOR_BACKEND, prepares the hooks, and registers them
//into the VM when the backend is device. Explicit cuda with a missing
//plugin fails fast (spec D3a); an unknown name reports once on stderr
//and runs on cpu.
void InitBackendSelection(NativeHost* host);

//--- CLASS 1: compute routing -------------------------------------------

//Per-call table routing by the primary operand's residency (the
//operands agreed — AssertSameResidency ran): 0 = host → the cpu table
//(even under a device backend), 1 = device → the active device table
//(the cpu table under mirror — its ops stay null — the plugin table
//under cuda).
const TensorOps& OpsForResidency(uint8_t residency);

//--- CLASS 2: transfers --------------------------------------------------

//Dense byte transfers through the hooks. No compute slots, no
//residency mixing. Raise by name when no device backend is active.
void CopyHostToDevice(NativeHost* host, void* deviceDst,
                      const void* hostSrc, size_t bytes);
void CopyDeviceToHost(NativeHost* host, void* hostDst,
                      const void* deviceSrc, size_t bytes);

//toDevice/toHost bodies: same-residency operands return the original
//handle (the array API to_device contract; toDevice is a no-op under
//backend=cpu). Strided sources materialize by SOURCE residency
//(MaterializeByResidency), then the hooks transfer lands the payload.
int32_t TransferToDevice(NativeHost* host, uint8_t* ret,
                         const uint8_t* args);
int32_t TransferToHost(NativeHost* host, uint8_t* ret,
                       const uint8_t* args);

//--- CLASS 3: metadata staging -------------------------------------------

//Metadata staging (plan C4): if v is device-resident, copies its
//payload into buf — single-block D2H when the view is contiguous,
//per-element affine hooks reads when strided (element i at
//data + (offset + sum(idx*strides)) * cellBytes) — and returns a HOST
//view over the staging. The staged view's dense strides live in buf
//AFTER the payload (the returned view's pointers never dangle). Host
//operands return unchanged (buf stays empty). The staging lives as
//long as buf.
NativeArrayView StageOperandToHost(NativeHost* host, NativeArrayView& v,
                                   std::vector<uint8_t>* buf);

//The staged exact-length reader (ReadIntList's twin): stages slot's
//operand when device-resident, then reads `want` cells. The ONLY
//sanctioned metadata read for the views family.
void ReadIntListStaged(NativeHost* host, const uint8_t* args, int slot,
                       int32_t want, const char* who, int32_t* out);

//--- materialization by residency ---------------------------------------

//The host-materialization family's residency router (ContiguousOperand
///FlattenMaterialized/reshape's copy arm): contiguous views pass
//through unchanged; a strided HOST source materializes via the cpu
//table (host memory); a strided DEVICE source materializes on-device
//(newArrayResident + the device table's copy — D2D). Returns the
//handle backing the computation and refills *view.
int32_t MaterializeByResidency(NativeHost* host, int32_t handle,
                               NativeArrayView* view);

//--- creation mints ------------------------------------------------------

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

//--- the staged-walk landing ---------------------------------------------

//The CPU-walk families' shared output landing (the gather R5 shape's
//one form): Prepare mints the output like `srcLike` and returns the
//view the walk fills — a device-resident source gets a HOST twin
//mint (the walk writes host memory), a host-resident source gets the
//mint itself. Land publishes the filled view: the device shape copies
//it H2D (a device backend is active by construction — a device view
//only exists with one), the host shape is already in place. Pairing
//the two calls is the sanctioned way to land a CPU walk, so a family
//cannot hand a host mint to a device transfer (the backend=cpu raise,
//or a host pointer handed to the cuda copy under cuda — the v0.14.6
//sort defect class).
struct StagedWalkOut {
    int32_t handle = 0;     //the output array's handle
    NativeArrayView fill;   //the view the CPU walk writes
    bool deviceIs = false;  //the device-side landing is pending
    NativeArrayView device; //the landing target (deviceIs only)
};

StagedWalkOut PrepareStagedWalkOut(NativeHost* host, uint8_t elemKind,
                                   int32_t rank, const int32_t* dims,
                                   const NativeArrayView& srcLike);
void LandStagedWalkOut(NativeHost* host, StagedWalkOut* landing);

//--- the axis family's shared validations --------------------------------

//The axis contract every per-axis family shares: the axis in range —
//the diagnostic names the axis and the rank (the §18.7 shape).
void RejectAxisOutOfRange(NativeHost* host, const std::string& who,
                          const NativeArrayView& a, int32_t axis);

//The per-line families' empty convention (sum/mean's scalar arm, the
//axis arms, sort/argsort): a zero-size source has no line to walk and
//no cell to start a scan from — the walks below divide the line count
//by the axis dim. The position families (gather/scatter) keep their
//own shape instead: a zero AXIS DIM raises, a zero-size source with a
//nonzero axis is a legitimate empty result.
void RejectZeroSizeSource(NativeHost* host, const std::string& who,
                          const NativeArrayView& a);

//The mixed-residency diagnostic (spec D6's format): raises when the
//operands disagree on residency.
void AssertSameResidency(NativeHost* host, const NativeArrayView* operands,
                         size_t count);

} // namespace tensor
