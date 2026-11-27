/*---
test_ncu_package.cpp - .npkg container unit tests (formats 1.0/1.1).

Writer/reader round trip, deterministic member ordering, duplicate and
checksum rejections, entry-record round trip, the magic/version
negatives, format 1.1 native members with kind round trip, the
table-level open with lazy extraction, the version ceiling, and 1.0
read compatibility. Pure container tests: no compiler, no VM.
---*/
#include "test_support/test_scratch.h"
#include <cstdio>
#include <fstream>
#include <filesystem>

#include "nlang/core/NcuPackage.h"

using namespace nlang;

namespace {
namespace fs = std::filesystem;

int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; \
        std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

fs::path scratchDir() {
    static fs::path dir = nlang::test::ScratchDir("nlang_test_ncu_pkg");
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

std::string someBytes(const std::string &tag, size_t size) {
    std::string bytes;
    while (bytes.size() < size)
        bytes += tag;
    return bytes;
}
} // namespace

int main() {
    std::fprintf(stderr, "=== .npkg Container Tests ===\n");
    const fs::path dir = scratchDir();

    //(1) Round trip: two members, read back byte-identical.
    {
        NcuPackageWriter writer;
        CHECK(writer.AddMember({"b.second", someBytes("beta", 40)}),
              "first member accepted");
        CHECK(writer.AddMember({"a.first", someBytes("alpha", 30)}),
              "second member accepted");
        std::string error;
        const NcuEntryRecord entry{"a.first", "main"};
        CHECK(writer.Write((dir / "round.npkg").string(), "round", &entry,
                           &error),
              "round-trip write succeeds");

        NcuPackageReader reader;
        CHECK(reader.Open((dir / "round.npkg").string(), &error),
              "round-trip read succeeds");
        const auto &paths = reader.MemberPaths();
        CHECK(paths.size() == 2 && paths[0] == "a.first"
                  && paths[1] == "b.second",
              "member paths sorted and complete");
        std::string image;
        CHECK(reader.ExtractMember("a.first", &image, &error),
              "member a.first extracts");
        CHECK(image == someBytes("alpha", 30), "a.first bytes identical");
        CHECK(reader.ExtractMember("b.second", &image, &error),
              "member b.second extracts");
        CHECK(image == someBytes("beta", 40), "b.second bytes identical");
        const NcuEntryRecord *pEntry = reader.EntryRecord();
        CHECK(pEntry != nullptr, "program package carries the entry record");
        if (pEntry) {
            CHECK(pEntry->modulePath == "a.first"
                      && pEntry->functionName == "main",
                  "entry record round-trips");
        }
    }

    //(2) Library package: no entry record.
    {
        NcuPackageWriter writer;
        CHECK(writer.AddMember({"lib.mod", someBytes("lib", 16)}),
              "member accepted");
        std::string error;
        CHECK(writer.Write((dir / "lib.npkg").string(), "lib", nullptr,
                           &error),
              "library write succeeds");
        NcuPackageReader reader;
        CHECK(reader.Open((dir / "lib.npkg").string(), &error),
              "library read succeeds");
        CHECK(reader.EntryRecord() == nullptr,
              "library package has no entry record");
    }

    //(3) Duplicate member path rejected at AddMember.
    {
        NcuPackageWriter writer;
        CHECK(writer.AddMember({"dup.mod", "one"}), "first add accepted");
        CHECK(!writer.AddMember({"dup.mod", "two"}),
              "duplicate module path rejected");
    }

    //(4) Checksum mismatch: flipping one member byte fails extraction,
    //naming the package and module.
    bool tamperNamed = false;
    {
        NcuPackageWriter writer;
        CHECK(writer.AddMember({"solo.mod", someBytes("payload", 64)}),
              "member accepted");
        std::string error;
        CHECK(writer.Write((dir / "tamper.npkg").string(), "tamper",
                           nullptr, &error),
              "write succeeds");
        const fs::path pkgPath = dir / "tamper.npkg";
        std::fstream raw(pkgPath, std::ios::binary | std::ios::in
                                      | std::ios::out);
        //Flip a byte deep inside the member blob (past header and table).
        raw.seekg(-1, std::ios::end);
        char byte = 0;
        raw.read(&byte, 1);
        raw.seekp(-1, std::ios::end);
        raw.put(static_cast<char>(byte ^ 0xFF));
        raw.close();

        NcuPackageReader reader;
        CHECK(reader.Open(pkgPath.string(), &error), "container still opens");
        std::string image;
        CHECK(!reader.ExtractMember("solo.mod", &image, &error),
              "tampered member fails extraction");
        tamperNamed = error.find("tamper") != std::string::npos
            && error.find("solo.mod") != std::string::npos;
    }
    CHECK(tamperNamed, "the checksum diagnostic names package and module");

    //(5) Magic/version negatives.
    {
        NcuPackageWriter writer;
        CHECK(writer.AddMember({"m.mod", "x"}), "member accepted");
        std::string error;
        CHECK(writer.Write((dir / "ver.npkg").string(), "ver", nullptr,
                           &error),
              "write succeeds");
        const fs::path pkgPath = dir / "ver.npkg";
        std::string bytes;
        {
            std::ifstream in(pkgPath, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), {});
        }
        //Bad magic.
        {
            std::string bad = bytes;
            bad[3] = 'X';
            std::ofstream out(pkgPath, std::ios::binary);
            out.write(bad.data(), static_cast<std::streamsize>(bad.size()));
            out.close();
            NcuPackageReader reader;
            CHECK(!reader.Open(pkgPath.string(), &error),
                  "bad magic rejected");
        }
        //Bad version.
        {
            std::string bad = bytes;
            bad[8] = 9;
            std::ofstream out(pkgPath, std::ios::binary);
            out.write(bad.data(), static_cast<std::streamsize>(bad.size()));
            out.close();
            NcuPackageReader reader;
            CHECK(!reader.Open(pkgPath.string(), &error),
                  "bad version rejected");
        }
    }

    //(6) Checksum known answers: the standard FNV-1a 64 vectors pin the
    //offset basis and the prime (the basis once lost its last digit in
    //this code, and the round-trip tests above cannot see that).
    CHECK(NcuChecksum("", 0) == 0xcbf29ce484222325ull,
          "FNV-1a 64 of the empty string");
    CHECK(NcuChecksum("a", 1) == 0xaf63dc4c8601ec8cull,
          "FNV-1a 64 of \"a\"");
    CHECK(NcuChecksum("foobar", 6) == 0x85944171f73967e8ull,
          "FNV-1a 64 of \"foobar\"");

    //(7) Format 1.1 native members: a native host library rides the
    //archive beside the unit images, keyed by file name and carrying
    //its kind; the written container reports minor 1, and the kinds
    //round trip.
    {
        NcuPackageWriter writer;
        CHECK(writer.AddMember({"app.main", someBytes("main", 20)}),
              "unit member accepted");
        CHECK(writer.AddMember({"nlang_net.dll", someBytes("dll", 12),
                                NcuMemberKind::Native}),
              "native member accepted");
        std::string error;
        const NcuEntryRecord entry{"app.main", "main"};
        const fs::path pkgPath = dir / "native.npkg";
        CHECK(writer.Write(pkgPath.string(), "native", &entry, &error),
              "1.1 write succeeds");
        //The writer is version-locked to the current format: the raw
        //version field must read 1.1 (major at byte 8, minor at 10).
        {
            std::string raw;
            std::ifstream in(pkgPath, std::ios::binary);
            raw.assign(std::istreambuf_iterator<char>(in), {});
            CHECK(raw.size() >= 12 && raw[8] == '\x01' && raw[10] == '\x01',
                  "written package carries format version 1.1");
        }
        NcuPackageReader reader;
        CHECK(reader.Open(pkgPath.string(), &error), "1.1 read succeeds");
        const auto &paths = reader.MemberPaths();
        CHECK(paths.size() == 2 && paths[0] == "app.main"
                  && paths[1] == "nlang_net.dll",
              "1.1 member paths sorted and complete");
        const auto &kinds = reader.MemberKinds();
        CHECK(kinds.size() == 2 && kinds[0] == NcuMemberKind::Unit
                  && kinds[1] == NcuMemberKind::Native,
              "member kinds round trip");
        CHECK(reader.HasUnitMember("app.main"), "unit member answers");
        CHECK(!reader.HasUnitMember("nlang_net.dll"),
              "native member answers no module path");
        std::string image;
        CHECK(reader.ExtractMember("nlang_net.dll", &image, &error),
              "native member extracts");
        CHECK(image == someBytes("dll", 12), "native bytes identical");
        const NcuEntryRecord *pEntry = reader.EntryRecord();
        CHECK(pEntry != nullptr && pEntry->modulePath == "app.main",
              "entry record unaffected by native members");
    }

    //(8) Table-level open: OpenTable reads the member table without the
    //blobs; extraction then reads a member on demand and still verifies
    //its checksum.
    {
        NcuPackageWriter writer;
        CHECK(writer.AddMember({"app.main", someBytes("main", 20)}),
              "unit member accepted");
        CHECK(writer.AddMember({"nlang_net.dll", someBytes("dll", 512),
                                NcuMemberKind::Native}),
              "native member accepted");
        std::string error;
        const fs::path pkgPath = dir / "table.npkg";
        CHECK(writer.Write(pkgPath.string(), "table", nullptr, &error),
              "library write succeeds");
        NcuPackageReader reader;
        CHECK(reader.OpenTable(pkgPath.string(), &error),
              "table-level open succeeds");
        CHECK(reader.PackageName() == "table", "package name parsed");
        CHECK(reader.MemberPaths().size() == 2
                  && reader.MemberPaths()[1] == "nlang_net.dll",
              "member table visible without blobs");
        CHECK(reader.MemberKinds()[1] == NcuMemberKind::Native,
              "kinds visible without blobs");
        CHECK(reader.EntryRecord() == nullptr,
              "library package has no entry record");
        std::string image;
        CHECK(reader.ExtractMember("app.main", &image, &error),
              "lazy extraction of a unit member");
        CHECK(image == someBytes("main", 20), "lazy bytes identical");
        CHECK(reader.ExtractMember("nlang_net.dll", &image, &error),
              "lazy extraction of a native member");
        CHECK(image == someBytes("dll", 512), "lazy native bytes identical");

        //A tampered blob under the lazy path fails its checksum,
        //naming the package and the member.
        const fs::path tamperedPath = dir / "table-tampered.npkg";
        fs::copy_file(pkgPath, tamperedPath);
        {
            std::fstream raw(tamperedPath,
                             std::ios::binary | std::ios::in | std::ios::out);
            raw.seekg(-1, std::ios::end);
            char byte = 0;
            raw.read(&byte, 1);
            raw.seekp(-1, std::ios::end);
            raw.put(static_cast<char>(byte ^ 0xFF));
        }
        NcuPackageReader tampered;
        CHECK(tampered.OpenTable(tamperedPath.string(), &error),
              "tampered container still opens at table level");
        std::string bytes;
        CHECK(!tampered.ExtractMember("nlang_net.dll", &bytes, &error),
              "lazy extraction verifies the checksum");
        CHECK(error.find("table") != std::string::npos
                  && error.find("nlang_net.dll") != std::string::npos,
              "lazy checksum diagnostic names package and member");
    }

    //(9) Version ceiling: a container claiming a format this reader
    //does not know is refused with the version named. The pin guards
    //the gate's existence: a future format bump must never silently
    //widen the accepted range.
    {
        NcuPackageWriter writer;
        CHECK(writer.AddMember({"m.mod", "x"}), "member accepted");
        std::string error;
        const fs::path pkgPath = dir / "future.npkg";
        CHECK(writer.Write(pkgPath.string(), "future", nullptr, &error),
              "write succeeds");
        std::string bytes;
        {
            std::ifstream in(pkgPath, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), {});
        }
        bytes[10] = 2;   //claim format 1.2
        {
            std::ofstream out(pkgPath, std::ios::binary);
            out.write(bytes.data(),
                      static_cast<std::streamsize>(bytes.size()));
        }
        NcuPackageReader full;
        CHECK(!full.Open(pkgPath.string(), &error),
              "full open refuses a future format version");
        CHECK(error.find("1.2") != std::string::npos,
              "the refusal names the version");
        NcuPackageReader table;
        CHECK(!table.OpenTable(pkgPath.string(), &error),
              "table-level open refuses a future format version");
        CHECK(error.find("1.2") != std::string::npos,
              "the table-level refusal names the version");
    }

    //(10) Format 1.0 read compatibility: packages written by the
    //previous toolchain (no member-kind byte, minor 0) keep opening,
    //their members reading as units.
    {
        const std::string image1 = someBytes("one", 11);
        const std::string image2 = someBytes("two", 22);
        const std::vector<std::pair<std::string, std::string>> members{
            {"a.first", image1}, {"b.second", image2}};
        const NcuEntryRecord entry{"a.first", "main"};
        std::string out;
        const auto put16 = [&out](uint16_t v) {
            out.append(reinterpret_cast<const char *>(&v), sizeof(v));
        };
        const auto put64 = [&out](uint64_t v) {
            out.append(reinterpret_cast<const char *>(&v), sizeof(v));
        };
        out.append("NLANGPKG", 8);
        put16(1);   //major
        put16(0);   //minor: format 1.0
        put16(static_cast<uint16_t>(3));   //package name "old"
        out.append("old");
        const uint32_t flags = 0;
        out.append(reinterpret_cast<const char *>(&flags), sizeof(flags));
        const uint16_t sigAlgo = 0;
        out.append(reinterpret_cast<const char *>(&sigAlgo), sizeof(sigAlgo));
        const uint64_t reserved = 0;
        out.append(reinterpret_cast<const char *>(&reserved), sizeof(reserved));
        out.append(reinterpret_cast<const char *>(&reserved), sizeof(reserved));
        const uint32_t count = static_cast<uint32_t>(members.size());
        out.append(reinterpret_cast<const char *>(&count), sizeof(count));
        //Entry record present: the two strings (a lone NUL would mark
        //their absence; presence has no marker byte).
        put16(static_cast<uint16_t>(entry.modulePath.size()));
        out.append(entry.modulePath);
        put16(static_cast<uint16_t>(entry.functionName.size()));
        out.append(entry.functionName);
        //Table rows without a kind byte; offsets point past header and
        //table, so the base adds each row's own size first.
        uint64_t offset = out.size();
        for (const auto &member : members)
            offset += 2 + member.first.size() + 24;
        std::string table;
        for (const auto &member : members) {
            std::string row;
            const auto row16 = [&row](uint16_t v) {
                row.append(reinterpret_cast<const char *>(&v), sizeof(v));
            };
            const auto row64 = [&row](uint64_t v) {
                row.append(reinterpret_cast<const char *>(&v), sizeof(v));
            };
            row16(static_cast<uint16_t>(member.first.size()));
            row.append(member.first);
            row64(offset);
            row64(member.second.size());
            row64(NcuChecksum(member.second.data(), member.second.size()));
            offset += member.second.size();
            table += row;
        }
        out += table;
        for (const auto &member : members)
            out += member.second;
        const fs::path pkgPath = dir / "old10.npkg";
        {
            std::ofstream file(pkgPath, std::ios::binary);
            file.write(out.data(), static_cast<std::streamsize>(out.size()));
        }
        NcuPackageReader reader;
        std::string error;
        CHECK(reader.Open(pkgPath.string(), &error),
              "1.0 package opens under the current reader");
        CHECK(reader.MemberPaths().size() == 2
                  && reader.MemberPaths()[0] == "a.first",
              "1.0 member table parsed");
        CHECK(reader.MemberKinds().size() == 2
                  && reader.MemberKinds()[0] == NcuMemberKind::Unit
                  && reader.MemberKinds()[1] == NcuMemberKind::Unit,
              "1.0 members read as units");
        std::string blob;
        CHECK(reader.ExtractMember("b.second", &blob, &error),
              "1.0 member extracts");
        CHECK(blob == image2, "1.0 member bytes identical");
        const NcuEntryRecord *pEntry = reader.EntryRecord();
        CHECK(pEntry != nullptr && pEntry->modulePath == "a.first"
                  && pEntry->functionName == "main",
              "1.0 entry record parsed");
        NcuPackageReader lazy;
        CHECK(lazy.OpenTable(pkgPath.string(), &error),
              "1.0 package opens at table level");
        CHECK(lazy.ExtractMember("a.first", &blob, &error),
              "1.0 lazy extraction");
        CHECK(blob == image1, "1.0 lazy bytes identical");
    }


    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
