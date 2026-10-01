/*---
test_ncu_package.cpp - .npkg container unit tests (format 1.0).

Writer/reader round trip, deterministic member ordering, duplicate and
checksum rejections, entry-record round trip, and the magic/version
negatives. Pure container tests: no compiler, no VM.
---*/
#include <cstdio>
#include <fstream>
#include <filesystem>

#include "nlang/vm/NcuPackage.h"

using namespace nlang;

namespace {
namespace fs = std::filesystem;

int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; \
        std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

fs::path scratchDir() {
    static fs::path dir = fs::temp_directory_path() / "nlang_test_ncu_pkg";
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

    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
