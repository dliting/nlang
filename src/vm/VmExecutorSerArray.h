/*---
    VmExecutorSerArray.h — the value-form serialization templates
    (v0.12.7 Task 13 array content; final-review C4 boxed values),
    split from VmExecutorSer.h for the file-size guard. Same shape as
    the struct/class templates: definitions live in a header because
    instantiation happens in the consuming TUs; include
    VmExecutorSer.h (which includes this file) rather than this one
    directly.
---*/
#pragma once
#include "VmExecutorSer.h"

namespace nlang {

//v0.12.7 Task 13: array content serialization. Tag 3 extends the
//class-ref tag space (0 null / 1 new object / 2 back-ref): an array
//value written by writeObject or a struct/class array field. The
//payload is self-describing — elemKind byte, the struct table key for
//struct elements (the reader rebuilds by name, mirroring classes),
//rank byte, dims — so readArray reconstructs without an external type
//literal. Views materialize first: the record written is the view's
//logical content in a fresh owning record.
//Final-review I2: array records join the class writers' back-reference
//table (tag 2, shared object-id space) — a repeated reference emits the
//back-ref instead of a second copy, so cyclic graphs through array
//fields round-trip exactly like class cycles. The id keys the ORIGINAL
//record (pre-materialization): every reference to one view resolves to
//one materialized copy.
template<typename Writer, typename StreamState>
void VmExecutor::SerializeArrayRecord(int32_t heapIdx,
    Writer&& write, StreamState& st, int depth)
{
    if (depth >= static_cast<int>(STRUCT_SERIALIZE_DEPTH_LIMIT))
        throw std::runtime_error("NLang VM: struct serialize depth limit exceeded");
    if (heapIdx <= 0) {
        uint8_t tag = 0;   //null array reference — same marker as classes
        write(&tag, 1);
        return;
    }
    if (static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: invalid heap index in array record write");
    //Register BEFORE the payload walk — an element or nested field may
    //reference this very record (the cycle case).
    auto it = st.serializeObjIds.find(heapIdx);
    if (it != st.serializeObjIds.end()) {
        uint8_t tag = 2;
        write(&tag, 1);
        uint32_t id = it->second;
        write(reinterpret_cast<const uint8_t*>(&id), 4);
        return;
    }
    st.serializeObjIds[heapIdx] = st.nextObjId++;

    //Materialize before taking any record reference — the allocation
    //below may grow the heap and move the records.
    if (m_slotKinds[static_cast<size_t>(heapIdx)] == RTK_ArrayView)
        heapIdx = MaterializeViewRecord(heapIdx);

    const auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    const uint16_t arrayTypeIdx = static_cast<uint16_t>(slot[1]);
    if (arrayTypeIdx >= m_currModule->arrayTypes.size())
        throw std::runtime_error("NLang VM: invalid array type index in array record write");
    const auto& at = m_currModule->arrayTypes[arrayTypeIdx];
    WriteArrayHeader(at, slot, std::forward<Writer>(write));
    //No write-side allocation past this point, so the slot reference
    //stays valid through the payload walk.
    WriteArrayElements(at, slot, std::forward<Writer>(write), st,
                       depth + 1);
}

//Self-describing head: tag, elemKind, the struct element's table key,
//rank, dims. Everything the reader needs to type and shape the record.
template<typename Writer>
void VmExecutor::WriteArrayHeader(const CompiledArrayType& at,
    const std::vector<int32_t>& slot, Writer&& write)
{
    uint8_t tag = 3;
    write(&tag, 1);
    const uint8_t elemKind = at.elemKind;
    write(&elemKind, 1);
    if (elemKind == RTK_Struct) {
        if (at.elemTypeIdx == 0xFFFF
            || at.elemTypeIdx >= m_currModule->structs.size())
            throw std::runtime_error(
                "NLang VM: unresolved struct element type in array record write");
        const std::string& name =
            m_currModule->structs[at.elemTypeIdx].name;
        uint32_t nameLen = static_cast<uint32_t>(name.size());
        write(reinterpret_cast<const uint8_t*>(&nameLen), 4);
        write(reinterpret_cast<const uint8_t*>(name.data()), nameLen);
    }
    const uint8_t rank = static_cast<uint8_t>(slot[2]);
    write(&rank, 1);
    for (uint8_t d = 0; d < rank; ++d)
        write(reinterpret_cast<const uint8_t*>(&slot[4 + d]), 4);
}

//Payload: elements in row-major order. Scalar kinds emit their
//registry width from the carrier cells; strings are length-prefixed;
//struct elements recurse through the existing struct writer.
template<typename Writer, typename StreamState>
void VmExecutor::WriteArrayElements(const CompiledArrayType& at,
    const std::vector<int32_t>& slot, Writer&& write, StreamState& st,
    int depth)
{
    const uint8_t elemKind = at.elemKind;
    const int32_t elemCount = slot[3];
    const size_t base = ArrayPayloadBase(slot);
    int primRow = ScalarPrimIndexOfRtk(elemKind);
    if (primRow >= 0) {
        const int cells = ArrayElemCells(elemKind);
        const size_t width = kScalarPrims[primRow].slotWidth;
        for (int32_t i = 0; i < elemCount; ++i) {
            uint8_t bytes[8];
            std::memcpy(bytes, &slot[base + static_cast<size_t>(i) * cells],
                        width);
            write(bytes, width);
        }
    } else if (elemKind == RTK_String) {
        for (int32_t i = 0; i < elemCount; ++i) {
            const std::string& s =
                StrVal(slot[base + static_cast<size_t>(i)]);
            int32_t len = static_cast<int32_t>(s.size());
            uint8_t lenBytes[4];
            std::memcpy(lenBytes, &len, 4);
            write(lenBytes, 4);
            write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        }
    } else if (elemKind == RTK_Struct) {
        for (int32_t i = 0; i < elemCount; ++i)
            SerializeStructFields(slot[base + static_cast<size_t>(i)],
                at.elemTypeIdx, std::forward<Writer>(write), st, depth);
    } else {
        //Class/func element kinds have no wire form — loud failure, the
        //func-field precedent (a silent skip desynchronizes the stream).
        throw std::runtime_error(
            "NLang VM: WriteObject does not support array element kind ("
            + std::to_string(elemKind) + ")");
    }
}

//Reads what SerializeArrayRecord wrote and returns a freshly-allocated
//OWNING record (never a view). Tag 0 reads as null (0); tag 2 resolves
//a previously-read record (the shared back-ref table). The arrayTypes
//row is resolved from the stream's own (elemKind, element type, rank)
//— a shape the program never declared fails loudly, not with a
//mis-typed record.
template<typename Reader, typename StreamState>
int32_t VmExecutor::DeserializeArrayRecord(Reader&& read, StreamState& st,
    int depth)
{
    uint8_t tag;
    read(&tag, 1);
    return DeserializeArrayRecordTagged(tag, std::forward<Reader>(read),
                                        st, depth);
}

//The tag-dispatch tail: entry above reads the tag; class-field reader
//positions that already peeked it enter here directly.
template<typename Reader, typename StreamState>
int32_t VmExecutor::DeserializeArrayRecordTagged(uint8_t tag,
    Reader&& read, StreamState& st, int depth)
{
    if (depth >= static_cast<int>(STRUCT_SERIALIZE_DEPTH_LIMIT))
        throw std::runtime_error("NLang VM: struct serialize depth limit exceeded");
    if (tag == 0)
        return 0;
    if (tag == 2) {
        uint32_t id;
        read(reinterpret_cast<uint8_t*>(&id), 4);
        auto it = st.deserializeObjIds.find(id);
        if (it == st.deserializeObjIds.end())
            throw std::runtime_error(
                "NLang VM: ReadArray back-reference to unknown object id");
        return it->second;
    }
    if (tag != 3)
        throw std::runtime_error("NLang VM: ReadArray unknown array tag");

    uint8_t elemKind = RTK_Int32;
    uint16_t elemTypeIdx = 0xFFFF;
    uint8_t rank = 0;
    int32_t dims[kMaxArrayRank] = {0};
    ReadArrayHeader(std::forward<Reader>(read), elemKind, elemTypeIdx,
                    rank, dims);
    const uint16_t arrayTypeIdx =
        FindArrayTypeDef(elemKind, elemTypeIdx, rank);
    //AllocArrayOnHeap pre-allocates struct elements; the payload walk
    //below fills them (struct) or overwrites the scalar cells.
    const int32_t heapIdx = AllocArrayOnHeap(arrayTypeIdx, rank, dims);
    //Register BEFORE the payload walk — an element or nested field may
    //back-reference this record (mirrors the writer's registration).
    st.deserializeObjIds[st.nextObjId++] = heapIdx;
    //Value snapshot: nested struct reads may grow the heap and move
    //the record; elemCount is data, immune to the move.
    const int32_t elemCount =
        m_structHeap[static_cast<size_t>(heapIdx)][3];
    ReadArrayElements(heapIdx, elemKind, elemTypeIdx, elemCount,
        std::forward<Reader>(read), st, depth + 1);
    return heapIdx;
}

//Header parse: elemKind, the struct element's table key resolved to a
//row, rank, dims. Raises on out-of-range shapes and unknown names.
template<typename Reader>
void VmExecutor::ReadArrayHeader(Reader&& read, uint8_t& elemKindOut,
    uint16_t& elemTypeIdxOut, uint8_t& rankOut, int32_t* dimsOut)
{
    read(&elemKindOut, 1);
    if (elemKindOut == RTK_Struct) {
        uint32_t nameLen;
        read(reinterpret_cast<uint8_t*>(&nameLen), 4);
        if (nameLen > MAX_STRING_LENGTH)
            throw std::runtime_error(
                "NLang VM: ReadArray struct name length exceeds cap");
        std::string name(static_cast<size_t>(nameLen), '\0');
        if (nameLen > 0)
            read(reinterpret_cast<uint8_t*>(&name[0]), nameLen);
        int sIdx = m_currModule->FindStruct(name);
        if (sIdx < 0)
            throw std::runtime_error(
                "NLang VM: ReadArray struct not found: " + name);
        elemTypeIdxOut = static_cast<uint16_t>(sIdx);
    }
    read(&rankOut, 1);
    if (rankOut < 1 || rankOut > kMaxArrayRank)
        throw std::runtime_error("NLang VM: ReadArray rank out of range");
    for (uint8_t d = 0; d < rankOut; ++d)
        read(reinterpret_cast<uint8_t*>(&dimsOut[d]), 4);
}

//Payload walk into the allocated record. Fresh slot lookup per
//element: nested struct reads may grow m_structHeap and move the
//records (the struct-field reader's discipline).
template<typename Reader, typename StreamState>
void VmExecutor::ReadArrayElements(int32_t heapIdx, uint8_t elemKind,
    uint16_t elemTypeIdx, int32_t elemCount, Reader&& read,
    StreamState& st, int depth)
{
    int primRow = ScalarPrimIndexOfRtk(elemKind);
    const size_t base = ArrayHeaderCells(
        static_cast<uint8_t>(m_structHeap[static_cast<size_t>(heapIdx)][2]));
    if (primRow >= 0) {
        const int cells = ArrayElemCells(elemKind);
        const size_t width = kScalarPrims[primRow].slotWidth;
        for (int32_t i = 0; i < elemCount; ++i) {
            uint8_t bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            read(bytes, width);
            std::memcpy(&m_structHeap[static_cast<size_t>(heapIdx)]
                        [base + static_cast<size_t>(i) * cells], bytes,
                        width);
        }
    } else if (elemKind == RTK_String) {
        for (int32_t i = 0; i < elemCount; ++i) {
            uint8_t lenBytes[4];
            read(lenBytes, 4);
            int32_t len;
            std::memcpy(&len, lenBytes, 4);
            if (len < 0 || static_cast<size_t>(len) > MAX_STRING_LENGTH)
                throw std::runtime_error(
                    "NLang VM: ReadArray string length out of range");
            std::string s(static_cast<size_t>(len), '\0');
            if (len > 0)
                read(reinterpret_cast<uint8_t*>(&s[0]),
                     static_cast<size_t>(len));
            m_structHeap[static_cast<size_t>(heapIdx)]
                [base + static_cast<size_t>(i)] =
                    MintNewString(std::move(s));
        }
    } else if (elemKind == RTK_Struct) {
        for (int32_t i = 0; i < elemCount; ++i) {
            const int32_t elemIdx =
                m_structHeap[static_cast<size_t>(heapIdx)]
                    [base + static_cast<size_t>(i)];
            DeserializeStructFields(elemIdx, elemTypeIdx,
                std::forward<Reader>(read), st, depth);
        }
    } else {
        //No writer emits this (the write side throws first) — a
        //corrupted or hand-crafted stream.
        throw std::runtime_error(
            "NLang VM: ReadArray does not support array element kind ("
            + std::to_string(elemKind) + ")");
    }
}

//--- boxed values in Object-typed positions (final-review C4) ----------
//Tag 4: a boxed record reaching a class-declared reference slot — any
//primitive or a string wrapped by OP_Box. Payload after the tag: the
//wrapped kind byte, then a length-prefixed string or a scalar at the
//registry width (8-byte rows span the box's two payload cells — the
//AllocBoxedValue layout). Boxes are immutable values: no
//back-reference, each read mints a fresh box.
template<typename Writer>
void VmExecutor::WriteBoxedRecord(int32_t heapIdx, Writer&& write)
{
    const auto& box = m_structHeap[static_cast<size_t>(heapIdx)];
    uint8_t tag = 4;
    write(&tag, 1);
    const uint8_t inner = static_cast<uint8_t>(box[0]);
    write(&inner, 1);
    if (inner == RTK_String) {
        const std::string& s = StrVal(box[1]);
        int32_t len = static_cast<int32_t>(s.size());
        write(reinterpret_cast<const uint8_t*>(&len), 4);
        write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        return;
    }
    const int primRow = ScalarPrimIndexOfRtk(inner);
    if (primRow < 0)
        throw std::runtime_error(
            "NLang VM: WriteObject does not support boxed kind ("
            + std::to_string(inner) + ")");
    const size_t width = kScalarPrims[primRow].slotWidth;
    if (width == 8) {
        const uint64_t bits =
            static_cast<uint32_t>(box[1])
            | (static_cast<uint64_t>(static_cast<uint32_t>(box[2]))
               << 32);
        write(reinterpret_cast<const uint8_t*>(&bits), 8);
    } else {
        write(reinterpret_cast<const uint8_t*>(&box[1]), width);
    }
}

//Reads what WriteBoxedRecord wrote; returns the fresh box's heap idx.
template<typename Reader>
int32_t VmExecutor::ReadBoxedRecord(Reader&& read)
{
    uint8_t inner = RTK_Int32;
    read(&inner, 1);
    if (inner == RTK_String) {
        uint8_t lenBytes[4];
        read(lenBytes, 4);
        int32_t len;
        std::memcpy(&len, lenBytes, 4);
        if (len < 0 || static_cast<size_t>(len) > MAX_STRING_LENGTH)
            throw std::runtime_error(
                "NLang VM: ReadObject boxed string length out of range");
        std::string s(static_cast<size_t>(len), '\0');
        if (len > 0)
            read(reinterpret_cast<uint8_t*>(&s[0]),
                 static_cast<size_t>(len));
        return AllocBoxedValue(RTK_String,
            MintNewString(std::move(s)));
    }
    const int primRow = ScalarPrimIndexOfRtk(inner);
    if (primRow < 0)
        throw std::runtime_error(
            "NLang VM: ReadObject does not support boxed kind ("
            + std::to_string(inner) + ")");
    uint64_t bits = 0;
    read(reinterpret_cast<uint8_t*>(&bits),
         kScalarPrims[primRow].slotWidth);
    return AllocBoxedValue(inner, static_cast<int64_t>(bits));
}

} // namespace nlang
