/*---
    VmExecutorOpsRef.cpp — v0.13.9 phase ③: the byref slot representation.
    The RefOf* family builds the (base heap-record handle, cell offset)
    pair for every bindable location (design doc §3.2); OP_RefHandle
    extracts the base handle back into a struct-valued slot; the
    OP_LoadRef/OP_StoreRef pair dereferences it. All ops are lock-free
    (cell-granularity value races only, the phase ② ruling) and allocate
    nothing — a byref value moves between slots as a raw 8-byte copy
    through the existing OP_VarLocal/OP_Assign pair.
---*/
#include "VmExecutor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstring>
#include <string>

namespace nlang {

namespace {
//Frame cell count for one dereference kind byte: scalar rows are 1 or 2
//cells by slot width, strings one cell (the handle's low half). Anything
//else is an emitter bug — byrefs bind scalars and strings only — and
//fails loudly instead of corrupting a record.
size_t RefDerefCells(uint8_t kind) {
    if (kind == RTK_String)
        return 1;
    const int prim = ScalarPrimIndexOfRtk(kind);
    if (prim < 0)
        throw std::runtime_error(
            "NLang VM: ref dereference on a non-bindable kind");
    return kScalarPrims[prim].slotWidth / kHeapCellBytes;
}
} //namespace

//Read and validate a byref pair; see VmExecutor.h for the contract.
int32_t VmExecutor::ByrefTarget(uint16_t slotOff, uint8_t* locals,
                                size_t cells, const char* opName,
                                size_t& cellOut) {
    int32_t pair[2];
    std::memcpy(pair, locals + slotOff, sizeof(pair));
    const int32_t handle = pair[0];
    if (handle <= 0 || static_cast<size_t>(handle) >= m_structHeap.size())
        throw std::runtime_error(std::string(opName)
            + ": corrupt byref base handle");
    const auto& record = m_structHeap[static_cast<size_t>(handle)];
    if (pair[1] < 0
        || static_cast<size_t>(pair[1]) + cells > record.size())
        throw std::runtime_error(std::string(opName)
            + ": byref cell window out of bounds");
    cellOut = static_cast<size_t>(pair[1]);
    return handle;
}

//Struct-typed local bind: the slot IS the record handle — pair (handle, 0).
void VmExecutor::OpRefOfLocal(BytecodeReader& reader, uint8_t* locals) {
    const uint16_t dst = reader.ReadUint16();
    const uint16_t src = reader.ReadUint16();
    int32_t handle;
    std::memcpy(&handle, locals + src, sizeof(handle));
    if (handle <= 0 || static_cast<size_t>(handle) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: OP_RefOfLocal on an invalid record slot");
    const int32_t pair[2] = {handle, 0};
    std::memcpy(locals + dst, pair, sizeof(pair));
}

//Promoted scalar/string local bind: the & binding established the
//local's stable boxing record (the emitter boxed once at the binding
//site); the slot low half carries its handle — pair (handle, 1), the
//boxing record's payload cell (a wide payload just extends into cell 2;
//the pair names the first).
void VmExecutor::OpRefOfPromoted(BytecodeReader& reader, uint8_t* locals) {
    const uint16_t dst = reader.ReadUint16();
    const uint16_t src = reader.ReadUint16();
    int32_t handle;
    std::memcpy(&handle, locals + src, sizeof(handle));
    if (handle <= 0 || static_cast<size_t>(handle) >= m_structHeap.size()
        || m_slotKinds[static_cast<size_t>(handle)] != RTK_Boxed)
        throw std::runtime_error(
            "NLang VM: OP_RefOfPromoted expects a boxed record slot");
    const int32_t pair[2] = {handle, 1};
    std::memcpy(locals + dst, pair, sizeof(pair));
}

//Record field bind: the emitter names the field's own cell (heap cells,
//not the byte offsets the field ops use — byref dereference walks cells
//directly).
void VmExecutor::OpRefOfField(BytecodeReader& reader, uint8_t* locals) {
    const uint16_t dst = reader.ReadUint16();
    const uint16_t base = reader.ReadUint16();
    const uint16_t cellOff = reader.ReadUint16();
    int32_t handle;
    std::memcpy(&handle, locals + base, sizeof(handle));
    if (handle <= 0 || static_cast<size_t>(handle) >= m_structHeap.size()
        || static_cast<size_t>(cellOff)
            >= m_structHeap[static_cast<size_t>(handle)].size())
        throw std::runtime_error(
            "NLang VM: OP_RefOfField on an invalid record or field offset");
    const int32_t pair[2] = {handle, static_cast<int32_t>(cellOff)};
    std::memcpy(locals + dst, pair, sizeof(pair));
}

//Array element bind: the flat-address walk of OP_LoadElementFlat, but
//the destination is the (record, cell) pair instead of the payload. A
//view's payload lives in its owner (depth-one peel), so the pair must
//name the OWNER record or every dereference would miss.
void VmExecutor::OpRefOfElement(BytecodeReader& reader, uint8_t* locals) {
    const uint16_t dst = reader.ReadUint16();
    const uint16_t arr = reader.ReadUint16();
    const uint16_t flatSlot = reader.ReadUint16();
    int32_t heapIdx, flat;
    std::memcpy(&heapIdx, locals + arr, sizeof(heapIdx));
    std::memcpy(&flat, locals + flatSlot, sizeof(flat));
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        RaiseNlangException(m_nullPtrExcClassIdx,
                            "NLang VM: null array access");
    const uint8_t kind = m_slotKinds[static_cast<size_t>(heapIdx)];
    if (kind != RTK_Array && kind != RTK_ArrayView)
        throw std::runtime_error(
            "NLang VM: OP_RefOfElement expects an array slot");
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const int32_t elemCount = slot[3];
    if (flat < 0 || flat >= elemCount)
        RaiseNlangException(m_oobExcClassIdx,
                            "NLang VM: array index out of bounds");
    uint16_t elemArrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (elemArrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error(
            "NLang VM: OP_RefOfElement on invalid array type");
    const int cells = ArrayElemCells(
        m_currModule->arrayTypes[elemArrayTypeIdx].elemKind);
    int32_t baseHandle = heapIdx;
    if (kind == RTK_ArrayView)
        baseHandle = slot[ArrayViewTargetCell(
            static_cast<uint8_t>(slot[2]))];
    //A view's payload is not contiguous: decode the linear index to
    //coordinates over the view's own dims, then apply the affine map
    //into owner payload space.
    const int64_t addr = kind == RTK_ArrayView
        ? ViewFlatElementAddress(slot, flat)
        : static_cast<int64_t>(flat);
    size_t payloadBase;
    //ElementPayloadRecord validates the view's depth-one target (throw on
    //a corrupt record) and returns the record owning the cells — the one
    //baseHandle names.
    auto& payload = ElementPayloadRecord(slot, kind, payloadBase);
    const int64_t cell = static_cast<int64_t>(payloadBase)
        + addr * cells;
    if (cell < 0
        || static_cast<size_t>(cell) + static_cast<size_t>(cells)
            > payload.size())
        throw std::runtime_error(
            "NLang VM: OP_RefOfElement cell window out of bounds");
    const int32_t pair[2] = {baseHandle, static_cast<int32_t>(cell)};
    std::memcpy(locals + dst, pair, sizeof(pair));
}

//Struct& consumption: extract the pair's base handle back into a
//struct-valued slot (offset 0) — the field ops take it from there.
void VmExecutor::OpRefHandle(BytecodeReader& reader, uint8_t* locals) {
    const uint16_t dst = reader.ReadUint16();
    const uint16_t src = reader.ReadUint16();
    int32_t handle;
    std::memcpy(&handle, locals + src, sizeof(handle));
    if (handle <= 0 || static_cast<size_t>(handle) >= m_structHeap.size())
        throw std::runtime_error(
            "NLang VM: OP_RefHandle on an invalid byref slot");
    const int32_t zero = 0;
    std::memcpy(locals + dst, &handle, sizeof(handle));
    std::memcpy(locals + dst + sizeof(handle), &zero, sizeof(zero));
}

//Dereference read: copy the addressed cells into dst (the LoadElementFlat
//copy discipline — narrow rows fill the low half, wide rows the slot).
void VmExecutor::OpLoadRef(BytecodeReader& reader, uint8_t* locals) {
    const uint8_t kind = reader.ReadByte();
    const uint16_t dst = reader.ReadUint16();
    const uint16_t src = reader.ReadUint16();
    const size_t cells = RefDerefCells(kind);
    size_t cell;
    const int32_t handle =
        ByrefTarget(src, locals, cells, "NLang VM: OP_LoadRef", cell);
    std::memcpy(locals + dst,
                m_structHeap[static_cast<size_t>(handle)].data() + cell,
                cells * sizeof(int32_t));
}

//Dereference write: copy src's addressed cells into the record.
void VmExecutor::OpStoreRef(BytecodeReader& reader, uint8_t* locals) {
    const uint8_t kind = reader.ReadByte();
    const uint16_t dstByref = reader.ReadUint16();
    const uint16_t srcVal = reader.ReadUint16();
    const size_t cells = RefDerefCells(kind);
    size_t cell;
    const int32_t handle =
        ByrefTarget(dstByref, locals, cells, "NLang VM: OP_StoreRef", cell);
    std::memcpy(m_structHeap[static_cast<size_t>(handle)].data() + cell,
                locals + srcVal, cells * sizeof(int32_t));
}

} // namespace nlang
