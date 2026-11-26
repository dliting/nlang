/*---
NcuPackage.h - .npkg package archive writer/reader (formats 1.0/1.1).

A package (.npkg) is the distribution form of one NLang package: a
header (package name, format version, flags, signature-block descriptor
- reserved, algorithm 0 = unsigned - and, for programs, an entry-point
record), a member table (module path -> offset/length/checksum, sorted
by path for deterministic output), and the embedded member blobs.
Format 1.1 adds a member-kind byte to the table: a member is a compiled
unit image or a native host library keyed by file name (no import name
ever matches it). Reading accepts 1.0 and 1.1; writing is version-
locked to the current format.

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

//Member kinds. A unit member is a compiled unit image named by its
//dotted module path; a native member is a host dynamic library named by
//its file name and carried for run-time extraction - no import name
//resolves to it.
enum class NcuMemberKind : uint8_t {
    Unit = 0,
    Native = 1,
};

//One embedded member: unit members key by module path, native members
//by file name; the blob is the .ncu image or the library file.
struct NcuMember {
    std::string modulePath;
    std::string bytes;
    NcuMemberKind kind = NcuMemberKind::Unit;
    uint64_t checksum = 0;   //computed by AddMember
};

class NcuPackageWriter {
public:
    //Duplicate member keys are rejected (false) - one package, one
    //member of each name.
    bool AddMember(NcuMember member);
    //entryRecord == nullptr packs a library (no entry-point record).
    //Members are sorted by member key inside Write (deterministic).
    bool Write(const std::string &path, const std::string &packageName,
               const NcuEntryRecord *entryRecord, std::string *error);

private:
    std::vector<NcuMember> m_members;
};

class NcuPackageReader {
public:
    //Opens and validates the whole archive and eagerly reads every
    //member blob - the run-time loader's mode (the closure walk
    //extracts every unit anyway). Returns false with *error set on a
    //malformed container.
    bool Open(const std::string &path, std::string *error);
    //Opens the member table only: header and table are read, member
    //blobs stay on disk until ExtractMember asks for one - the
    //compile-time discovery's mode, so a package carrying
    //multi-megabyte native libraries costs table IO only.
    bool OpenTable(const std::string &path, std::string *error);
    const std::vector<std::string> &MemberPaths() const { return m_memberPaths; }
    //Parallel to MemberPaths().
    const std::vector<NcuMemberKind> &MemberKinds() const { return m_memberKinds; }
    //True when a UNIT member with this module path exists. Native
    //members answer no module path: their key is a file name.
    bool HasUnitMember(const std::string &modulePath) const;
    //Extracts one member and verifies its checksum; false with *error
    //naming the package and member on a missing member or mismatch.
    //Under OpenTable the blob is read from the file on demand.
    bool ExtractMember(const std::string &modulePath,
                       std::string *ncuBytes, std::string *error) const;
    //nullptr when the package has no entry-point record (a library).
    const NcuEntryRecord *EntryRecord() const { return m_entry.get(); }
    const std::string &PackageName() const { return m_packageName; }

private:
    //Second Open/OpenTable on one reader starts from a clean slate.
    void Reset();
    //Shared header/entry/table parse. dataSize bounds each member's
    //offset/length (the table-level probe may be a file prefix); table
    //rows themselves must be present in image. Clears the table state
    //first: a retry after a probe parse failure starts clean.
    bool ParseTableFrom(const std::string &image, uint64_t dataSize,
                        std::string *error);
    //Lazy path (OpenTable): read one member's blob from the opened file.
    bool ReadMemberFromDisk(size_t memberIndex, std::string *bytes,
                            std::string *error) const;

    std::string m_path;   //opened file; lazy extraction reopens it
    std::string m_packageName;
    std::vector<std::string> m_memberPaths;
    //Parallel to m_memberPaths.
    std::vector<NcuMemberKind> m_memberKinds;
    std::vector<uint64_t> m_memberOffsets;
    std::vector<uint64_t> m_memberLengths;
    std::vector<uint64_t> m_memberChecksums;
    //Loaded blobs (Open only); empty under OpenTable.
    std::vector<std::string> m_memberBytes;
    std::unique_ptr<NcuEntryRecord> m_entry;
};

} // namespace nlang
