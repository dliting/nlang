/*---
    VmExecutorSer.h — struct/class 序列化模板（唯一定义，内部头文件）。
    从 VmExecutor.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
    模板以调用点局部 lambda 为类型参数，实例化只能发生在消费方 TU 内，
    故定义放头文件：VmExecutorIntrinsics.cpp（及后续流域 TU）include 本文件。
---*/
#pragma once
#include "VmExecutor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstring>

namespace nlang {

//Len-prefixed string body shared by every string arm (struct fields,
//class fields, T-dispatched type-argument fields): i32 length, then the
//bytes; the read side validates against the string cap.
template<typename Writer>
void VmExecutor::WriteStringBody(const std::string& s, Writer&& write)
{
    int32_t len = static_cast<int32_t>(s.size());
    uint8_t lenBytes[4];
    std::memcpy(lenBytes, &len, 4);
    write(lenBytes, 4);
    write(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

template<typename Reader>
int32_t VmExecutor::ReadStringBody(Reader&& read)
{
    uint8_t lenBytes[4];
    read(lenBytes, 4);
    int32_t len;
    std::memcpy(&len, lenBytes, 4);
    if (len < 0 || static_cast<size_t>(len) > MAX_STRING_LENGTH)
        throw std::runtime_error(
            "NLang VM: ReadStruct string length out of range ("
            + std::to_string(len) + ")");
    std::string s(static_cast<size_t>(len), '\0');
    if (len > 0)
        read(reinterpret_cast<uint8_t*>(&s[0]), static_cast<size_t>(len));
    return MintNewString(std::move(s));
}

//Phase 8b struct serialization helpers.
//SerializeStructFields: walks a struct's fields and emits bytes via the Writer.
//Class fields recurse via SerializeClassFields (Phase 8c); array fields
//recurse via SerializeArrayRecord (v0.12.7 Task 13). Depth limit prevents
//pathological cycles (structs are value types, but a struct array's
//elements can point back at the array — the back-ref table resolves
//those).
template<typename Writer, typename StreamState>
void VmExecutor::SerializeStructFields(int32_t heapIdx, uint16_t structIdx,
    Writer&& write, StreamState& st, int depth)
{
    if (depth >= static_cast<int>(STRUCT_SERIALIZE_DEPTH_LIMIT))
        throw std::runtime_error("NLang VM: struct serialize depth limit exceeded");
    if (structIdx >= m_currModule->structs.size())
        throw std::runtime_error("NLang VM: invalid struct index in SerializeStructFields");
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: invalid struct heap index in SerializeStructFields");
    const auto& cs = m_currModule->structs[structIdx];
    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    for (uint16_t i = 0; i < cs.fieldCount; ++i)
    {
        uint16_t ftk = cs.fieldTypeKinds[i];
        int primRow = ScalarPrimIndexOfRtk(static_cast<uint8_t>(ftk));
        if (primRow >= 0)
        {
            //0.7.5: registry-driven scalar fields — every primitive row
            //(byte..ulong, bool/char) serializes at its own width; the
            //value is value-extended in the field's low cell(s), so
            //8-byte rows span cells [i*2, i*2+1].
            uint8_t bytes[8];
            std::memcpy(bytes, &slot[i * 2],
                kScalarPrims[primRow].slotWidth);
            write(bytes, kScalarPrims[primRow].slotWidth);
        }
        else if (ftk == RTK_String)
        {
            WriteStringBody(StrVal(slot[i * 2]),
                std::forward<Writer>(write));
        }
        else if (ftk == RTK_Struct)
        {
            if (cs.fieldStructIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested struct type");
            int32_t innerHeapIdx = slot[i * 2];
            SerializeStructFields(innerHeapIdx, cs.fieldStructIndices[i],
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_Class)
        {
            if (cs.fieldClassIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested class type");
            int32_t childHeapIdx = slot[i * 2];
            SerializeClassFields(childHeapIdx,
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_Array)
        {
            SerializeArrayRecord(slot[i * 2],
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_Func)
        {
            //Phase 13: handles reference module functions/objects and
            //are not serializable bytes.
            throw std::runtime_error(
                "NLang VM: WriteStruct does not support func fields");
        }
        else
        {
            //Unrecognized kinds must fail loudly, not skip: a silent
            //skip desynchronizes writer and reader (0.7.5 stage-2
            //review finding — this arm used to be absent).
            throw std::runtime_error(
                "NLang VM: WriteStruct unsupported field kind ("
                + std::to_string(ftk) + ")");
        }
    }
}

template<typename Reader, typename StreamState>
void VmExecutor::DeserializeStructFields(int32_t heapIdx, uint16_t structIdx,
    Reader&& read, StreamState& st, int depth)
{
    if (depth >= static_cast<int>(STRUCT_SERIALIZE_DEPTH_LIMIT))
        throw std::runtime_error("NLang VM: struct serialize depth limit exceeded");
    if (structIdx >= m_currModule->structs.size())
        throw std::runtime_error("NLang VM: invalid struct index in DeserializeStructFields");
    if (heapIdx <= 0 || static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: invalid struct heap index in DeserializeStructFields");
    const auto& cs = m_currModule->structs[structIdx];
    size_t heapIdxSz = static_cast<size_t>(heapIdx);
    for (uint16_t i = 0; i < cs.fieldCount; ++i)
    {
        uint16_t ftk = cs.fieldTypeKinds[i];
        int primRow = ScalarPrimIndexOfRtk(static_cast<uint8_t>(ftk));
        if (primRow >= 0)
        {
            //0.7.5: registry-driven scalar read at the row's own width;
            //4-byte rows fill exactly the low cell, 8-byte rows span
            //cells [i*2, i*2+1] (mirrors the write side).
            uint8_t bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            read(bytes, kScalarPrims[primRow].slotWidth);
            std::memcpy(&m_structHeap[heapIdxSz][i * 2], bytes,
                kScalarPrims[primRow].slotWidth);
        }
        else if (ftk == RTK_String)
        {
            m_structHeap[heapIdxSz][i * 2] =
                ReadStringBody(std::forward<Reader>(read));
        }
        else if (ftk == RTK_Struct)
        {
            if (cs.fieldStructIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested struct type");
            int32_t innerHeapIdx = AllocStructOnHeap(cs.fieldStructIndices[i]);
            m_structHeap[heapIdxSz][i * 2] = innerHeapIdx;
            DeserializeStructFields(innerHeapIdx, cs.fieldStructIndices[i],
                std::forward<Reader>(read), st, depth + 1);
        }
        else if (ftk == RTK_Class)
        {
            if (cs.fieldClassIndices[i] == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested class type");
            int32_t childHeapIdx = 0;
            DeserializeClassFields(cs.fieldClassIndices[i], childHeapIdx,
                std::forward<Reader>(read), st, depth + 1);
            m_structHeap[heapIdxSz][i * 2] = childHeapIdx;
        }
        else if (ftk == RTK_Array)
        {
            m_structHeap[heapIdxSz][i * 2] = DeserializeArrayRecord(
                std::forward<Reader>(read), st, depth + 1);
        }
        else if (ftk == RTK_Func)
        {
            //Phase 13: no writer can emit a func field (the write side
            //throws), so reaching this arm means a corrupted stream.
            throw std::runtime_error(
                "NLang VM: ReadStruct does not support func fields");
        }
        else
        {
            //No writer emits an unrecognized kind (the write side
            //throws), so this means a corrupted or hand-crafted stream.
            throw std::runtime_error(
                "NLang VM: ReadStruct unsupported field kind ("
                + std::to_string(ftk) + ")");
        }
    }
}

//Phase 8c class serialization helpers.
//SerializeClassFields: emits a tagged record for a class-typed reference.
//tag=0: null (heapIdx <= 0). tag=1: new object (class name + field payload),
//recorded in st.serializeObjIds for later back-references. tag=2: back-ref
//to a previously-emitted object id (class objects and array records share
//the id space). tag=3: array record (owned or view), tag=4: boxed value —
//an Object-typed position may hold either at runtime (final-review C4;
//routing below). Per-kind field switch mirrors SerializeStructFields but
//reads from cc.fieldTypeKinds[i] / slot[i*2+1] (class header occupies
//cell[0]; field i lives at cell 1+2i).
template<typename Writer, typename StreamState>
void VmExecutor::SerializeClassFields(int32_t heapIdx,
    Writer&& write, StreamState& st, int depth)
{
    if (depth >= static_cast<int>(STRUCT_SERIALIZE_DEPTH_LIMIT))
        throw std::runtime_error("NLang VM: struct serialize depth limit exceeded");

    if (heapIdx <= 0) {
        uint8_t tag = 0;
        write(&tag, 1);
        return;
    }
    if (static_cast<size_t>(heapIdx) >= m_structHeap.size())
        throw std::runtime_error("NLang VM: invalid class heap index in SerializeClassFields");

    //Runtime-kind routing (final-review C4): a class-declared reference
    //field may hold any Object value. Arrays take the array record form
    //(own back-ref table entry inside), boxed records (every primitive
    //and string) the boxed form; a func value has no wire form (the
    //func-field precedent — a silent skip desynchronizes the stream).
    const uint8_t runtimeKind = m_slotKinds[static_cast<size_t>(heapIdx)];
    if (runtimeKind == RTK_Array || runtimeKind == RTK_ArrayView) {
        SerializeArrayRecord(heapIdx, std::forward<Writer>(write),
                             st, depth);
        return;
    }
    if (runtimeKind == RTK_Boxed) {
        WriteBoxedRecord(heapIdx, std::forward<Writer>(write));
        return;
    }
    if (runtimeKind == RTK_Func)
        throw std::runtime_error(
            "NLang VM: WriteObject does not support func values");
    if (runtimeKind != RTK_Class)
        throw std::runtime_error(
            "NLang VM: WriteObject unsupported runtime kind ("
            + std::to_string(runtimeKind) + ")");

    auto it = st.serializeObjIds.find(heapIdx);
    if (it != st.serializeObjIds.end()) {
        uint8_t tag = 2;
        write(&tag, 1);
        uint32_t id = it->second;
        write(reinterpret_cast<const uint8_t*>(&id), 4);
        return;
    }

    uint32_t id = st.nextObjId++;
    st.serializeObjIds[heapIdx] = id;

    uint8_t tag = 1;
    write(&tag, 1);

    auto& slot = m_structHeap[static_cast<size_t>(heapIdx)];
    int32_t classIdx = slot[0];
    if (classIdx < 0 || static_cast<size_t>(classIdx) >= m_currModule->classes.size())
        throw std::runtime_error("NLang VM: invalid class index in class heap slot");
    const auto& cc = m_currModule->classes[static_cast<size_t>(classIdx)];

    //Write/read same-source (phase 5): the name in the stream IS the
    //table key. v0.13.1 D3: the head also carries the instantiation
    //descriptor as its canonical spelling (VmExecutorSerInstDesc.h).
    WriteClassRefHead(cc.name, InstDescriptorOf(heapIdx),
        std::forward<Writer>(write));

    //0.9.12 T1: the field surface comes from the layout view; the
    //stream key (cc.name) and subclass checks stay on the record.
    const auto& fields = m_classLayout.Fields(static_cast<size_t>(classIdx));
    for (size_t i = 0; i < fields.size(); ++i)
    {
        uint16_t ftk = fields[i].kind;
        int primRow = ScalarPrimIndexOfRtk(static_cast<uint8_t>(ftk));
        if (primRow >= 0)
        {
            //0.7.5: scalar fields at their own width (class header
            //occupies cell 0; field i spans cells [1+2i, 2+2i]).
            uint8_t bytes[8];
            std::memcpy(bytes, &slot[i * 2 + 1],
                kScalarPrims[primRow].slotWidth);
            write(bytes, kScalarPrims[primRow].slotWidth);
        }
        else if (ftk == RTK_String)
        {
            WriteStringBody(StrVal(slot[i * 2 + 1]),
                std::forward<Writer>(write));
        }
        else if (ftk == RTK_Struct)
        {
            if (fields[i].structIdx == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested struct type");
            int32_t innerHeapIdx = slot[i * 2 + 1];
            SerializeStructFields(innerHeapIdx, fields[i].structIdx,
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_Class)
        {
            if (fields[i].classIdx == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested class type");
            int32_t childHeapIdx = slot[i * 2 + 1];
            SerializeClassFields(childHeapIdx,
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_Array)
        {
            SerializeArrayRecord(slot[i * 2 + 1],
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_TypeParam)
        {
            //v0.13.1 D3: the T field dispatches through the receiver's
            //instantiation argument (ordinal from the class-row slot).
            SerializeTypeArgField(InstDescriptorOf(heapIdx),
                fields[i].classIdx, slot, i * 2 + 1,
                std::forward<Writer>(write), st, depth + 1);
        }
        else if (ftk == RTK_Func)
        {
            //Phase 13: handles reference module functions/objects and
            //are not serializable bytes.
            throw std::runtime_error(
                "NLang VM: WriteStruct does not support func fields");
        }
        else
        {
            //Unrecognized kinds must fail loudly, not skip: a silent
            //skip desynchronizes writer and reader (0.7.5 stage-2
            //review finding — this arm used to be absent).
            throw std::runtime_error(
                "NLang VM: WriteStruct unsupported field kind ("
                + std::to_string(ftk) + ")");
        }
    }
}

//DeserializeClassFields: reads a tagged record written by SerializeClassFields
//and produces a freshly-allocated class object (or back-reference). On tag=1,
//the object is registered in st.deserializeObjIds before fields are decoded
//so cycles back to this object resolve correctly. Per-kind field switch
//mirrors DeserializeStructFields but writes to slot[i+1].
template<typename Reader, typename StreamState>
void VmExecutor::DeserializeClassFields(uint16_t declaredClassIdx,
    int32_t& outHeapIdx, Reader&& read, StreamState& st, int depth)
{
    if (depth >= static_cast<int>(STRUCT_SERIALIZE_DEPTH_LIMIT))
        throw std::runtime_error("NLang VM: struct serialize depth limit exceeded");

    uint8_t tag;
    read(&tag, 1);

    if (tag == 0) {
        outHeapIdx = 0;
        return;
    }
    //A class-declared field position may carry an array (tag 3) or a
    //boxed value (tag 4) — the reader side of SerializeClassFields's
    //runtime-kind routing (final-review C4). Object is the only class
    //type these values convert to (§4.10.7, §4.11.1), so a value-form
    //tag under any other declared class means a hand-crafted stream —
    //storing it would plant an array record in a typed slot (the loud
    //failure every other malformed case takes).
    const bool objTypedField =
        static_cast<int32_t>(declaredClassIdx) == ObjectRootClassIdx();
    if (tag == 3) {
        if (!objTypedField)
            throw std::runtime_error(
                "NLang VM: ReadObject array record under a class-typed field");
        outHeapIdx = DeserializeArrayRecordTagged(tag,
            std::forward<Reader>(read), st, depth + 1);
        return;
    }
    if (tag == 4) {
        if (!objTypedField)
            throw std::runtime_error(
                "NLang VM: ReadObject boxed value under a class-typed field");
        outHeapIdx = ReadBoxedRecord(std::forward<Reader>(read));
        return;
    }
    if (tag == 2) {
        uint32_t id;
        read(reinterpret_cast<uint8_t*>(&id), 4);
        auto it = st.deserializeObjIds.find(id);
        if (it == st.deserializeObjIds.end())
            throw std::runtime_error(
                "NLang VM: ReadStruct back-reference to unknown object id");
        outHeapIdx = it->second;
        return;
    }
    if (tag != 1)
        throw std::runtime_error("NLang VM: ReadStruct unknown class ref tag");

    std::string className;
    GenericInstDescriptor* instDesc = nullptr;
    ReadClassRefHead(read, className, instDesc, *m_currModule);

    //Consumes the string the writer stored: same compilation, same key.
    int classIdx = m_currModule->FindClass(className);
    if (classIdx < 0)
        throw std::runtime_error(
            "NLang VM: class not found in stream: " + className);
    if (!IsSubclassOf(static_cast<uint16_t>(classIdx), declaredClassIdx)) {
        const std::string& declaredName =
            m_currModule->classes[declaredClassIdx].name;
        throw std::runtime_error(
            "NLang VM: class type mismatch: expected "
            + declaredName + ", got " + className);
    }

    //The descriptor (when carried) stamps the fresh object before the
    //field walk — the T-field arm dispatches through it.
    int32_t heapIdx = AllocClassOnHeap(static_cast<uint16_t>(classIdx),
        instDesc);
    st.deserializeObjIds[st.nextObjId++] = heapIdx;

    //0.9.12 T1: the field surface comes from the layout view (mirrors
    //the record today; the record binding is no longer needed here).
    const auto& fields = m_classLayout.Fields(static_cast<size_t>(classIdx));
    size_t heapIdxSz = static_cast<size_t>(heapIdx);
    for (size_t i = 0; i < fields.size(); ++i)
    {
        uint16_t ftk = fields[i].kind;
        int primRow = ScalarPrimIndexOfRtk(static_cast<uint8_t>(ftk));
        if (primRow >= 0)
        {
            //0.7.5: registry-driven scalar read at the row's own width
            //(mirrors the write side; field i writes cells [1+2i, 2+2i]).
            uint8_t bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            read(bytes, kScalarPrims[primRow].slotWidth);
            std::memcpy(&m_structHeap[heapIdxSz][i * 2 + 1], bytes,
                kScalarPrims[primRow].slotWidth);
        }
        else if (ftk == RTK_String)
        {
            m_structHeap[heapIdxSz][i * 2 + 1] =
                ReadStringBody(std::forward<Reader>(read));
        }
        else if (ftk == RTK_Struct)
        {
            if (fields[i].structIdx == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested struct type");
            int32_t innerHeapIdx = AllocStructOnHeap(fields[i].structIdx);
            m_structHeap[heapIdxSz][i * 2 + 1] = innerHeapIdx;
            DeserializeStructFields(innerHeapIdx, fields[i].structIdx,
                std::forward<Reader>(read), st, depth + 1);
        }
        else if (ftk == RTK_Class)
        {
            if (fields[i].classIdx == 0xFFFF)
                throw std::runtime_error("NLang VM: unresolved nested class type");
            int32_t childHeapIdx = 0;
            DeserializeClassFields(fields[i].classIdx, childHeapIdx,
                std::forward<Reader>(read), st, depth + 1);
            m_structHeap[heapIdxSz][i * 2 + 1] = childHeapIdx;
        }
        else if (ftk == RTK_Array)
        {
            m_structHeap[heapIdxSz][i * 2 + 1] = DeserializeArrayRecord(
                std::forward<Reader>(read), st, depth + 1);
        }
        else if (ftk == RTK_TypeParam)
        {
            //v0.13.1 D3: the descriptor stamped above dispatches the
            //T field through the receiver's instantiation argument.
            DeserializeTypeArgField(heapIdx, fields[i].classIdx,
                i * 2 + 1, read, st, depth + 1);
        }
        else if (ftk == RTK_Func)
        {
            //Phase 13: no writer can emit a func field (the write side
            //throws), so reaching this arm means a corrupted stream.
            throw std::runtime_error(
                "NLang VM: ReadStruct does not support func fields");
        }
        else
        {
            //No writer emits an unrecognized kind (the write side
            //throws), so this means a corrupted or hand-crafted stream.
            throw std::runtime_error(
                "NLang VM: ReadStruct unsupported field kind ("
                + std::to_string(ftk) + ")");
        }
    }

    outHeapIdx = heapIdx;
}

} // namespace nlang

//Array-content templates (Task 13) — included here, after the
//struct/class definitions they build on, so the existing Ser consumers
//need no include change.
#include "VmExecutorSerArray.h"

//Instantiation-aware bits (v0.13.1 D3) — included after the array
//templates for the same reason: the T-field dispatch recurses into
//struct/class/array serialization.
#include "VmExecutorSerInstDesc.h"
