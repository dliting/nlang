/*---
    NcuPackageReader.cpp - the .npkg reader: Open (eager whole-file
    read, the run-time loader's mode) and OpenTable (table level, the
    compile-time discovery mode - header and member table only, members
    extracted lazily), both verifying member checksums on extraction.
    The byte layout itself is parsed in NcuPackageFormat.cpp; the public
    API is nlang/vm/NcuPackage.h.
---*/
#include "NcuPackageFormat.h"

#include <algorithm>
#include <fstream>
#include <utility>

namespace nlang
{

void NcuPackageReader::Reset()
{
    m_path.clear();
    m_packageName.clear();
    m_memberPaths.clear();
    m_memberKinds.clear();
    m_memberOffsets.clear();
    m_memberLengths.clear();
    m_memberChecksums.clear();
    m_memberBytes.clear();
    m_entry.reset();
}

bool NcuPackageReader::ParseTableFrom(const std::string &image,
                                      uint64_t dataSize, std::string *error)
{
    m_packageName.clear();
    m_memberPaths.clear();
    m_memberKinds.clear();
    m_memberOffsets.clear();
    m_memberLengths.clear();
    m_memberChecksums.clear();
    m_memberBytes.clear();
    m_entry.reset();
    size_t pos = 0;
    uint16_t formatMinor = 0;
    uint32_t memberCount = 0;
    if (!ParseNpkgHeader(image, m_packageName, formatMinor, memberCount,
                         pos, error))
        return false;
    if (!ParseEntryRecord(image, pos, m_entry, m_path, error))
        return false;
    return ParseMemberTable(image, pos, memberCount, formatMinor, dataSize,
                            m_memberPaths, m_memberKinds, m_memberOffsets,
                            m_memberLengths, m_memberChecksums, m_path,
                            error);
}

bool NcuPackageReader::Open(const std::string &path, std::string *error)
{
    Reset();
    m_path = path;
    std::string image;
    if (!ReadFileImage(path, image, error))
        return false;
    if (!ParseTableFrom(image, image.size(), error))
        return false;
    //Eager blobs - the run-time loader's mode: the closure walk extracts
    //every unit member anyway, and the mirror wants the whole image.
    m_memberBytes.resize(m_memberPaths.size());
    for (size_t i = 0; i < m_memberPaths.size(); ++i)
        m_memberBytes[i] = image.substr(static_cast<size_t>(m_memberOffsets[i]),
                                        static_cast<size_t>(m_memberLengths[i]));
    return true;
}

bool NcuPackageReader::OpenTable(const std::string &path, std::string *error)
{
    Reset();
    m_path = path;
    std::ifstream fs(path, std::ios::binary);
    if (!fs.good())
    {
        if (error)
            *error = "cannot open package file: " + path;
        return false;
    }
    fs.seekg(0, std::ios::end);
    const std::streampos endPos = fs.tellg();
    fs.seekg(0, std::ios::beg);
    if (!fs.good() || endPos < 0)
    {
        if (error)
            *error = "cannot open package file: " + path;
        return false;
    }
    //Probe read: header and table of every real package fit inside the
    //probe, so member blobs cost no IO here.
    const auto probeSize = static_cast<std::streamsize>(
        std::min<uint64_t>(static_cast<uint64_t>(endPos), NPKG_TABLE_PROBE));
    std::string image(static_cast<size_t>(probeSize), '\0');
    fs.read(image.data(), probeSize);
    if (fs.gcount() != probeSize)
    {
        if (error)
            *error = "cannot open package file: " + path;
        return false;
    }
    if (ParseTableFrom(image, static_cast<uint64_t>(endPos), error))
        return true;
    //A table that overflows the probe - like any parse failure on a
    //partial read - escalates to one full read; the full parse's error
    //is the honest one.
    if (!ReadFileImage(path, image, error))
        return false;
    return ParseTableFrom(image, image.size(), error);
}

bool NcuPackageReader::HasUnitMember(const std::string &modulePath) const
{
    for (size_t i = 0; i < m_memberPaths.size(); ++i)
    {
        if (m_memberPaths[i] == modulePath
            && m_memberKinds[i] == NcuMemberKind::Unit)
            return true;
    }
    return false;
}

bool NcuPackageReader::ReadMemberFromDisk(size_t memberIndex,
                                          std::string *bytes,
                                          std::string *error) const
{
    std::ifstream fs(m_path, std::ios::binary);
    if (!fs.good())
    {
        if (error)
            *error = "cannot open package file: " + m_path;
        return false;
    }
    fs.seekg(static_cast<std::streamoff>(m_memberOffsets[memberIndex]));
    bytes->assign(static_cast<size_t>(m_memberLengths[memberIndex]), '\0');
    fs.read(bytes->data(), static_cast<std::streamsize>(bytes->size()));
    if (fs.gcount() != static_cast<std::streamsize>(bytes->size()))
    {
        if (error)
            *error = "cannot read package file: " + m_path;
        return false;
    }
    return true;
}

bool NcuPackageReader::ExtractMember(const std::string &modulePath,
                                     std::string *ncuBytes,
                                     std::string *error) const
{
    for (size_t i = 0; i < m_memberPaths.size(); ++i)
    {
        if (m_memberPaths[i] != modulePath)
            continue;
        //Eager blobs when present (Open), a lazy read otherwise
        //(OpenTable); both paths verify the checksum.
        std::string blob;
        if (!m_memberBytes.empty())
        {
            blob = m_memberBytes[i];
        }
        else if (!ReadMemberFromDisk(i, &blob, error))
        {
            return false;
        }
        if (NcuChecksum(blob.data(), blob.size()) != m_memberChecksums[i])
        {
            if (error)
                *error = "Package '" + m_packageName + "': member '"
                    + modulePath + "' failed its checksum";
            return false;
        }
        *ncuBytes = std::move(blob);
        return true;
    }
    if (error)
        *error = "Package '" + m_packageName + "': member '" + modulePath
            + "' is not a member of the package";
    return false;
}

} //namespace nlang
