/*---
    IntrinsicsFileStream.cpp — FileStream 内建（文件字节流读写族：基元/字符串/struct/object 序列化）
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

bool VmExecutor::ExecuteIntrinsicFileStream(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    //FileStream intrinsics (20-37): 20-29 primitives (4-byte), 30-31
    //struct, 32-33 object, 34-37 the 8-byte scalar quartet.
    if (intrinsicId >= INTR_FS_Ctor && intrinsicId <= INTR_FS_ReadDouble) {
        switch (intrinsicId) {
        case INTR_FS_Ctor: {
            //this at callParamBase[0], path string idx at [1], mode string idx at [2].
            int32_t thisHeapIdx;
            std::memcpy(&thisHeapIdx, locals + callParamBase, sizeof(thisHeapIdx));
            int32_t pathIdx, modeIdx;
            std::memcpy(&pathIdx, locals + callParamBase + kFrameSlotBytes, sizeof(pathIdx));
            std::memcpy(&modeIdx, locals + callParamBase + 2 * kFrameSlotBytes, sizeof(modeIdx));
            const std::string& path = StrVal(pathIdx);
            const std::string& mode = StrVal(modeIdx);
            if (mode != "r" && mode != "w" && mode != "a")
                throw std::runtime_error("NLang VM: FileStream mode must be \"r\", \"w\", or \"a\"");
            auto fstate = std::make_unique<FileStreamState>();
            std::ios_base::openmode om = std::ios_base::binary;
            if (mode == "r") { om |= std::ios_base::in; fstate->readable = true; }
            else if (mode == "w") { om |= std::ios_base::out | std::ios_base::trunc; fstate->writable = true; }
            else { om |= std::ios_base::out | std::ios_base::app; fstate->writable = true; }
            auto fs = std::make_unique<std::fstream>();
            fs->open(path, om);
            if (!fs->is_open())
                throw std::runtime_error("NLang VM: FileStream cannot open: " + path);
            fstate->fs = std::move(fs);
            int32_t handle = AllocFileStreamHandle();
            m_fileStreams[static_cast<size_t>(handle) - 1] = std::move(fstate);
            m_structHeap[static_cast<size_t>(thisHeapIdx)][1] = handle;
            break;
        }
        case INTR_FS_WriteInt: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeInt");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            int32_t val;
            std::memcpy(&val, locals + callParamBase + kFrameSlotBytes, sizeof(val));
            st->fs->write(reinterpret_cast<const char*>(&val), 4);
            break;
        }
        case INTR_FS_ReadInt: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readInt");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
            int32_t val;
            st->fs->read(reinterpret_cast<char*>(&val), 4);
            if (st->fs->gcount() < 4)
                throw std::runtime_error("NLang VM: ReadInt past end of stream");
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_FS_WriteFloat: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeFloat");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            float val;
            std::memcpy(&val, locals + callParamBase + kFrameSlotBytes, sizeof(val));
            st->fs->write(reinterpret_cast<const char*>(&val), 4);
            break;
        }
        case INTR_FS_ReadFloat: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readFloat");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
            float val;
            st->fs->read(reinterpret_cast<char*>(&val), 4);
            if (st->fs->gcount() < 4)
                throw std::runtime_error("NLang VM: ReadFloat past end of stream");
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_FS_WriteLong: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeLong");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            int64_t val;
            std::memcpy(&val, locals + callParamBase + kFrameSlotBytes, sizeof(val));
            st->fs->write(reinterpret_cast<const char*>(&val), 8);
            break;
        }
        case INTR_FS_ReadLong: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readLong");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
            int64_t val;
            st->fs->read(reinterpret_cast<char*>(&val), 8);
            if (st->fs->gcount() < 8)
                throw std::runtime_error("NLang VM: ReadLong past end of stream");
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_FS_WriteDouble: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeDouble");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            double val;
            std::memcpy(&val, locals + callParamBase + kFrameSlotBytes, sizeof(val));
            st->fs->write(reinterpret_cast<const char*>(&val), 8);
            break;
        }
        case INTR_FS_ReadDouble: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readDouble");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
            double val;
            st->fs->read(reinterpret_cast<char*>(&val), 8);
            if (st->fs->gcount() < 8)
                throw std::runtime_error("NLang VM: ReadDouble past end of stream");
            std::memcpy(pResult, &val, sizeof(val));
            break;
        }
        case INTR_FS_WriteString: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeString");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            int32_t strIdx;
            std::memcpy(&strIdx, locals + callParamBase + kFrameSlotBytes, sizeof(strIdx));
            const std::string& s = StrVal(strIdx);
            int32_t len = static_cast<int32_t>(s.size());
            st->fs->write(reinterpret_cast<const char*>(&len), 4);
            st->fs->write(s.data(), len);
            break;
        }
        case INTR_FS_ReadString: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readString");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
            int32_t len;
            st->fs->read(reinterpret_cast<char*>(&len), 4);
            if (st->fs->gcount() < 4)
                throw std::runtime_error("NLang VM: ReadString length prefix past end of stream");
            if (len < 0)
                throw std::runtime_error("NLang VM: ReadString length negative (" + std::to_string(len) + ")");
            if (static_cast<size_t>(len) > MAX_STRING_LENGTH)
                throw std::runtime_error(
                    "NLang VM: ReadString length exceeds cap (" + std::to_string(len)
                    + " > " + std::to_string(MAX_STRING_LENGTH) + ")");
            std::string s(static_cast<size_t>(len), '\0');
            st->fs->read(&s[0], len);
            if (st->fs->gcount() < len)
                throw std::runtime_error("NLang VM: ReadString bytes past end of stream");
            int32_t strHandle = MintNewString(std::move(s));
            std::memcpy(pResult, &strHandle, sizeof(strHandle));
            break;
        }
        case INTR_FS_Length: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "length");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            auto cur = st->fs->tellg();
            st->fs->seekg(0, std::ios_base::end);
            auto sz = st->fs->tellg();
            st->fs->seekg(cur);
            int32_t len = static_cast<int32_t>(sz);
            std::memcpy(pResult, &len, sizeof(len));
            break;
        }
        case INTR_FS_Position: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "position");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            int32_t pos = static_cast<int32_t>(st->fs->tellg());
            std::memcpy(pResult, &pos, sizeof(pos));
            break;
        }
        case INTR_FS_WriteStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeStruct");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            int32_t structHeapIdx;
            std::memcpy(&structHeapIdx, locals + callParamBase + kFrameSlotBytes,
                sizeof(structHeapIdx));
            if (structHeapIdx <= 0
                || static_cast<size_t>(structHeapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: WriteStruct on null struct");
            uint16_t structIdx = m_slotStructIdx[static_cast<size_t>(structHeapIdx)];
            SerializeStructFields(structHeapIdx, structIdx,
                [&](const uint8_t* p, size_t n) {
                    st->fs->write(reinterpret_cast<const char*>(p),
                        static_cast<std::streamsize>(n));
                }, *st);
            break;
        }
        case INTR_FS_ReadStruct: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readStruct");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
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
            DeserializeStructFields(rootHeapIdx, structIdx,
                [&](uint8_t* dst, size_t n) {
                    st->fs->read(reinterpret_cast<char*>(dst),
                        static_cast<std::streamsize>(n));
                    if (st->fs->gcount() < static_cast<std::streamsize>(n))
                        throw std::runtime_error(
                            "NLang VM: ReadStruct past end of stream");
                }, *st);
            std::memcpy(pResult, &rootHeapIdx, sizeof(rootHeapIdx));
            break;
        }
        case INTR_FS_WriteObject: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "writeObject");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->writable)
                throw std::runtime_error("NLang VM: FileStream not opened for writing");
            int32_t heapIdx;
            std::memcpy(&heapIdx, locals + callParamBase + kFrameSlotBytes,
                sizeof(heapIdx));
            if (heapIdx < 0
                || static_cast<size_t>(heapIdx) >= m_structHeap.size())
                throw std::runtime_error("NLang VM: WriteObject invalid heap index");
            SerializeClassFields(heapIdx,
                [&](const uint8_t* p, size_t n) {
                    st->fs->write(reinterpret_cast<const char*>(p),
                        static_cast<std::streamsize>(n));
                }, *st, 0);
            break;
        }
        case INTR_FS_ReadObject: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "readObject");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (!st || st->closed)
                throw std::runtime_error("NLang VM: stream handle is invalid or closed");
            if (!st->readable)
                throw std::runtime_error("NLang VM: FileStream not opened for reading");
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
                    st->fs->read(reinterpret_cast<char*>(dst),
                        static_cast<std::streamsize>(n));
                    if (st->fs->gcount() < static_cast<std::streamsize>(n))
                        throw std::runtime_error(
                            "NLang VM: ReadObject past end of stream");
                }, *st, 0);
            std::memcpy(pResult, &outHeapIdx, sizeof(outHeapIdx));
            break;
        }
        case INTR_FS_Close: {
            int32_t handle = ReadStreamHandle(callParamBase, locals,
                m_structHeap, "close");
            auto& st = m_fileStreams[static_cast<size_t>(handle) - 1];
            if (st) {
                st->closed = true;
                if (st->fs) st->fs->close();
                ClearObjIdState(*st);
            }
            size_t idx = static_cast<size_t>(handle) - 1;
            m_fileStreams[idx].reset();
            m_fileStreamFreeList.push_back(handle);
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
