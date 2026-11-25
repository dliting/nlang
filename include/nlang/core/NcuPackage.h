/*---
NcuPackage.h - .npkg package archive writer/reader (format 1.0).

A package (.npkg) is the distribution form of one NLang package: a
header (package name, format version, flags, signature-block descriptor
- reserved, algorithm 0 = unsigned - and, for programs, an entry-point
record), a member table (module path -> offset/length/checksum, sorted
by path for deterministic output), and the embedded .ncu images.

The runtime loads the import closure from packages on the search path
and links symbolically; this header only describes the container.
---*/
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nlang {

//Program entry record: only program packages carry one (libraries are
//the same container without it). functionName is "main" today; the
//.nproj names the entry source file, defaulting to main.n.
struct NcuEntryRecord {
    std::string modulePath;
    std::string functionName;
};

//FNV-1a 64 - accidental-corruption detection only, not a security
//primitive (phase6_loader_design.md §10, layer 1). Phase 7 dependency
//records reuse this same implementation.
uint64_t NcuChecksum(const char *data, size_t size);

//One embedded compile unit: its dotted module path and the .ncu bytes.
struct NcuMember {
    std::string modulePath;
    std::string bytes;
    uint64_t checksum = 0;   //computed by AddMember
};

class NcuPackageWriter {
public:
    //Duplicate module paths are rejected (false) - one package, one
    //module of each path.
    bool AddMember(NcuMember member);
    //entryRecord == nullptr packs a library (no entry-point record).
    //Members are sorted by module path inside Write (deterministic).
    bool Write(const std::string &path, const std::string &packageName,
               const NcuEntryRecord *entryRecord, std::string *error);

private:
    std::vector<NcuMember> m_members;
};

class NcuPackageReader {
public:
    //Opens the archive and validates magic/version/member table.
    //Returns false with *error set on a malformed container.
    bool Open(const std::string &path, std::string *error);
    const std::vector<std::string> &MemberPaths() const { return m_memberPaths; }
    //Extracts one member and verifies its checksum; false with *error
    //naming the package and module on a missing member or mismatch.
    bool ExtractMember(const std::string &modulePath,
                       std::string *ncuBytes, std::string *error) const;
    //nullptr when the package has no entry-point record (a library).
    const NcuEntryRecord *EntryRecord() const { return m_entry.get(); }
    const std::string &PackageName() const { return m_packageName; }

private:
    std::string m_packageName;
    std::vector<std::string> m_memberPaths;
    //Parallel to m_memberPaths.
    std::vector<std::string> m_memberBytes;
    std::vector<uint64_t> m_memberChecksums;
    std::unique_ptr<NcuEntryRecord> m_entry;
};

} // namespace nlang
