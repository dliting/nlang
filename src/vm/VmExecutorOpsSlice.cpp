/*---
    VmExecutorOpsSlice.cpp — OP_MakeArrayView runtime (v0.12.7 Task 11):
    the slice-subscript executor. Normalizes per-axis endpoints and
    steps (defaults by step direction, negative endpoints len+neg,
    clipping), folds scalar axes into the offset, and mints the
    depth-one view through AllocViewOnHeap — the single view-creation
    choke point. Split from VmExecutorOpsArrayN.cpp at birth
    (source-size governance: that file was already near the cap).
    K8 lock classification follows the array family: lock-free record
    reads, allocation through the per-carrier shards, error paths
    re-acquire inside RaiseNlangException.
---*/
#include "VmExecutor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <algorithm>
#include <cstring>

namespace nlang {

namespace {
//Parameter staging shared with the emitter: per axis 3 int32 slots
//(start/stop/step, INT32_MIN = the default sentinel), followed by
//ceil(4*rank/32) mask slots — 4 bits per axis (start/stop/step
//present + axis-is-scalar). An explicitly written part that evaluates
//to INT32_MIN is indistinguishable from the default by value, so the
//mask marks it present and the executor rejects it.
constexpr int32_t kSliceDefaultSentinel = INT32_MIN;
constexpr uint32_t kSliceBitStart = 0x1;
constexpr uint32_t kSliceBitStop = 0x2;
constexpr uint32_t kSliceBitStep = 0x4;
constexpr uint32_t kSliceBitScalar = 0x8;

int32_t ReadSliceParam(const uint8_t* locals, uint16_t base, uint16_t idx) {
    int32_t value = 0;
    std::memcpy(&value, locals + base + idx * kFrameSlotBytes, sizeof(value));
    return value;
}

//The presence bits of axis d out of the staged mask words.
uint32_t SliceAxisBits(const uint8_t* locals, uint16_t paramsBase,
    uint16_t rank, uint16_t d) {
    const int32_t word = ReadSliceParam(locals, paramsBase,
        static_cast<uint16_t>(3 * rank + d / 8));
    return (static_cast<uint32_t>(word) >> (4 * (d % 8))) & 0xFu;
}

//Clip a written endpoint into the axis window: positive steps address
//[0, len], negative steps address [-1, len-1] (both ends inclusive as
// iteration bounds — -1 means "include index 0 and stop").
int32_t ClipSliceEndpoint(int32_t value, int32_t len, bool positiveStep) {
    if (value < 0)
        value += len;
    if (positiveStep)
        return std::min(std::max(value, 0), len);
    return std::min(std::max(value, -1), len - 1);
}

//The stride recorded for a folded slice axis: the true affine step in
//owner space (the view-record layout invariant — never a folded 0,
//which would mark a writable axis as broadcast). The one corner where
//the step overflows int32 on a count<=1 axis is address-irrelevant and
//takes placeholder 1, keeping the record free of a fake broadcast
//marker; a count>1 overflow is rejected by the caller beforehand.
int32_t SliceAxisStride(int64_t axisStride) {
    if (axisStride > INT32_MAX || axisStride < INT32_MIN)
        return 1;
    return static_cast<int32_t>(axisStride);
}
}  //namespace

//v0.12.7 Task 11: fold ONE slice axis into the accumulating view. The
//written-part reads reject the default sentinel (presence-masked) and
//the zero step; normalization then narrows the span (defaults follow
//the step direction, negative endpoints len+neg, both ends clip) and
//the kept stride is the owner stride scaled by the step — the true
//affine step per the view-record layout invariant, so the record
//never carries a fake broadcast marker.
void VmExecutor::FoldOneSliceAxis(VmExecutor* e, uint16_t axis,
    uint32_t bits, const int32_t* dims, const int32_t* strides,
    const uint8_t* locals, uint16_t paramsBase, SliceFoldState& st) {
    int32_t step = 1;
    if (bits & kSliceBitStep) {
        step = ReadSliceParam(locals, paramsBase,
            static_cast<uint16_t>(3 * axis + 2));
        if (step == kSliceDefaultSentinel)
            e->RaiseNlangExceptionBase("slice endpoint/step out of range");
        if (step == 0)
            e->RaiseNlangExceptionBase("slice step cannot be zero");
    }
    const bool positiveStep = step > 0;
    const int32_t len = dims[axis];
    int32_t start = positiveStep ? 0 : len - 1;
    if (bits & kSliceBitStart) {
        start = ReadSliceParam(locals, paramsBase,
            static_cast<uint16_t>(3 * axis));
        if (start == kSliceDefaultSentinel)
            e->RaiseNlangExceptionBase("slice endpoint/step out of range");
        start = ClipSliceEndpoint(start, len, positiveStep);
    }
    int32_t stop = positiveStep ? len : -1;
    if (bits & kSliceBitStop) {
        stop = ReadSliceParam(locals, paramsBase,
            static_cast<uint16_t>(3 * axis + 1));
        if (stop == kSliceDefaultSentinel)
            e->RaiseNlangExceptionBase("slice endpoint/step out of range");
        stop = ClipSliceEndpoint(stop, len, positiveStep);
    }
    //Element count toward the step direction (ceil division on the
    //narrowed span; empty axes legitimately produce 0).
    int32_t count = 0;
    if (positiveStep && stop > start)
        count = (stop - start + step - 1) / step;
    else if (!positiveStep && stop < start)
        count = (start - stop - step - 1) / -step;
    const int64_t axisStride =
        static_cast<int64_t>(strides[axis]) * step;
    if (count > 1
        && (axisStride > INT32_MAX || axisStride < INT32_MIN))
        e->RaiseNlangException(e->m_oobExcClassIdx,
            "view range exceeds the target array bounds");
    st.newStrides[st.viewRank] = SliceAxisStride(axisStride);
    st.newDims[st.viewRank] = count;
    st.offset += static_cast<int64_t>(start) * strides[axis];
    ++st.viewRank;
}

//v0.12.7 Task 11: walk the subscript's axes once — scalar axes fold
//into the offset and drop from the result, slice axes fold through
//FoldOneSliceAxis. The base may be an owning record (row-major strides
//folded from 0) or a view (its recorded strides/offset already in
//owner space). Requires: the record's rank equals the axis count.
void VmExecutor::FoldSliceAxes(VmExecutor* e, uint16_t rank,
    uint16_t paramsBase, const uint8_t* locals, uint8_t baseKind,
    const std::vector<int32_t>& record, SliceFoldState& st) {
    const int32_t* dims = record.data() + 4;
    int32_t rowMajor[kMaxArrayRank];
    const int32_t* strides;
    if (baseKind == RTK_ArrayView) {
        strides = record.data() + ArrayViewStridesBase(
            static_cast<uint8_t>(rank));
        st.offset = record[ArrayViewOffsetCell(
            static_cast<uint8_t>(rank))];
    } else {
        int32_t stride = 1;
        for (int32_t d = rank - 1; d >= 0; --d) {
            rowMajor[d] = stride;
            stride *= dims[d];
        }
        strides = rowMajor;
        st.offset = 0;
    }
    for (uint16_t d = 0; d < rank; ++d) {
        const uint32_t bits = SliceAxisBits(locals, paramsBase, rank, d);
        if (bits & kSliceBitScalar) {
            //Scalar axis: the index rides the start slot and folds
            //into the offset (bounds-named, like the element loads).
            const int32_t idx = ReadSliceParam(locals, paramsBase,
                static_cast<uint16_t>(3 * d));
            if (idx < 0 || idx >= dims[d])
                e->RaiseNlangException(e->m_oobExcClassIdx,
                    "array index out of bounds (dim "
                    + std::to_string(d) + ")");
            st.offset += static_cast<int64_t>(idx) * strides[d];
            continue;
        }
        FoldOneSliceAxis(e, d, bits, dims, strides, locals, paramsBase,
            st);
    }
}

//v0.12.7 Task 11: mint the slice-subscript view. Element-type identity
//is validated against the owner's row inside AllocViewOnHeap
//(ValidateViewTypeMatch); the view's own type row rode the opcode.
//All shape math completes on locals before AllocViewOnHeap — the slot
//take may grow the store and invalidate the record reference.
void VmExecutor::OpMakeArrayView(BytecodeReader& reader, uint8_t* locals) {
    const uint16_t dst = reader.ReadUint16();
    const uint16_t arrSlot = reader.ReadUint16();
    const uint16_t arrayTypeIdx = reader.ReadUint16();
    const uint16_t rank = reader.ReadUint16();
    const uint16_t paramsBase = reader.ReadUint16();
    if (rank < 1 || rank > kMaxArrayRank)
        throw std::runtime_error(
            "NLang VM: OP_MakeArrayView rank out of range");
    int32_t arrHandle = 0;
    std::memcpy(&arrHandle, locals + arrSlot, sizeof(arrHandle));
    if (arrHandle <= 0
        || static_cast<size_t>(arrHandle) >= m_slotKinds.size())
        RaiseNlangException(m_nullPtrExcClassIdx, "array is null");
    const uint8_t kind = m_slotKinds[static_cast<size_t>(arrHandle)];
    if (kind != RTK_Array && kind != RTK_ArrayView)
        RaiseNlangExceptionBase("sliced value is not an array");
    const auto& record = m_structHeap[static_cast<size_t>(arrHandle)];
    //The Array<E> spelling has no static rank — the record itself
    //verifies against the staged axis count (mirrors the Dyn element
    //pair's rank gate).
    if (record[2] != static_cast<int32_t>(rank))
        RaiseNlangException(m_oobExcClassIdx,
            "array rank does not match the subscript");
    SliceFoldState st;
    FoldSliceAxes(this, rank, paramsBase, locals, kind, record, st);
    const int32_t handle = AllocViewOnHeap(arrayTypeIdx, st.viewRank,
        st.newDims, st.newStrides,
        static_cast<int32_t>(st.offset), arrHandle);
    std::memcpy(locals + dst, &handle, sizeof(handle));
    NoteGcPressure();
}

}  //namespace nlang
