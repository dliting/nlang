/*---
    NcuPackageWriter.cpp - the .npkg writer: members added by unique
    key (the checksum computed once, here), written sorted by key so
    the same input always produces the same bytes. The byte layout
    itself is built in NcuPackageFormat.cpp; the public API is
    nlang/vm/NcuPackage.h.
---*/
#include "NcuPackageFormat.h"

#include <algorithm>
#include <fstream>
#include <utility>

namespace nlang
{

bool NcuPackageWriter::AddMember(NcuMember member)
{
    for (const auto &existing : m_members)
        if (existing.modulePath == member.modulePath)
            return false;
    member.checksum = NcuChecksum(member.bytes.data(), member.bytes.size());
    m_members.push_back(std::move(member));
    return true;
}

bool NcuPackageWriter::Write(const std::string &path,
                             const std::string &packageName,
                             const NcuEntryRecord *entryRecord,
                             std::string *error)
{
    //Sort by module path: same input, same bytes (deterministic packing).
    std::sort(m_members.begin(), m_members.end(),
              [](const NcuMember &a, const NcuMember &b)
              { return a.modulePath < b.modulePath; });

    //No length arithmetic: the header is BUILT, and member offsets derive
    //from the actual sizes of the pieces before them (header, then table).
    //Two-pass table build: pass 1 measures the table with placeholder
    //offsets (8 bytes wide, same as real ones), pass 2 writes real
    //offsets with the blob base = header + table.
    std::string header = BuildNpkgHeader(packageName, entryRecord,
                                         m_members.size());
    std::string probe = BuildNpkgTable(m_members, 0);
    std::string table = BuildNpkgTable(m_members,
        static_cast<uint64_t>(header.size() + probe.size()));
    std::string out = header + table;
    for (const auto &member : m_members)
        out.append(member.bytes);

    std::ofstream fs(path, std::ios::binary);
    if (!fs.good())
    {
        if (error)
            *error = "cannot write package file: " + path;
        return false;
    }
    fs.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!fs.good())
    {
        if (error)
            *error = "cannot write package file: " + path;
        return false;
    }
    return true;
}

} //namespace nlang
