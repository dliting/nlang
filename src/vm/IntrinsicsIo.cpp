/*---
IntrinsicsIo.cpp — io namespace intrinsics (content IO: console + text
files; fs.* handles names/metadata — see the operation principle in
StdLib.h / the Phase 11 plan).

ABI per StdLib.h: arguments are read from callParamBase slot 0 upward —
no this pointer.

Error model: file open/write failures raise IOException (class index
cached in m_ioExcClassIdx). io.readLine has no failure mode on its own —
EOF and an empty input line are both "" (documented semantics, see
language-spec); only an installed IHostIo without input makes it raise
(see INTR_Io_ReadLine below).
---*/
#include "VmExecutor.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace nlang
{

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

//Read one string argument. Returns by value: the pool can grow during an
//intrinsic (result strings), and a held reference would dangle.
static std::string ReadIoStringArg(const std::vector<std::string>& pool,
    const uint8_t* locals, uint16_t callParamBase, int slot)
{
    int32_t idx;
    std::memcpy(&idx, locals + callParamBase + slot * VALUE_SIZE, sizeof(idx));
    if (idx < 0 || static_cast<size_t>(idx) >= pool.size())
        return "";
    return pool[static_cast<size_t>(idx)];
}

bool VmExecutor::ExecuteIntrinsicIo(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    switch (intrinsicId)
    {
    case INTR_Io_Print:
    {
        std::string s = ReadIoStringArg(m_stringPool, locals,
            callParamBase, 0);
        if (m_pHostIo)
        {
            //Two calls, not one concatenation: same bytes, no temp alloc.
            m_pHostIo->OnOutput(s);
            m_pHostIo->OnOutput("\n");
        }
        else
        {
            std::fwrite(s.data(), 1, s.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
        }
        //Void return: leave pResult untouched.
        return true;
    }
    case INTR_Io_ReadLine:
    {
        //An installed host without input must fail loudly: in machine
        //mode the subprocess stdin is the protocol channel, and a silent
        //getline would consume protocol bytes. No host at all keeps the
        //console getline behavior (nvm / CLI ndb).
        if (m_pHostIo && !m_pHostIo->IsInputAvailable())
            RaiseNlangException(m_ioExcClassIdx,
                "io.readLine: stdin is not available in this session.");
        std::string line;
        //getline fails (and leaves line empty) at EOF with no chars read,
        //so an empty final line and EOF are indistinguishable — documented
        //semantics rather than a defect.
        if (!std::getline(std::cin, line))
            line.clear();
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
        m_stringPool.push_back(std::move(line));
        std::memcpy(pResult, &newIdx, sizeof(newIdx));
        return true;
    }
    case INTR_Io_ReadFile:
    {
        std::string path = ReadIoStringArg(m_stringPool, locals,
            callParamBase, 0);
        std::ifstream in(path, std::ios_base::binary);
        if (!in.is_open())
            RaiseNlangException(m_ioExcClassIdx,
                "io.readFile: cannot open \"" + path + "\".");
        //DoS bound: same cap the deserializer enforces for untrusted
        //length prefixes — a huge local file is a program bug, not OOM.
        in.seekg(0, std::ios_base::end);
        const std::streamoff size = in.tellg();
        if (size < 0)
            RaiseNlangException(m_ioExcClassIdx,
                "io.readFile: cannot determine size of \"" + path + "\".");
        if (static_cast<unsigned long long>(size) > MAX_STRING_LENGTH)
            RaiseNlangException(m_ioExcClassIdx,
                "io.readFile: \"" + path + "\" is too large to read.");
        std::string content(static_cast<size_t>(size), '\0');
        in.seekg(0, std::ios_base::beg);
        if (size > 0)
            in.read(&content[0], size);
        //gcount catches a short read (file shrank between tellg and read:
        //eofbit+failbit, not badbit) — otherwise the tail stays zero-filled.
        if (in.bad() || in.gcount() != static_cast<std::streamsize>(size))
            RaiseNlangException(m_ioExcClassIdx,
                "io.readFile: read error on \"" + path + "\".");
        int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
        m_stringPool.push_back(std::move(content));
        std::memcpy(pResult, &newIdx, sizeof(newIdx));
        return true;
    }
    case INTR_Io_WriteFile:
    case INTR_Io_AppendFile:
    {
        std::string path = ReadIoStringArg(m_stringPool, locals,
            callParamBase, 0);
        std::string content = ReadIoStringArg(m_stringPool, locals,
            callParamBase, 1);
        const char* funcName = (intrinsicId == INTR_Io_WriteFile)
            ? "writeFile" : "appendFile";
        std::ofstream out(path,
            std::ios_base::binary | std::ios_base::out
            | ((intrinsicId == INTR_Io_WriteFile)
                ? std::ios_base::trunc : std::ios_base::app));
        if (!out.is_open())
            RaiseNlangException(m_ioExcClassIdx,
                std::string("io.") + funcName + ": cannot open \"" + path
                + "\".");
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (out.fail())
            RaiseNlangException(m_ioExcClassIdx,
                std::string("io.") + funcName + ": write error on \"" + path
                + "\".");
        //Void return: leave pResult untouched.
        return true;
    }
    default:
        return false;
    }
}

} //namespace nlang
