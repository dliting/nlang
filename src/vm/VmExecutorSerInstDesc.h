/*---
    VmExecutorSerInstDesc.h — instantiation-aware serialization bits
    (v0.13.1 D3): the class-ref head (table key + descriptor spelling)
    and the T-field dispatch through the receiver's descriptor. Split
    from VmExecutorSer.h for the file-size guard; same header-template
    shape — definitions live here because instantiation happens in the
    consuming TUs; include VmExecutorSer.h (which includes this file
    after the array templates) rather than this one directly.
---*/
#pragma once
#include "VmExecutor.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <cstring>

namespace nlang {

//The class-ref head: the stream key (the erased class-row name) plus
//the instantiation descriptor as its canonical spelling. The flag byte
//keeps non-instantiation objects at one byte; pre-0.13.1 streams carry
//no flag and only same-version streams interconvert (no backward
//compatibility). The spelling is the intern key — the read side
//rebuilds the argument tree through InternDescriptorFromSpelling so
//kind dispatch (casts, typeName, equality, T fields) survives the
//round trip.
template<typename Writer>
void VmExecutor::WriteClassRefHead(const std::string& className,
    const GenericInstDescriptor* instDesc, Writer&& write)
{
    uint32_t nameLen = static_cast<uint32_t>(className.size());
    write(reinterpret_cast<const uint8_t*>(&nameLen), 4);
    write(reinterpret_cast<const uint8_t*>(className.data()), nameLen);
    uint8_t descFlag = instDesc ? 1 : 0;
    write(&descFlag, 1);
    if (!instDesc)
        return;
    const std::string& spelling = instDesc->Spelling();
    uint32_t spellingLen = static_cast<uint32_t>(spelling.size());
    write(reinterpret_cast<const uint8_t*>(&spellingLen), 4);
    write(reinterpret_cast<const uint8_t*>(spelling.data()), spellingLen);
}

template<typename Reader>
void VmExecutor::ReadClassRefHead(Reader&& read, std::string& classNameOut,
    GenericInstDescriptor*& instDescOut, const CompiledModule& module)
{
    uint32_t nameLen;
    read(reinterpret_cast<uint8_t*>(&nameLen), 4);
    if (static_cast<size_t>(nameLen) > MAX_STRING_LENGTH)
        throw std::runtime_error(
            "NLang VM: ReadStruct class name length exceeds cap ("
            + std::to_string(nameLen) + " > "
            + std::to_string(MAX_STRING_LENGTH) + ")");
    classNameOut.assign(nameLen, '\0');
    if (nameLen > 0)
        read(reinterpret_cast<uint8_t*>(&classNameOut[0]), nameLen);

    instDescOut = nullptr;
    uint8_t descFlag;
    read(&descFlag, 1);
    if (descFlag == 0)
        return;
    if (descFlag != 1)
        throw std::runtime_error(
            "NLang VM: ReadStruct unknown descriptor flag");
    uint32_t spellingLen;
    read(reinterpret_cast<uint8_t*>(&spellingLen), 4);
    if (static_cast<size_t>(spellingLen) > MAX_STRING_LENGTH)
        throw std::runtime_error(
            "NLang VM: ReadStruct descriptor spelling length exceeds cap ("
            + std::to_string(spellingLen) + " > "
            + std::to_string(MAX_STRING_LENGTH) + ")");
    std::string spelling(static_cast<size_t>(spellingLen), '\0');
    if (spellingLen > 0)
        read(reinterpret_cast<uint8_t*>(&spelling[0]), spellingLen);
    instDescOut = InternDescriptorFromSpelling(spelling, module);
    //A descriptor naming another template under this row is a corrupt
    //stream — the loud failure every malformed head takes.
    if (instDescOut->TemplateName() != classNameOut)
        throw std::runtime_error(
            "NLang VM: ReadStruct descriptor template mismatch: "
            + spelling + " under " + classNameOut);
}

//The T-field arms: a type-parameter field's runtime kind comes from the
//receiver's instantiation argument (the ordinal rides the class-row
//class-index slot; the erased row cannot know it). The value
//representation is the argument kind's own — inline scalar cells, a
//string handle, struct/class/array heap indices — the same conservative
//domains the GC root scan routes for RTK_TypeParam slots.
template<typename Writer, typename StreamState>
void VmExecutor::SerializeTypeArgField(const GenericInstDescriptor* instDesc,
    uint16_t ordinal, const std::vector<int32_t>& slot, size_t cellOffset,
    Writer&& write, StreamState& st, int depth)
{
    if (!instDesc || ordinal >= instDesc->TypeArgs().size())
        throw std::runtime_error(
            "NLang VM: WriteObject T field without its instantiation argument");
    const TypeArgDesc& arg = instDesc->TypeArgs()[ordinal];
    int primRow = ScalarPrimIndexOfRtk(arg.resolvedKind);
    if (primRow >= 0)
    {
        uint8_t bytes[8];
        std::memcpy(bytes, &slot[cellOffset], kScalarPrims[primRow].slotWidth);
        write(bytes, kScalarPrims[primRow].slotWidth);
    }
    else if (arg.resolvedKind == RTK_String)
    {
        WriteStringBody(StrVal(slot[cellOffset]),
            std::forward<Writer>(write));
    }
    else if (arg.resolvedKind == RTK_Struct)
    {
        SerializeStructFields(slot[cellOffset],
            static_cast<uint16_t>(arg.rowRef),
            std::forward<Writer>(write), st, depth);
    }
    else if (arg.resolvedKind == RTK_Class)
    {
        //Recursion: the nested object writes its own head, descriptor
        //included, so nested instantiations carry their arguments too.
        SerializeClassFields(slot[cellOffset],
            std::forward<Writer>(write), st, depth);
    }
    else if (arg.resolvedKind == RTK_Array)
    {
        SerializeArrayRecord(slot[cellOffset],
            std::forward<Writer>(write), st, depth);
    }
    else
    {
        //The dictionary model binds concrete arguments only (scalars,
        //enum rows erased to int, string, struct, class, array) — any
        //other resolved kind means a corrupt descriptor.
        throw std::runtime_error(
            "NLang VM: WriteObject unsupported type-argument kind ("
            + std::to_string(arg.resolvedKind) + ")");
    }
}

template<typename Reader, typename StreamState>
void VmExecutor::DeserializeTypeArgField(int32_t heapIdx, uint16_t ordinal,
    size_t cellOffset, Reader&& read, StreamState& st, int depth)
{
    const auto* instDesc = InstDescriptorOf(heapIdx);
    if (!instDesc || ordinal >= instDesc->TypeArgs().size())
        throw std::runtime_error(
            "NLang VM: ReadStruct T field without its instantiation argument");
    const TypeArgDesc& arg = instDesc->TypeArgs()[ordinal];
    int primRow = ScalarPrimIndexOfRtk(arg.resolvedKind);
    if (primRow >= 0)
    {
        uint8_t bytes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        read(bytes, kScalarPrims[primRow].slotWidth);
        std::memcpy(&m_structHeap[static_cast<size_t>(heapIdx)][cellOffset],
            bytes, kScalarPrims[primRow].slotWidth);
    }
    else if (arg.resolvedKind == RTK_String)
    {
        m_structHeap[static_cast<size_t>(heapIdx)][cellOffset] =
            ReadStringBody(std::forward<Reader>(read));
    }
    else if (arg.resolvedKind == RTK_Struct)
    {
        int32_t innerHeapIdx = AllocStructOnHeap(
            static_cast<uint16_t>(arg.rowRef));
        m_structHeap[static_cast<size_t>(heapIdx)][cellOffset] = innerHeapIdx;
        DeserializeStructFields(innerHeapIdx,
            static_cast<uint16_t>(arg.rowRef),
            std::forward<Reader>(read), st, depth);
    }
    else if (arg.resolvedKind == RTK_Class)
    {
        int32_t childHeapIdx = 0;
        DeserializeClassFields(static_cast<uint16_t>(arg.rowRef),
            childHeapIdx, std::forward<Reader>(read), st, depth);
        m_structHeap[static_cast<size_t>(heapIdx)][cellOffset] = childHeapIdx;
    }
    else if (arg.resolvedKind == RTK_Array)
    {
        m_structHeap[static_cast<size_t>(heapIdx)][cellOffset] =
            DeserializeArrayRecord(std::forward<Reader>(read), st, depth);
    }
    else
    {
        throw std::runtime_error(
            "NLang VM: ReadStruct unsupported type-argument kind ("
            + std::to_string(arg.resolvedKind) + ")");
    }
}

} //namespace nlang
