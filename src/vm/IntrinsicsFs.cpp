/*---
IntrinsicsFs.cpp — fs namespace intrinsics (names/directories/metadata
only — content IO lives in io; see the operation principle in StdLib.h).

ABI per StdLib.h: arguments are read from callParamBase slot 0 upward —
no this pointer.

Error model: std::filesystem with error_code (no C++ exceptions cross
the intrinsic ABI). Mutations and queries that cannot answer raise
IOException (class index cached in m_ioExcClassIdx). The three type
predicates (exists/isFile/isDirectory) never raise: a path we cannot
stat answers "no" — the same shape std::filesystem's own error_code
predicate overloads have.
---*/
#include "VmExecutor.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace nlang
{

//TU-local alias: plain std::filesystem is spelled out where possible, but
//the path arithmetic reads better short. Deliberately NOT "fs" — that is
//the NLang namespace this TU implements.
namespace fsys = std::filesystem;

static const uint16_t VALUE_SIZE = 4; // int32 and float are both 4 bytes

//Read one string argument. Returns by value: the pool can grow during an
//intrinsic (result strings), and a held reference would dangle.
static std::string ReadFsStringArg(const std::vector<std::string>& pool,
    const uint8_t* locals, uint16_t callParamBase, int slot)
{
    int32_t idx;
    std::memcpy(&idx, locals + callParamBase + slot * VALUE_SIZE, sizeof(idx));
    if (idx < 0 || static_cast<size_t>(idx) >= pool.size())
        return "";
    return pool[static_cast<size_t>(idx)];
}

bool VmExecutor::ExecuteIntrinsicFs(uint16_t intrinsicId,
    uint16_t callParamBase, uint8_t* locals, uint8_t* pResult)
{
    switch (intrinsicId)
    {
    case INTR_FileSystem_Exists:
    case INTR_FileSystem_IsFile:
    case INTR_FileSystem_IsDir:
    {
        std::string path = ReadFsStringArg(m_stringPool, locals,
            callParamBase, 0);
        std::error_code ec;
        int32_t result = 0;
        if (intrinsicId == INTR_FileSystem_Exists)
            result = fsys::exists(path, ec) ? 1 : 0;
        else if (intrinsicId == INTR_FileSystem_IsFile)
            result = fsys::is_regular_file(path, ec) ? 1 : 0;
        else
            result = fsys::is_directory(path, ec) ? 1 : 0;
        //ec deliberately ignored: a missing or inaccessible path is a
        //"no" answer for a predicate, not an error (fs.size/listFiles
        //raise IOException for the same paths).
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_FileSystem_Size:
    {
        std::string path = ReadFsStringArg(m_stringPool, locals,
            callParamBase, 0);
        //Stat first: file_size on a non-regular file is unspecified in
        //the standard (directory "sizes" are filesystem noise), so the
        //regular-file gate is explicit rather than trusted.
        std::error_code stEc;
        fsys::file_status st = fsys::status(path, stEc);
        //Discriminate by file type, not by stEc: a plain missing file
        //also sets stEc (ENOENT maps to file_type::not_found), so "ec
        //set" alone cannot mean "cannot stat". none = type could not be
        //determined (permissions, ...); not_found falls through to the
        //exists() check below.
        if (st.type() == fsys::file_type::none)
            RaiseNlangException(m_ioExcClassIdx,
                "fs.size: cannot stat \"" + path + "\": " + stEc.message()
                + ".");
        if (!fsys::exists(st))
            RaiseNlangException(m_ioExcClassIdx,
                "fs.size: \"" + path + "\" does not exist.");
        if (!fsys::is_regular_file(st))
            RaiseNlangException(m_ioExcClassIdx,
                "fs.size: \"" + path + "\" is not a regular file.");
        std::error_code szEc;
        std::uintmax_t bytes = fsys::file_size(path, szEc);
        if (szEc)
            RaiseNlangException(m_ioExcClassIdx,
                "fs.size: cannot stat \"" + path + "\": " + szEc.message()
                + ".");
        if (bytes > static_cast<std::uintmax_t>(INT32_MAX))
            RaiseNlangException(m_ioExcClassIdx,
                "fs.size: \"" + path + "\" is too large for int.");
        int32_t result = static_cast<int32_t>(bytes);
        std::memcpy(pResult, &result, sizeof(result));
        return true;
    }
    case INTR_FileSystem_ListFiles:
    {
        std::string path = ReadFsStringArg(m_stringPool, locals,
            callParamBase, 0);
        std::error_code itEc;
        fsys::directory_iterator it(path, itEc);
        if (itEc)
            RaiseNlangException(m_ioExcClassIdx,
                "fs.listFiles: cannot open \"" + path + "\": "
                + itEc.message() + ".");
        //Collect plain strings first, then build the List so no pool
        //reference is held across allocation (trap: pool growth — same
        //discipline as string.split). Names only (no directory prefix),
        //non-recursive, regular files only — subdirectories are not
        //listed by a function named listFiles. Sorted, because the OS
        //iteration order is unspecified and the result must be testable.
        std::vector<std::string> names;
        fsys::directory_iterator end;
        while (it != end)
        {
            std::error_code typeEc;
            if (it->is_regular_file(typeEc))
                names.push_back(it->path().filename().generic_string());
            it.increment(itEc);
            if (itEc)
                RaiseNlangException(m_ioExcClassIdx,
                    "fs.listFiles: iteration error in \"" + path + "\": "
                    + itEc.message() + ".");
        }
        std::sort(names.begin(), names.end());
        //List<string> = List class instance + handle (INTR_Dict_Keys
        //recipe); elements are boxed-string heap slots.
        if (m_listClassIdx < 0)
            throw std::runtime_error(
                "NLang VM: List class not registered");
        int32_t listHeapIdx = AllocClassOnHeap(
            static_cast<uint16_t>(m_listClassIdx));
        int32_t listHandle = AllocListHandle();
        m_structHeap[static_cast<size_t>(listHeapIdx)]
            [kListHandleFieldOffset] = listHandle;
        auto& dst = m_listStore[listHandle - 1].elements;
        dst.reserve(names.size());
        for (auto& name : names)
        {
            int32_t poolIdx = static_cast<int32_t>(m_stringPool.size());
            m_stringPool.push_back(std::move(name));
            dst.push_back(AllocBoxedValue(RTK_String, poolIdx));
        }
        std::memcpy(pResult, &listHeapIdx, sizeof(listHeapIdx));
        return true;
    }
    case INTR_FileSystem_MakeDirs:
    {
        std::string path = ReadFsStringArg(m_stringPool, locals,
            callParamBase, 0);
        std::error_code ec;
        //mkdir -p semantics: the false return (path already exists as a
        //directory) is success; only a set ec is a failure.
        fsys::create_directories(path, ec);
        if (ec)
            RaiseNlangException(m_ioExcClassIdx,
                "fs.makeDirs: cannot create \"" + path + "\": "
                + ec.message() + ".");
        //Void return: leave pResult untouched.
        return true;
    }
    case INTR_FileSystem_Remove:
    {
        std::string path = ReadFsStringArg(m_stringPool, locals,
            callParamBase, 0);
        std::error_code ec;
        //File or empty directory. A missing path is a silent no-op and a
        //non-empty directory raises (fs::remove semantics both ways).
        fsys::remove(path, ec);
        if (ec)
            RaiseNlangException(m_ioExcClassIdx,
                "fs.remove: cannot remove \"" + path + "\": "
                + ec.message() + ".");
        //Void return: leave pResult untouched.
        return true;
    }
    case INTR_FileSystem_Join:
    {
        std::string a = ReadFsStringArg(m_stringPool, locals,
            callParamBase, 0);
        std::string b = ReadFsStringArg(m_stringPool, locals,
            callParamBase, 1);
        //generic_string: forward slashes on every platform; path append
        //never doubles a separator and yields b alone when a is empty.
        //A rooted b ("/b", or a Windows drive/UNC name) REPLACES a —
        //std::filesystem append semantics, same as Python os.path.join;
        //an empty b leaves a trailing separator.
        std::string joined = (fsys::path(a) / b).generic_string();
        int32_t newIdx = static_cast<int32_t>(m_stringPool.size());
        m_stringPool.push_back(std::move(joined));
        std::memcpy(pResult, &newIdx, sizeof(newIdx));
        return true;
    }
    default:
        return false;
    }
}

} //namespace nlang
