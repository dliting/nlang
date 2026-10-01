/*---
NcuPackage.cpp - .npkg archive writer/reader (format 1.0).
Writer and reader live in one file so the container layout cannot drift.
---*/
#include "nlang/vm/NcuPackage.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace nlang
{

namespace
{

//Container magic: 8 bytes, NUL-padded spelling of "NLANGPKG".
const char NPKG_MAGIC[8] = {'N', 'L', 'A', 'N', 'G', 'P', 'K', 'G'};
constexpr uint16_t NPKG_FORMAT_MAJOR = 1;
constexpr uint16_t NPKG_FORMAT_MINOR = 0;
constexpr uint8_t NPKG_SIGNATURE_ALGO_NONE = 0;

void PutU16(std::string &out, uint16_t v)
{
    out.append(reinterpret_cast<const char *>(&v), sizeof(v));
}

void PutU32(std::string &out, uint32_t v)
{
    out.append(reinterpret_cast<const char *>(&v), sizeof(v));
}

void PutU64(std::string &out, uint64_t v)
{
    out.append(reinterpret_cast<const char *>(&v), sizeof(v));
}

void PutLenBytes(std::string &out, const std::string &bytes)
{
    PutU16(out, static_cast<uint16_t>(bytes.size()));
    out.append(bytes);
}

uint16_t GetU16(const std::string &image, size_t pos)
{
    uint16_t v = 0;
    std::memcpy(&v, image.data() + pos, sizeof(v));
    return v;
}

uint32_t GetU32(const std::string &image, size_t pos)
{
    uint32_t v = 0;
    std::memcpy(&v, image.data() + pos, sizeof(v));
    return v;
}

uint64_t GetU64(const std::string &image, size_t pos)
{
    uint64_t v = 0;
    std::memcpy(&v, image.data() + pos, sizeof(v));
    return v;
}

} // namespace

uint64_t NcuChecksum(const char *data, size_t size)
{
    //FNV-1a 64.
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool NcuPackageWriter::AddMember(NcuMember member)
{
    for (const auto &existing : m_members)
        if (existing.modulePath == member.modulePath)
            return false;
    member.checksum = NcuChecksum(member.bytes.data(), member.bytes.size());
    m_members.push_back(std::move(member));
    return true;
}

//Build the container header (everything before the member table).
std::string BuildNpkgHeader(const std::string &packageName,
                            const NcuEntryRecord *entryRecord,
                            size_t memberCount)
{
    std::string header;
    header.append(NPKG_MAGIC, 8);
    PutU16(header, NPKG_FORMAT_MAJOR);
    PutU16(header, NPKG_FORMAT_MINOR);
    PutLenBytes(header, packageName);
    PutU32(header, 0); //flags
    //Signature block descriptor: reserved for the security layer
    //(phase6_loader_design.md §10) - algorithm 0 = unsigned.
    PutU16(header, NPKG_SIGNATURE_ALGO_NONE);
    PutU64(header, 0);
    PutU64(header, 0);
    PutU32(header, static_cast<uint32_t>(memberCount));
    if (entryRecord != nullptr)
    {
        PutLenBytes(header, entryRecord->modulePath);
        PutLenBytes(header, entryRecord->functionName);
    }
    else
    {
        header.push_back('\0'); //entry marker: absent
    }
    return header;
}

//Build the member table with offsets pointing at blobBase + the running
//size of the members before each one.
std::string BuildNpkgTable(const std::vector<NcuMember> &members,
                           uint64_t blobBase)
{
    std::string table;
    uint64_t offset = blobBase;
    for (const auto &member : members)
    {
        PutLenBytes(table, member.modulePath);
        PutU64(table, offset);
        PutU64(table, member.bytes.size());
        PutU64(table, member.checksum);
        offset += member.bytes.size();
    }
    return table;
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

namespace {

//Validate magic/version and unpack the package name, stopping right
//after the signature descriptor. Outputs the resume position (the
//member count follows); returns false with *error set.
bool ParseNpkgHeader(const std::string &image, std::string &packageName,
                     size_t &pos, std::string *error)
{
    //Magic (fixed offset 0) and version (fixed offset 8) validate before
    //anything else; both were separated from the reader's former
    //per-field minor gates by the 2.0 floor/ceiling.
    if (image.size() < 8
        || std::memcmp(image.data(), NPKG_MAGIC, sizeof(NPKG_MAGIC)) != 0)
    {
        if (error)
            *error = "Invalid package: bad magic";
        return false;
    }
    if (image.size() < 12)
    {
        if (error)
            *error = "Invalid package: truncated header";
        return false;
    }
    {
        const uint16_t major = GetU16(image, 8);
        const uint16_t minor = GetU16(image, 10);
        if (major != NPKG_FORMAT_MAJOR || minor > NPKG_FORMAT_MINOR)
        {
            if (error)
                *error = "Invalid package: unsupported format version "
                    + std::to_string(major) + "." + std::to_string(minor);
            return false;
        }
    }
    //Start after magic + major + minor.
    pos = 12;
    const uint16_t pkgNameLen = GetU16(image, pos);
    //Everything through the signature descriptor must be present.
    if (pos + 2 + pkgNameLen + 4 + 1 + 8 + 8 + 4 > image.size())
    {
        if (error)
            *error = "Invalid package: truncated header";
        return false;
    }
    packageName = image.substr(pos + 2, pkgNameLen);
    //Resume right after the signature descriptor; the member count follows.
    pos += 2 + pkgNameLen + 4 + 18;
    return true;
}

//Parse the entry-point record at pos (present when the marker byte is
//not NUL; a lone NUL means the package is a library). Advances pos.
bool ParseEntryRecord(const std::string &image, size_t &pos,
                      std::unique_ptr<NcuEntryRecord> &entry,
                      const std::string &path, std::string *error)
{
    if (pos >= image.size())
    {
        if (error)
            *error = "Invalid package '" + path + "': truncated header";
        return false;
    }
    if (image[pos] == '\0')
    {
        ++pos; //consume the absent-entry marker
        return true;
    }
    entry = std::make_unique<NcuEntryRecord>();
    const uint16_t modulePathLen = GetU16(image, pos);
    pos += 2;
    if (pos + modulePathLen > image.size())
    {
        if (error)
            *error = "Invalid package '" + path + "': truncated entry record";
        return false;
    }
    entry->modulePath = image.substr(pos, modulePathLen);
    pos += modulePathLen;
    const uint16_t functionNameLen = GetU16(image, pos);
    pos += 2;
    if (pos + functionNameLen > image.size())
    {
        if (error)
            *error = "Invalid package '" + path + "': truncated entry record";
        return false;
    }
    entry->functionName = image.substr(pos, functionNameLen);
    pos += functionNameLen;
    return true;
}

//Parse the member table at pos: per member a u16-prefixed module path,
//then offset/length/checksum. Advances pos.
bool ParseMemberTable(const std::string &image, size_t &pos,
                      uint32_t memberCount,
                      std::vector<std::string> &paths,
                      std::vector<std::string> &bytes,
                      std::vector<uint64_t> &checksums,
                      const std::string &path, std::string *error)
{
    for (uint32_t i = 0; i < memberCount; ++i)
    {
        if (pos + 2 > image.size())
        {
            if (error)
                *error = "Invalid package '" + path + "': truncated member table";
            return false;
        }
        const uint16_t pathLen = GetU16(image, pos);
        pos += 2;
        if (pos + pathLen + 8 * 3 > image.size())
        {
            if (error)
                *error = "Invalid package '" + path + "': truncated member table";
            return false;
        }
        const std::string modulePath = image.substr(pos, pathLen);
        pos += pathLen;
        const uint64_t offset = GetU64(image, pos);
        const uint64_t length = GetU64(image, pos + 8);
        const uint64_t checksum = GetU64(image, pos + 16);
        pos += 8 * 3;
        if (offset > image.size() || length > image.size() - offset)
        {
            if (error)
                *error = "Invalid package '" + path + "': member '"
                    + modulePath + "' is out of bounds";
            return false;
        }
        paths.push_back(modulePath);
        bytes.push_back(image.substr(static_cast<size_t>(offset),
                                     static_cast<size_t>(length)));
        checksums.push_back(checksum);
    }
    return true;
}

} // namespace

bool NcuPackageReader::Open(const std::string &path, std::string *error)
{
    //Idempotent: a second Open on the same reader must not append to the
    //previous parse's member vectors.
    m_packageName.clear();
    m_memberPaths.clear();
    m_memberBytes.clear();
    m_memberChecksums.clear();
    m_entry.reset();

    std::ifstream fs(path, std::ios::binary);
    if (!fs.good())
    {
        if (error)
            *error = "cannot open package file: " + path;
        return false;
    }
    std::string image((std::istreambuf_iterator<char>(fs)),
                      std::istreambuf_iterator<char>());
    size_t pos = 0;
    if (!ParseNpkgHeader(image, m_packageName, pos, error))
        return false;
    const uint32_t memberCount = GetU32(image, pos);
    pos += 4;

    if (!ParseEntryRecord(image, pos, m_entry, path, error))
        return false;
    if (!ParseMemberTable(image, pos, memberCount, m_memberPaths,
                          m_memberBytes, m_memberChecksums, path, error))
        return false;
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
        if (NcuChecksum(m_memberBytes[i].data(), m_memberBytes[i].size())
            != m_memberChecksums[i])
        {
            if (error)
                *error = "Package '" + m_packageName + "': member '"
                    + modulePath + "' failed its checksum";
            return false;
        }
        *ncuBytes = m_memberBytes[i];
        return true;
    }
    if (error)
        *error = "Package '" + m_packageName + "': member '" + modulePath
            + "' is not a member of the package";
    return false;
}

} // namespace nlang
