/*---
    VmExecutorDeviceArray.cpp — VM侧设备数组payload族
    从 VmExecutorAlloc.cpp 抽取（2026-11-16 P4 source-size收口，
    零行为变化）：设备mint、设备指针的存储与读取、透明元素访问
    与整块staging（元素操作码、GC、序列化/打印/视图物化共用）。
---*/
#include "VmExecutor.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace nlang {

namespace {
//A native side holds NativeHost* equal to &vmNativeHost.c (the first
//member); restore the wrapper.
VmNativeHost* Wrap(NativeHost* self) {
    return reinterpret_cast<VmNativeHost*>(self);
}
}

//The device mint (newArrayResident's path and the FollowActiveDevice
//arm): the hooks allocate and zero the payload; the record
//carries the 64-bit device pointer in its payload-area descriptor
//cells (two int32 cells) and the residency side vector marks it.
int32_t VmExecutor::AllocDeviceArrayOnHeap(uint16_t arrayTypeIdx,
    uint8_t rank, const int32_t* dims) {
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error("NLang VM: invalid array type index");
    const auto& at = m_currModule->arrayTypes[arrayTypeIdx];
    const int32_t size = ValidateArrayDims(rank, dims);
    if (!m_tensorHooksActive)
        RaiseNlangException(m_exceptionClassIdx,
            "tensor: no device backend active (backend=cpu); "
            "device-resident arrays need --tensor-backend mirror/cuda");
    if (at.elemKind != RTK_Int32 && at.elemKind != RTK_Long
        && at.elemKind != RTK_Float && at.elemKind != RTK_Double) {
        RaiseNlangException(m_exceptionClassIdx,
            "tensor: only int/long/float/double arrays can live on the "
            "device (byte/ubyte are host I/O buffers)");
    }
    const int cells = ArrayElemCells(at.elemKind);
    const size_t bytes = static_cast<size_t>(size) * cells
        * kHeapCellBytes;
    NoteGcPressure();
    const int32_t heapIdx = TakeHeapSlot();
    //Header-only record + the 2-cell device-pointer descriptor.
    const int32_t totalSlots =
        static_cast<int32_t>(ArrayHeaderCells(rank)) + 2;
    m_structHeap[static_cast<size_t>(heapIdx)].assign(totalSlots, 0);
    m_slotKinds[static_cast<size_t>(heapIdx)] = RTK_Array;
    m_slotStructIdx[static_cast<size_t>(heapIdx)] = arrayTypeIdx;
    m_instDescByHeap[static_cast<size_t>(heapIdx)] = nullptr;
    auto& record = m_structHeap[static_cast<size_t>(heapIdx)];
    record[0] = RTK_Array;
    record[1] = arrayTypeIdx;
    record[2] = rank;
    record[3] = size;
    for (uint8_t d = 0; d < rank; ++d) record[4 + d] = dims[d];
    void* devicePtr = m_tensorHooks.alloc(bytes);
    m_tensorHooks.zero(devicePtr, bytes);
    SetDevicePayload(heapIdx, devicePtr);
    m_arrResidency[static_cast<size_t>(heapIdx)] = 1;
    return heapIdx;
}

//The spec D2 kind/mechanism gate: language creation of a numeric
//four-kind array with an active device backend mints device-resident;
//byte/ubyte and every ForceHost caller stay host.
bool VmExecutor::FollowActiveDeviceMint(uint16_t arrayTypeIdx,
                                       ArrayResidency residency) {
    if (residency != ArrayResidency::FollowActiveDevice
        || !m_tensorHooksActive)
        return false;
    const auto& at = m_currModule->arrayTypes[arrayTypeIdx];
    return at.elemKind == RTK_Int32 || at.elemKind == RTK_Long
        || at.elemKind == RTK_Float || at.elemKind == RTK_Double;
}

//The device pointer lives in the record's payload-area descriptor
//cells (two int32 cells right after the header — the payload area of
//a device record is the descriptor, there are no inline elements).
void* VmExecutor::DevicePayloadOf(int32_t heapIdx) const {
    auto& record = m_structHeap[static_cast<size_t>(heapIdx)];
    const size_t base = ArrayPayloadBase(record);
    void* ptr = nullptr;
    std::memcpy(&ptr, &record[base], sizeof(ptr));
    return ptr;
}

void VmExecutor::SetDevicePayload(int32_t heapIdx, void* devicePtr) {
    auto& record = m_structHeap[static_cast<size_t>(heapIdx)];
    const size_t base = ArrayPayloadBase(record);
    //The 64-bit pointer spans the two descriptor cells exactly (8 bytes
    //== 2 int32 cells), so this memcpy covers the whole payload area —
    //the record was zeroed at mint, nothing is left to clear. (An earlier
    //memset of &record[base + 2] indexed one past the end: base + 2 ==
    //totalSlots, so Debug's std::vector bounds check tripped on it.)
    std::memcpy(&record[base], &devicePtr, sizeof(devicePtr));
}

bool VmExecutor::IsDeviceArray(int32_t heapIdx) const {
    return m_arrResidency[static_cast<size_t>(heapIdx)] == 1;
}

//Transparent element access on device records: one-element synchronous
//transfer through the hooks (spec D2's industry-standard semantics —
//PyTorch tensor[0] and CuPy arr[i] do the same implicit transfer).
//The cell is the 8-byte frame cell; 4-byte kinds live in the low half.
void VmExecutor::DeviceLoadElement(int32_t heapIdx, int64_t elemCell,
    int64_t cellBytes, uint8_t* cell) const {
    std::memset(cell, 0, NLANG_VALUE_SIZE);
    uint8_t* base = static_cast<uint8_t*>(DevicePayloadOf(heapIdx));
    m_tensorHooks.copyD2H(cell,
        base + static_cast<size_t>(elemCell) * cellBytes,
        static_cast<size_t>(cellBytes));
}

void VmExecutor::DeviceStoreElement(int32_t heapIdx, int64_t elemCell,
    int64_t cellBytes, const uint8_t* cell) const {
    uint8_t* base = static_cast<uint8_t*>(DevicePayloadOf(heapIdx));
    m_tensorHooks.copyH2D(
        base + static_cast<size_t>(elemCell) * cellBytes, cell,
        static_cast<size_t>(cellBytes));
}

void VmExecutor::DeviceStageToHost(int32_t heapIdx, void* dst,
    size_t bytes) const {
    m_tensorHooks.copyD2H(dst, DevicePayloadOf(heapIdx), bytes);
}

void VmExecutor::DeviceStageFromHost(int32_t heapIdx, const void* src,
    size_t bytes) const {
    m_tensorHooks.copyH2D(DevicePayloadOf(heapIdx), src, bytes);
}

//The view materialization's device arm: the owner's payload stages
//before the walk when it lives on the device (one bulk D2H; the mint
//is ForceHost — the materialized copy is host-resident by definition).
//The staging covers the OWNER's whole payload: the affine addresses
//range over the owner's element space (the view's elemCount is only
//the gathered subset — a slice view's addresses reach past it).
bool VmExecutor::StageDeviceOwnerForView(int32_t viewHeapIdx, uint8_t rank,
                                         int cells,
                                         std::vector<int32_t>& staged) {
    const auto& view = m_structHeap[static_cast<size_t>(viewHeapIdx)];
    const int32_t ownerIdx =
        view[ArrayViewTargetCell(rank)];
    if (m_arrResidency[static_cast<size_t>(ownerIdx)] != 1)
        return false;
    const int32_t ownerCount =
        m_structHeap[static_cast<size_t>(ownerIdx)][3];
    staged.resize(static_cast<size_t>(ownerCount) * cells);
    DeviceStageToHost(ownerIdx, staged.data(),
        staged.size() * sizeof(int32_t));
    return true;
}

//The formatter's device arm: a device-resident record's payload area is
//the 2-cell device-pointer descriptor — stage the real payload into a
//record-shaped local before the bracket walk (one bulk D2H). The caller
//tests stagedSlot.empty() to decide between the staged copy and the
//heap record itself.
void VmExecutor::StageDeviceArrayForFormat(int32_t heapIdx,
    std::vector<int32_t>& stagedSlot) const {
    if (m_arrResidency[static_cast<size_t>(heapIdx)] != 1)
        return;
    stagedSlot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t elemCount = stagedSlot[3];
    const int stageCells = ArrayElemCells(
        stagedSlot[1] < m_currModule->arrayTypes.size()
            ? m_currModule->arrayTypes[
                static_cast<size_t>(stagedSlot[1])].elemKind
            : RTK_Int32);
    stagedSlot.resize(ArrayPayloadBase(stagedSlot)
        + static_cast<size_t>(elemCount) * stageCells);
    DeviceStageToHost(heapIdx, stagedSlot.data()
        + ArrayPayloadBase(stagedSlot),
        static_cast<size_t>(elemCount) * stageCells * 4);
}

//The consumer-side gate (spec D2, called from the DLL-native dispatch):
//a device-resident array reaching a NON-tensor native hands that
//native a device pointer it cannot dereference — raise by name
//instead. The gate sits in the DLL-native dispatch because the
//embedded-host arm routes array reads/writes through HostArrayGet/
//SetCell, which already carry device arms. Param types come from the
//callee's isParam locals rows in declaration (offset) order; a
//device-resident array argument to the tensor package is fine — its
//slots are the device kernels.
void VmExecutor::CheckDeviceArrayArgs(const CompiledFunction& callee,
    uint16_t callParamBase, uint8_t* locals) {
    if (!m_tensorHooksActive || callee.name.rfind("tensor.", 0) == 0)
        return;
    std::vector<const LocalDescriptor*> params;
    for (const auto& ld : callee.locals)
        if (ld.isParam)
            params.push_back(&ld);
    std::sort(params.begin(), params.end(),
        [](const LocalDescriptor* a, const LocalDescriptor* b) {
            return a->offset < b->offset;
        });
    for (uint16_t i = 0; i < callee.paramCount
            && i < params.size(); ++i) {
        const uint8_t pk = params[i]->typeKind;
        if (pk != RTK_Array && pk != RTK_ArrayView)
            continue;
        int32_t handle = 0;
        std::memcpy(&handle, locals + callParamBase
            + static_cast<size_t>(i) * kFrameSlotBytes,
            sizeof(handle));
        if (handle > 0
            && static_cast<size_t>(handle) < m_structHeap.size()
            && m_arrResidency[static_cast<size_t>(handle)] == 1) {
            RaiseNlangException(m_exceptionClassIdx,
                "device array passed to a host-only native ("
                + callee.name + "); move it with tensor.toHost");
        }
    }
}

//--- v13 tensor device side (contracts in NativeHost.h) ----------------
//Registration is an init-sequence event (the tensor module's
//nlang_native_init runs before any user frame or carrier); a second
//registration is a module contract violation. All hook slots must be
//populated — the VM routes device records through them and never links
//vendor runtimes itself.

void VmExecutor::NativeSetTensorDeviceHooks(NativeHost* self,
    const TensorDeviceHooks* hooks) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    if (e->m_tensorHooksActive)
        e->RaiseNlangExceptionBase(
            "setTensorDeviceHooks: hooks already registered for this "
            "executor");
    if (hooks == nullptr || hooks->alloc == nullptr
        || hooks->zero == nullptr || hooks->free == nullptr
        || hooks->copyH2D == nullptr || hooks->copyD2H == nullptr
        || hooks->sync == nullptr) {
        e->RaiseNlangExceptionBase(
            "setTensorDeviceHooks: incomplete hook set");
    }
    e->m_tensorHooks = *hooks;
    e->m_tensorHooksActive = true;
}

int32_t VmExecutor::NativeNewArrayResident(NativeHost* self,
    uint8_t elemKind, int32_t rank, const int32_t* dims) {
    VmExecutor* e = Wrap(self)->executor;
    GilEnsureGuard gil(*e);
    if (elemKind != NEK_Int32 && elemKind != NEK_Int64
        && elemKind != NEK_Float32 && elemKind != NEK_Float64) {
        e->RaiseNlangExceptionBase(
            "newArrayResident: only int/long/float/double arrays can "
            "live on the device (byte/ubyte are host I/O buffers)");
    }
    const uint16_t arrayTypeIdx = e->FindScalarArrayTypeIdx(elemKind,
        static_cast<uint8_t>(rank));
    const int32_t handle = e->AllocDeviceArrayOnHeap(arrayTypeIdx,
        static_cast<uint8_t>(rank), dims);
    e->RootMintedNativeHandle(handle);
    return handle;
}

//The atomic snapshot+discard composite (spec D3): one mutex-protected
//step — a read/discard pair would race between concurrent carriers and
//hand two uniform draws the same snapshot.
void VmExecutor::NativeSnapshotRngForDevice(NativeHost* self,
    uint32_t* out624, uint32_t* outPosition, int64_t n) {
    VmExecutor* e = Wrap(self)->executor;
    std::lock_guard<std::mutex> rngGuard(e->m_rngMutex);
    e->m_rng.Snapshot(out624, outPosition);
    e->m_rng.discard(n);
}

} // namespace nlang
