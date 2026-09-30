/*---
    IntrinsicsByteStream.cpp — ByteStream 内建（内存字节流读写族：基元/字符串/struct/object 序列化）
    从 VmExecutorIntrinsics.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
    域委托与 math/io/string 族同构：域内 id 处理后返回 true，域外返回 false。
---*/
#include "VmExecutor.h"
#include "VmExecutorSer.h"
#include <cstring>
#include <cstdio>
#include <exception>

namespace nlang {


//Helper: read this.__handle from callParamBase[0].
//Returns the 1-based handle. Throws if invalid or closed.
static int32_t ReadStreamHandle(uint16_t callParamBase, uint8_t* locals,
    const std::vector<std::vector<int32_t>>& structHeap,
    const char* label)
{
    int32_t thisHeapIdx;
    std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
    if (thisHeapIdx <= 0 || static_cast<size_t>(thisHeapIdx) >= structHeap.size())
        throw std::runtime_error(std::string("NLang VM: ") + label + " on null reference");
    int32_t handle = structHeap[static_cast<size_t>(thisHeapIdx)][1]; //slot 1 = __handle
    if (handle <= 0)
        throw std::runtime_error("NLang VM: stream handle is invalid or closed");
    return handle;
}

bool VmExecutor::ExecuteIntrinsicByteStream(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    //ByteStream intrinsics (0-18): 0-10 primitives (4-byte), 11-12
    //struct, 13-14 object, 15-18 the 8-byte scalar quartet.
    if (intrinsicId <= INTR_BS_ReadDouble) {
        switch (intrinsicId) {
        case INTR_BS_Ctor: {
            //this is at callParamBase[0] (heapIdx). Allocate handle, store in __handle.
            int32_t thisHeapIdx;
            std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
            int32_t handle = AllocByteStreamHandle();
            m_structHeap[static_cast<size_t>(thisHeapIdx)][1] = handle;
            break;
        }
        case INTR_BS_WriteInt: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeInt");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t val;
            std::memcpy(&val, locals + callParamBase + kFrameSlotBytes, sizeof(val));
            uint8_t bytes[4];
            std::memcpy(bytes, &val, 4);
            st->buf.insert(st->buf.end(), bytes, bytes + 4);
            st->pos += 4;
            break;
        }
        case INTR_BS_ReadInt: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readInt");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (st->pos + 4 > st->buf.size())
                throw std::runtime_error("NLang VM: ReadInt past end of stream");
            int32_t val;
            std::memcpy(&val, st->buf.data() + st->pos, 4);
            st->pos += 4;
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_BS_WriteFloat: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeFloat");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            float val;
            std::memcpy(&val, locals + callParamBase + kFrameSlotBytes, sizeof(val));
            uint8_t bytes[4];
            std::memcpy(bytes, &val, 4);
            st->buf.insert(st->buf.end(), bytes, bytes + 4);
            st->pos += 4;
            break;
        }
        case INTR_BS_ReadFloat: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readFloat");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (st->pos + 4 > st->buf.size())
                throw std::runtime_error("NLang VM: ReadFloat past end of stream");
            float val;
            std::memcpy(&val, st->buf.data() + st->pos, 4);
            st->pos += 4;
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_BS_WriteLong: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeLong");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int64_t val;
            std::memcpy(&val, locals + callParamBase + kFrameSlotBytes, sizeof(val));
            uint8_t bytes[8];
            std::memcpy(bytes, &val, 8);
            st->buf.insert(st->buf.end(), bytes, bytes + 8);
            st->pos += 8;
            break;
        }
        case INTR_BS_ReadLong: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readLong");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (st->pos + 8 > st->buf.size())
                throw std::runtime_error("NLang VM: ReadLong past end of stream");
            int64_t val;
            std::memcpy(&val, st->buf.data() + st->pos, 8);
            st->pos += 8;
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_BS_WriteDouble: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeDouble");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            double val;
            std::memcpy(&val, locals + callParamBase + kFrameSlotBytes, sizeof(val));
            uint8_t bytes[8];
            std::memcpy(bytes, &val, 8);
            st->buf.insert(st->buf.end(), bytes, bytes + 8);
            st->pos += 8;
            break;
        }
        case INTR_BS_ReadDouble: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readDouble");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (st->pos + 8 > st->buf.size())
                throw std::runtime_error("NLang VM: ReadDouble past end of stream");
            double val;
            std::memcpy(&val, st->buf.data() + st->pos, 8);
            st->pos += 8;
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_BS_WriteString: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeString");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t strIdx;
            std::memcpy(&strIdx, locals + callParamBase + kFrameSlotBytes, sizeof(strIdx));
            const std::string& s = StrVal(strIdx);
            int32_t len = static_cast<int32_t>(s.size());
            uint8_t lenBytes[4];
            std::memcpy(lenBytes, &len, 4);
            st->buf.insert(st->buf.end(), lenBytes, lenBytes + 4);
            st->buf.insert(st->buf.end(), reinterpret_cast<const uint8_t*>(s.data()),
                           reinterpret_cast<const uint8_t*>(s.data()) + s.size());
            st->pos += 4 + static_cast<size_t>(len);
            break;
        }
        case INTR_BS_ReadString: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readString");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (st->pos + 4 > st->buf.size())
                throw std::runtime_error("NLang VM: ReadString length prefix past end of stream");
            int32_t len;
            std::memcpy(&len, st->buf.data() + st->pos, 4);
            st->pos += 4;
            if (len < 0)
                throw std::runtime_error("NLang VM: ReadString length negative (" + std::to_string(len) + ")");
            if (static_cast<size_t>(len) > MAX_STRING_LENGTH)
                throw std::runtime_error(
                    "NLang VM: ReadString length exceeds cap (" + std::to_string(len)
                    + " > " + std::to_string(MAX_STRING_LENGTH) + ")");
            if (st->pos + static_cast<size_t>(len) > st->buf.size())
                throw std::runtime_error("NLang VM: ReadString bytes past end of stream");
            std::string s(reinterpret_cast<const char*>(st->buf.data() + st->pos),
                          static_cast<size_t>(len));
            st->pos += static_cast<size_t>(len);
            int32_t strHandle = MintNewString(std::move(s));
            std::memcpy(pResult, &strHandle, sizeof(strHandle));
            break;
        }
        case INTR_BS_Length: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "length");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t len = static_cast<int32_t>(st->buf.size());
            std::memcpy(pResult, &len, sizeof(len));
            break;
        }
        case INTR_BS_Position: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "position");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t pos = static_cast<int32_t>(st->pos);
            std::memcpy(pResult, &pos, sizeof(pos));
            break;
        }
        case INTR_BS_Reset: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "reset");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            st->pos = 0;
            ClearObjIdState(*st);
            break;
        }
        case INTR_BS_WriteStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeStruct");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t structHeapIdx;
            std::memcpy(&structHeapIdx, locals + callParamBase + kFrameSlotBytes,
                sizeof(structHeapIdx));
            if (structHeapIdx <= 0
                || static_cast<size_t>(structHeapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: WriteStruct on null struct");
            uint16_t structIdx = m_slotStructIdx[static_cast<size_t>(structHeapIdx)];
            size_t sizeBefore = st->buf.size();
            //Writer lambda: append bytes to the ByteStream's buffer.
            SerializeStructFields(structHeapIdx, structIdx,
                [&](const uint8_t* p, size_t n) {
                    st->buf.insert(st->buf.end(), p, p + n);
                }, *st);
            st->pos += st->buf.size() - sizeBefore;
            break;
        }
        case INTR_BS_ReadStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readStruct");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t typeNameIdx;
            std::memcpy(&typeNameIdx, locals + callParamBase + kFrameSlotBytes,
                sizeof(typeNameIdx));
            const std::string& typeName = StrVal(typeNameIdx);
            int sIdx = m_currModule->FindStruct(typeName);
            if (sIdx < 0)
                throw std::runtime_error(
                    "NLang VM: ReadStruct type not found: " + typeName);
            uint16_t structIdx = static_cast<uint16_t>(sIdx);
            int32_t rootHeapIdx = AllocStructOnHeap(structIdx);
            //Reader lambda: copy from buffer, advance pos, throw on EOF.
            DeserializeStructFields(rootHeapIdx, structIdx,
                [&](uint8_t* dst, size_t n) {
                    if (st->pos + n > st->buf.size())
                        throw std::runtime_error(
                            "NLang VM: ReadStruct past end of stream");
                    std::memcpy(dst, st->buf.data() + st->pos, n);
                    st->pos += n;
                }, *st);
            std::memcpy(pResult, &rootHeapIdx, sizeof(rootHeapIdx));
            break;
        }
        case INTR_BS_WriteObject: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeObject");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t heapIdx;
            std::memcpy(&heapIdx, locals + callParamBase + kFrameSlotBytes,
                sizeof(heapIdx));
            if (heapIdx < 0
                || static_cast<size_t>(heapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: WriteObject invalid heap index");
            size_t sizeBefore = st->buf.size();
            SerializeClassFields(heapIdx,
                [&](const uint8_t* p, size_t n) {
                    st->buf.insert(st->buf.end(), p, p + n);
                }, *st, 0);
            st->pos += st->buf.size() - sizeBefore;
            break;
        }
        case INTR_BS_ReadObject: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readObject");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            if (st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t typeNameIdx;
            std::memcpy(&typeNameIdx, locals + callParamBase + kFrameSlotBytes,
                sizeof(typeNameIdx));
            const std::string& declaredName = StrVal(typeNameIdx);
            int declaredIdx = m_currModule->FindClass(declaredName);
            if (declaredIdx < 0)
                throw std::runtime_error(
                    "NLang VM: ReadObject class not found: " + declaredName);
            int32_t outHeapIdx = 0;
            DeserializeClassFields(static_cast<uint16_t>(declaredIdx),
                outHeapIdx,
                [&](uint8_t* dst, size_t n) {
                    if (st->pos + n > st->buf.size())
                        throw std::runtime_error(
                            "NLang VM: ReadObject past end of stream");
                    std::memcpy(dst, st->buf.data() + st->pos, n);
                    st->pos += n;
                }, *st, 0);
            std::memcpy(pResult, &outHeapIdx, sizeof(outHeapIdx));
            break;
        }
        case INTR_BS_Close: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "close");
            auto& st = m_byteStreams[static_cast<size_t>(handle) - 1];
            st->closed = true;
            st->buf.clear();
            st->pos = 0;
            ClearObjIdState(*st);
            //Release handle back to free list.
            size_t idx = static_cast<size_t>(handle) - 1;
            m_byteStreams[idx].reset();
            m_byteStreamFreeList.push_back(handle);
            //Zero the __handle field so subsequent calls fail.
            int32_t thisHeapIdx;
            std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
            m_structHeap[static_cast<size_t>(thisHeapIdx)][1] = 0;
            break;
        }
        }
        return true;
    }
    return false;
}

} // namespace nlang
