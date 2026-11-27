/*---
test_stdlib_layout.cpp - stdlib distribution package layout check.

The stdlib ships as one package per domain (docs/dev/
package_artifact_design.md, D4). Reads the real build-tree packages
through NcuPackageReader and pins the package file set under
STDLIB_DIR, each package's member table against the domain's unit list,
and the library shape (no entry record). lang and containers are
prelude-embedded and never ride along in a package.
---*/
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nlang/core/NcuPackage.h"

using namespace nlang;

namespace {
namespace fs = std::filesystem;

int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; \
        std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

//Domain -> the units that make up the package's member table.
const std::map<std::string, std::vector<std::string>> kDomainUnits = {
    {"concurrent", {"concurrent", "thread"}},
    {"io", {"io"}},
    {"math", {"math"}},
    {"fs", {"fs"}},
    {"tensor", {"tensor"}},
    {"net", {"net"}},
    {"http", {"http"}},
    {"url", {"url"}},
    {"system", {"system"}},
    {"gzip", {"gzip"}},
    {"regex", {"regex"}},
    {"json", {"json"}},
    {"web", {"web"}},
};

std::set<std::string> DomainNames() {
    std::set<std::string> names;
    for (const auto &domain : kDomainUnits)
        names.insert(domain.first);
    return names;
}

std::set<std::string> PackageNames(const fs::path &dir) {
    std::set<std::string> names;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".npkg")
            names.insert(entry.path().stem().string());
    }
    return names;
}

//The web package's member table is "web" plus one dotted sub-module per
//stdlib/web/<name>.n source (a module path is the dotted path relative
//to the project root); the build mirrors those sources into the tree.
std::set<std::string> ExpectedMembers(const std::string &domain) {
    const std::vector<std::string> &units = kDomainUnits.at(domain);
    std::set<std::string> members(units.begin(), units.end());
    if (domain == "web") {
        std::error_code ec;
        const fs::path subDir = fs::path(STDLIB_DIR) / "web";
        for (const auto &entry : fs::directory_iterator(subDir, ec)) {
            if (entry.path().extension() == ".n")
                members.insert("web." + entry.path().stem().string());
        }
    }
    return members;
}
} // namespace

int main() {
    const fs::path stdlibDir(STDLIB_DIR);
    const std::set<std::string> expectedPackages = DomainNames();
    const std::set<std::string> builtPackages = PackageNames(stdlibDir);

    //File set: the legacy monolithic package is gone, exactly the
    //domain packages are present.
    CHECK(!fs::exists(stdlibDir / "stdlib.npkg"),
          "the legacy stdlib.npkg must not be built");
    for (const auto &name : expectedPackages)
        CHECK(builtPackages.count(name) == 1,
              ("missing package " + name + ".npkg").c_str());
    for (const auto &name : builtPackages)
        CHECK(expectedPackages.count(name) == 1,
              ("unexpected package " + name + ".npkg").c_str());

    //Member tables: each package is a readable library holding exactly
    //its domain's units.
    std::set<std::string> packagedUnits;
    for (const auto &domain : kDomainUnits) {
        const std::string &name = domain.first;
        const fs::path packagePath = stdlibDir / (name + ".npkg");
        NcuPackageReader reader;
        std::string error;
        if (!reader.Open(packagePath.string(), &error)) {
            ++g_fail;
            std::fprintf(stderr, "FAIL: cannot open %s: %s\n",
                         packagePath.string().c_str(), error.c_str());
            continue;
        }
        const std::set<std::string> expected = ExpectedMembers(name);
        const std::vector<std::string> &members = reader.MemberPaths();
        const std::set<std::string> actual(members.begin(), members.end());
        packagedUnits.insert(actual.begin(), actual.end());
        for (const auto &unit : expected)
            CHECK(actual.count(unit) == 1,
                  (name + ".npkg lacks member " + unit).c_str());
        for (const auto &member : actual) {
            CHECK(expected.count(member) == 1,
                  (name + ".npkg has unexpected member " + member).c_str());
            std::string image;
            CHECK(reader.ExtractMember(member, &image, &error),
                  (name + ".npkg member " + member + " does not extract: "
                   + error).c_str());
        }
        CHECK(reader.EntryRecord() == nullptr,
              (name + ".npkg must be a library package").c_str());
    }

    //Coverage: every top-level unit outside the prelude rides in some
    //package. A unit's package is decided by a named table in CMakeLists,
    //so a new unit missing from that table would ship no package at all.
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(stdlibDir, ec)) {
        if (entry.path().extension() != ".n")
            continue;
        const std::string unit = entry.path().stem().string();
        if (unit == "lang" || unit == "containers")
            continue;
        CHECK(packagedUnits.count(unit) == 1,
              ("unit " + unit + ".n is in no package").c_str());
    }

    std::printf("stdlib layout: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
