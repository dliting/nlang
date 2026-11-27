/*---
    NcuPackageFormat.cpp - the .npkg byte-layout layer: every constant,
    builder and parser of the container format (1.0/1.1) lives in this
    one translation unit, so the write side and the read side cannot
    drift apart. The writer and reader classes (NcuPackageWriter.cpp,
    NcuPackageReader.cpp) compose from here; the public API is
    nlang/core/NcuPackage.h.
---*/
#include "NcuPackageFormat.h"

#include <cstring>
#include <fstream>

namespace nlang
{

namespace
{

//Container magic: 8 bytes, NUL-padded spelling of "NLANGPKG".
const char NPKG_MAGIC[8] = {'N', 'L', 'A', 'N', 'G', 'P', 'K', 'G'};
constexpr uint16_t NPKG_FORMAT_MAJOR = 1;
//1.1 adds the member-kind byte to the table. Reading accepts every
//earlier minor; writing always emits the current format (a 1.1 package
//is refused by a 1.0-only reader with the version named - the no
//backward compatibility rule).
constexpr uint16_t NPKG_FORMAT_MINOR = 1;
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
    //FNV-1a 64 (the standard offset basis and prime; the basis once
    //lost its last digit here — see the known-answer tests).
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

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

std::string BuildNpkgTable(const std::vector<NcuMember> &members,
                           uint64_t blobBase)
{
    std::string table;
    uint64_t offset = blobBase;
    for (const auto &member : members)
    {
        PutLenBytes(table, member.modulePath);
        table.push_back(static_cast<char>(member.kind));
        PutU64(table, offset);
        PutU64(table, member.bytes.size());
        PutU64(table, member.checksum);
        offset += member.bytes.size();
    }
    return table;
}

bool ReadFileImage(const std::string &path, std::string &image,
                   std::string *error)
{
    std::ifstream fs(path, std::ios::binary);
    if (!fs.good())
    {
        if (error)
            *error = "cannot open package file: " + path;
        return false;
    }
    image.assign(std::istreambuf_iterator<char>(fs),
                 std::istreambuf_iterator<char>());
    return true;
}

bool ParseNpkgHeader(const std::string &image, std::string &packageName,
                     uint16_t &formatMinor, uint32_t &memberCount,
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
        formatMinor = minor;
    }
    //Start after magic + major + minor.
    pos = 12;
    const uint16_t pkgNameLen = GetU16(image, pos);
    //Everything through the member count must be present.
    if (pos + 2 + pkgNameLen + 4 + 1 + 8 + 8 + 4 > image.size())
    {
        if (error)
            *error = "Invalid package: truncated header";
        return false;
    }
    packageName = image.substr(pos + 2, pkgNameLen);
    //Resume right after the member count; the entry record follows.
    pos += 2 + pkgNameLen + 4 + 18;
    memberCount = GetU32(image, pos);
    pos += 4;
    return true;
}

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

//The kind byte of a 1.1 row at pos (a 1.0 row has none and every
//1.0 member is a unit). Advances pos. False on an unknown value.
bool ParseMemberKind(const std::string &image, size_t &pos,
                     uint16_t formatMinor, NcuMemberKind &kind,
                     const std::string &modulePath,
                     const std::string &path, std::string *error)
{
    if (formatMinor < 1)
    {
        kind = NcuMemberKind::Unit;
        return true;
    }
    const uint8_t rawKind = static_cast<uint8_t>(image[pos]);
    ++pos;
    if (rawKind > static_cast<uint8_t>(NcuMemberKind::Native))
    {
        if (error)
            *error = "Invalid package '" + path + "': member '"
                + modulePath + "' has an unknown kind byte";
        return false;
    }
    kind = static_cast<NcuMemberKind>(rawKind);
    return true;
}

//Parse one member row at pos: the u16-prefixed key, the kind byte
//(1.1 only; a 1.0 row has none and every 1.0 member is a unit), then
//offset/length/checksum. The row must be present in image; offset and
//length are validated against dataSize (the table-level probe may end
//before the blobs). Appends to the parallel arrays only on success.
//Advances pos.
bool ParseMemberRow(const std::string &image, size_t &pos,
                    uint16_t formatMinor, uint64_t dataSize,
                    std::vector<std::string> &paths,
                    std::vector<NcuMemberKind> &kinds,
                    std::vector<uint64_t> &offsets,
                    std::vector<uint64_t> &lengths,
                    std::vector<uint64_t> &checksums,
                    const std::string &path, std::string *error)
{
    const size_t rowRest = formatMinor >= 1 ? 8 * 3 + 1 : 8 * 3;
    if (pos + 2 > image.size())
    {
        if (error)
            *error = "Invalid package '" + path + "': truncated member table";
        return false;
    }
    const uint16_t pathLen = GetU16(image, pos);
    pos += 2;
    if (pos + pathLen + rowRest > image.size())
    {
        if (error)
            *error = "Invalid package '" + path + "': truncated member table";
        return false;
    }
    const std::string modulePath = image.substr(pos, pathLen);
    pos += pathLen;
    NcuMemberKind kind = NcuMemberKind::Unit;
    if (!ParseMemberKind(image, pos, formatMinor, kind, modulePath, path,
                         error))
        return false;
    const uint64_t offset = GetU64(image, pos);
    const uint64_t length = GetU64(image, pos + 8);
    const uint64_t checksum = GetU64(image, pos + 16);
    pos += 8 * 3;
    if (offset > dataSize || length > dataSize - offset)
    {
        if (error)
            *error = "Invalid package '" + path + "': member '"
                + modulePath + "' is out of bounds";
        return false;
    }
    paths.push_back(modulePath);
    kinds.push_back(kind);
    offsets.push_back(offset);
    lengths.push_back(length);
    checksums.push_back(checksum);
    return true;
}

bool ParseMemberTable(const std::string &image, size_t &pos,
                      uint32_t memberCount, uint16_t formatMinor,
                      uint64_t dataSize,
                      std::vector<std::string> &paths,
                      std::vector<NcuMemberKind> &kinds,
                      std::vector<uint64_t> &offsets,
                      std::vector<uint64_t> &lengths,
                      std::vector<uint64_t> &checksums,
                      const std::string &path, std::string *error)
{
    for (uint32_t i = 0; i < memberCount; ++i)
    {
        if (!ParseMemberRow(image, pos, formatMinor, dataSize, paths,
                            kinds, offsets, lengths, checksums, path,
                            error))
            return false;
    }
    return true;
}

} //namespace nlang
