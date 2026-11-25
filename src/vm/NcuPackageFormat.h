/*---
    NcuPackageFormat.h - the .npkg byte-layout layer shared by the
    archive writer and reader (internal to src/vm; the public API is
    nlang/vm/NcuPackage.h). One translation unit - NcuPackageFormat.cpp
    - owns every constant, builder and parser of the container layout,
    so the write side and the read side cannot drift apart.
---*/
#ifndef NLANG_VM_NCU_PACKAGE_FORMAT_H
#define NLANG_VM_NCU_PACKAGE_FORMAT_H

#include "nlang/vm/NcuPackage.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nlang
{

//The table-level open reads at most this many bytes up front: the
//header and member table of every real package fit inside, so member
//blobs (multi-megabyte native libraries included) cost no IO. A table
//that overflows the probe - like any parse failure on a partial read -
//escalates to one full read; the full parse's error is the honest one.
constexpr size_t NPKG_TABLE_PROBE = 128 * 1024;

//Read the whole file; false with *error when it cannot be opened.
bool ReadFileImage(const std::string &path, std::string &image,
                   std::string *error);

//Build the container header (everything before the member table).
std::string BuildNpkgHeader(const std::string &packageName,
                            const NcuEntryRecord *entryRecord,
                            size_t memberCount);

//Build the member table with offsets pointing at blobBase + the running
//size of the members before each one. A 1.1 row carries the member
//kind after the key.
std::string BuildNpkgTable(const std::vector<NcuMember> &members,
                           uint64_t blobBase);

//Validate magic/version and unpack the package name and member count,
//stopping right after the member count. Outputs the accepted format
//minor (the member table's row layout depends on it) and the resume
//position (the entry record follows); returns false with *error set.
bool ParseNpkgHeader(const std::string &image, std::string &packageName,
                     uint16_t &formatMinor, uint32_t &memberCount,
                     size_t &pos, std::string *error);

//Parse the entry-point record at pos (present when the marker byte is
//not NUL; a lone NUL means the package is a library). Advances pos.
bool ParseEntryRecord(const std::string &image, size_t &pos,
                      std::unique_ptr<NcuEntryRecord> &entry,
                      const std::string &path, std::string *error);

//Parse the member table at pos: per member a u16-prefixed key, then -
//in 1.1 - a kind byte, then offset/length/checksum. A 1.0 row carries
//no kind byte and every 1.0 member is a unit. Rows must be present in
//image; offset/length are validated against dataSize (the table-level
//probe may end before the blobs). Advances pos.
bool ParseMemberTable(const std::string &image, size_t &pos,
                      uint32_t memberCount, uint16_t formatMinor,
                      uint64_t dataSize,
                      std::vector<std::string> &paths,
                      std::vector<NcuMemberKind> &kinds,
                      std::vector<uint64_t> &offsets,
                      std::vector<uint64_t> &lengths,
                      std::vector<uint64_t> &checksums,
                      const std::string &path, std::string *error);

} //namespace nlang

#endif //NLANG_VM_NCU_PACKAGE_FORMAT_H
