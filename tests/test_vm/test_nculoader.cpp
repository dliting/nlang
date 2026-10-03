// nloader unit tests (phase 6 Step 3/4): synthetic .ncu images and .npkg
// archives drive the runtime closure loader — entry selection, eager
// discovery from the symbol-import tables, member location on the search
// path, and the one-shot diagnostics contract (design §4 steps 1-2, §6).
// No codegen and no linking here: nlink has its own suite
// (test_nculinker.cpp); these tests pin where the units come from.
#include "NcuLoader.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/NcuPackage.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

namespace fs = std::filesystem;

// A fresh, isolated scenario directory under the temp tree.
fs::path scenarioDir(const char* name) {
    auto d = fs::temp_directory_path() / "nlang_test_nculoader" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

CompiledModule MakeUnit(const std::string& modulePath) {
    CompiledModule u;
    u.modulePath = modulePath;
    u.name = modulePath;
    return u;
}

// One own function record; names are fully qualified keys ("alib.add") —
// the table convention the linker and loader share.
void AddFunc(CompiledModule& u, const std::string& qualifiedName,
             uint32_t paramCount) {
    CompiledFunction f;
    f.name = qualifiedName;
    f.paramCount = static_cast<uint16_t>(paramCount);
    u.functions.push_back(std::move(f));
}

// One cross-unit function import slot (the discovery input). Mirrors the
// producer's table layout: the import record rides WITH a placeholder
// tail entry in the function table (own entries 0..n-1, imports n..n+m),
// so the image stays structurally well-formed for every ownCount
// consumer (the loader refuses an import count above the table size).
void AddFuncImport(CompiledModule& u, const std::string& targetModule,
                   const std::string& qualifiedName, uint32_t paramCount) {
    u.functionImports.push_back({targetModule, qualifiedName, paramCount, ""});
    AddFunc(u, qualifiedName, paramCount);
}

// Serialize a unit as <dir>/<modulePath>.ncu (the location convention the
// loader probes). entryKey round-trips through the file.
void WriteUnit(const fs::path& dir, const CompiledModule& u,
               const std::string& entryKey = "") {
    std::ofstream out(dir / (u.modulePath + ".ncu"), std::ios::binary);
    WriteCompiledModule(out, u, entryKey);
}

// LoadClosure that must fail; captures the diagnostic.
struct FailResult {
    bool failed = false;
    std::string msg;
};

FailResult MustFail(const std::string& entryArtifact,
                    const std::vector<std::string>& searchDirs) {
    FailResult out;
    try {
        NcuLoader::Options opts;
        opts.searchDirs = searchDirs;
        NcuLoader::LoadClosure(entryArtifact, opts);
    } catch (const std::exception& e) {
        out.failed = true;
        out.msg = e.what();
    }
    return out;
}

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// --- shape 1: sibling unit discovered from a search dir (.ncu file) ---

void TestSiblingDiscoveredFromDir() {
    const auto dir = scenarioDir("sibling");
    CompiledModule main = MakeUnit("main");
    AddFunc(main, "main.main", 0);
    AddFuncImport(main, "alib", "alib.add", 2);
    WriteUnit(dir, main, "main.main");
    CompiledModule alib = MakeUnit("alib");
    AddFunc(alib, "alib.add", 2);
    WriteUnit(dir, alib);

    NcuLoader::Options opts;
    opts.searchDirs = { dir.string() };
    const NcuLoader::Result r =
        NcuLoader::LoadClosure((dir / "main.ncu").string(), opts);
    CHECK(r.units.size() == 2, "entry + sibling in the closure");
    CHECK(r.units[0].modulePath == "main", "entry unit is units[0]");
    CHECK(r.units[1].modulePath == "alib", "sibling unit follows");
    CHECK(r.entryKey == "main.main", "entry key round-trips");
}

// --- shape 2: transitive chain a -> b -> c, discovered eagerly ---

void TestTransitiveChain() {
    const auto dir = scenarioDir("chain");
    CompiledModule a = MakeUnit("a");
    AddFunc(a, "a.main", 0);
    AddFuncImport(a, "b", "b.f", 1);
    WriteUnit(dir, a, "a.main");
    CompiledModule b = MakeUnit("b");
    AddFunc(b, "b.f", 1);
    AddFuncImport(b, "c", "c.g", 0);
    WriteUnit(dir, b);
    CompiledModule c = MakeUnit("c");
    AddFunc(c, "c.g", 0);
    WriteUnit(dir, c);

    NcuLoader::Options opts;
    opts.searchDirs = { dir.string() };
    const NcuLoader::Result r =
        NcuLoader::LoadClosure((dir / "a.ncu").string(), opts);
    CHECK(r.units.size() == 3, "whole chain discovered");
    CHECK(r.units[2].modulePath == "c", "transitive unit loaded");
}

// --- shape 3: import cycle terminates (eager walk, dedup by module path) ---

void TestCycleLoads() {
    const auto dir = scenarioDir("cycle");
    CompiledModule a = MakeUnit("a");
    AddFunc(a, "a.main", 0);
    AddFuncImport(a, "b", "b.f", 0);
    WriteUnit(dir, a, "a.main");
    CompiledModule b = MakeUnit("b");
    AddFunc(b, "b.f", 0);
    AddFuncImport(b, "a", "a.main", 0);
    WriteUnit(dir, b);

    NcuLoader::Options opts;
    opts.searchDirs = { dir.string() };
    const NcuLoader::Result r =
        NcuLoader::LoadClosure((dir / "a.ncu").string(), opts);
    CHECK(r.units.size() == 2, "cycle loads each unit exactly once");
}

// --- shape 4: import resolved from a .npkg member on the search path ---

void TestPackageMemberFromSearchDir() {
    const auto entryDir = scenarioDir("pkg_entry");
    const auto libDir = scenarioDir("pkg_lib");
    CompiledModule main = MakeUnit("main");
    AddFunc(main, "main.main", 0);
    AddFuncImport(main, "io", "io.println", 1);
    WriteUnit(entryDir, main, "main.main");

    NcuPackageWriter pkg;
    NcuMember member;
    member.modulePath = "io";
    {
        CompiledModule io = MakeUnit("io");
        AddFunc(io, "io.println", 1);
        std::ostringstream out;
        WriteCompiledModule(out, io, "");
        member.bytes = out.str();
    }
    CHECK(pkg.AddMember(std::move(member)), "member added");
    std::string pkgError;
    CHECK(pkg.Write((libDir / "stdlib.npkg").string(), "stdlib",
                    nullptr, &pkgError),
          "package written");

    NcuLoader::Options opts;
    opts.searchDirs = { libDir.string() };
    const NcuLoader::Result r =
        NcuLoader::LoadClosure((entryDir / "main.ncu").string(), opts);
    CHECK(r.units.size() == 2, "entry + package member");
    CHECK(r.units[1].modulePath == "io", "member unit loaded from package");
    CHECK(r.units[1].FindFunction("io.println") >= 0,
          "member carries its function");
}

// --- shape 5: entry .npkg — entry record selects the member; siblings
// --- come from the package's own member table FIRST (they ship together),
// --- ahead of a same-named unit file on the search path.

void TestEntryPackageMembersFirst() {
    const auto dir = scenarioDir("entry_pkg");
    fs::create_directories(dir / "elsewhere");

    NcuPackageWriter pkg;
    for (const char* mod : { "main", "alib" }) {
        NcuMember member;
        member.modulePath = mod;
        CompiledModule u = MakeUnit(mod);
        if (std::string(mod) == "main") {
            AddFunc(u, "main.main", 0);
            AddFuncImport(u, "alib", "alib.real", 0);
        } else {
            AddFunc(u, "alib.real", 0);
        }
        std::ostringstream out;
        WriteCompiledModule(out, u, std::string(mod) == "main" ? "main.main" : "");
        member.bytes = out.str();
        pkg.AddMember(std::move(member));
    }
    NcuEntryRecord entry;
    entry.modulePath = "main";
    entry.functionName = "main";
    std::string pkgError;
    CHECK(pkg.Write((dir / "prog.npkg").string(), "prog", &entry, &pkgError),
          "program package written");

    //Decoy: a standalone alib.ncu on the search path offering a DIFFERENT
    //symbol — the package's own member must win.
    CompiledModule decoy = MakeUnit("alib");
    AddFunc(decoy, "alib.decoy", 0);
    WriteUnit(dir / "elsewhere", decoy);

    NcuLoader::Options opts;
    opts.searchDirs = { (dir / "elsewhere").string() };
    const NcuLoader::Result r =
        NcuLoader::LoadClosure((dir / "prog.npkg").string(), opts);
    CHECK(r.units.size() == 2, "entry member + sibling member");
    CHECK(r.units[0].modulePath == "main", "entry record selected main");
    CHECK(r.entryKey == "main.main", "entry key derived from the record");
    const CompiledModule& alibUnit = r.units[1];
    CHECK(alibUnit.FindFunction("alib.real") >= 0,
          "sibling came from the entry package");
    CHECK(alibUnit.FindFunction("alib.decoy") < 0,
          "search-path decoy did not shadow the package member");
}

// --- shape 6: missing modules all reported at once, with the searched
// --- locations listed (design section 6).

void TestMissingModulesAllReported() {
    const auto dir = scenarioDir("missing");
    CompiledModule main = MakeUnit("main");
    AddFunc(main, "main.main", 0);
    AddFuncImport(main, "ghost1", "ghost1.f", 0);
    AddFuncImport(main, "ghost2", "ghost2.g", 0);
    WriteUnit(dir, main, "main.main");

    const FailResult fr = MustFail((dir / "main.ncu").string(),
                                   { dir.string() });
    CHECK(fr.failed, "missing modules fail the load");
    CHECK(Contains(fr.msg, "ghost1"), "first missing module named");
    CHECK(Contains(fr.msg, "ghost2"), "second missing module named");
    CHECK(Contains(fr.msg, dir.string()),
          "searched locations listed");
}

// --- shape 7: within one search dir, <path>.ncu beats package members
// --- (the explicit file form outranks the ambient member lookup).

void TestSearchOrderNcuBeforePackage() {
    const auto dir = scenarioDir("order");
    CompiledModule main = MakeUnit("main");
    AddFunc(main, "main.main", 0);
    AddFuncImport(main, "c", "c.f", 0);
    WriteUnit(dir, main, "main.main");

    NcuPackageWriter pkg;
    NcuMember member;
    member.modulePath = "c";
    {
        CompiledModule fromPkg = MakeUnit("c");
        AddFunc(fromPkg, "c.fromPackage", 0);
        std::ostringstream out;
        WriteCompiledModule(out, fromPkg, "");
        member.bytes = out.str();
    }
    pkg.AddMember(std::move(member));
    std::string pkgError;
    pkg.Write((dir / "holder.npkg").string(), "holder", nullptr, &pkgError);

    CompiledModule fromFile = MakeUnit("c");
    AddFunc(fromFile, "c.fromFile", 0);
    WriteUnit(dir, fromFile);

    NcuLoader::Options opts;
    opts.searchDirs = { dir.string() };
    const NcuLoader::Result r =
        NcuLoader::LoadClosure((dir / "main.ncu").string(), opts);
    CHECK(r.units.size() == 2, "one c unit either way");
    CHECK(r.units[1].FindFunction("c.fromFile") >= 0,
          "unit file won over the package member");
}

// --- shape 8: corrupted package member bytes -> checksum diagnostic
// --- naming the package and the member, collected in the one report.
//
// The corruption flips one byte inside a marker string that exists ONLY in
// the member's serialized function records — never in the package's member
// table (which carries the module path, not function names).

void TestChecksumMismatchReported() {
    const auto entryDir = scenarioDir("cksum_entry");
    const auto libDir = scenarioDir("cksum_lib");
    CompiledModule main = MakeUnit("main");
    AddFunc(main, "main.main", 0);
    AddFuncImport(main, "io", "io.println", 1);
    WriteUnit(entryDir, main, "main.main");

    const std::string marker = "corruption_marker_fn";
    NcuPackageWriter pkg;
    NcuMember member;
    member.modulePath = "io";
    {
        CompiledModule io = MakeUnit("io");
        AddFunc(io, "io." + marker, 0);
        std::ostringstream out;
        WriteCompiledModule(out, io, "");
        member.bytes = out.str();
    }
    pkg.AddMember(std::move(member));
    std::string pkgError;
    CHECK(pkg.Write((libDir / "stdlib.npkg").string(), "stdlib",
                    nullptr, &pkgError),
          "package written");

    //Flip one byte of the marker inside the archive file.
    const fs::path pkgFile = libDir / "stdlib.npkg";
    std::string image;
    {
        std::ifstream in(pkgFile, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        image = buf.str();
    }
    const size_t at = image.find(marker);
    CHECK(at != std::string::npos, "marker found in the archive");
    if (at != std::string::npos) {
        image[at] = static_cast<char>(image[at] == 'X' ? 'Y' : 'X');
        std::ofstream out(pkgFile, std::ios::binary | std::ios::trunc);
        out.write(image.data(), static_cast<std::streamsize>(image.size()));
    }

    const FailResult fr = MustFail((entryDir / "main.ncu").string(),
                                   { libDir.string() });
    CHECK(fr.failed, "checksum mismatch fails the load");
    CHECK(Contains(fr.msg, "failed its checksum"),
          "checksum diagnostic present");
    CHECK(Contains(fr.msg, "io"), "member module named");
}

// --- shape 9: outdated .ncu version -> collected once with the file named
// --- (the entry unit still loads; the broken sibling is the problem).

void TestVersionMismatchReported() {
    const auto dir = scenarioDir("version");
    CompiledModule main = MakeUnit("main");
    AddFunc(main, "main.main", 0);
    AddFuncImport(main, "old", "old.f", 0);
    WriteUnit(dir, main, "main.main");
    CompiledModule old = MakeUnit("old");
    AddFunc(old, "old.f", 0);
    WriteUnit(dir, old);

    //Patch the sibling's version fields (magic is 8 bytes, then major/minor
    //u16 per the .ncu header layout) down to 1.9 — an "outdated" image.
    const fs::path oldFile = dir / "old.ncu";
    std::string image;
    {
        std::ifstream in(oldFile, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        image = buf.str();
    }
    image[8] = 1; image[9] = 0;   //major = 1
    image[10] = 9; image[11] = 0; //minor = 9
    {
        std::ofstream out(oldFile, std::ios::binary | std::ios::trunc);
        out.write(image.data(), static_cast<std::streamsize>(image.size()));
    }

    const FailResult fr = MustFail((dir / "main.ncu").string(),
                                   { dir.string() });
    CHECK(fr.failed, "version mismatch fails the load");
    CHECK(Contains(fr.msg, "outdated"), "version diagnostic present");
    CHECK(Contains(fr.msg, "old.ncu"), "offending file named");
}

// --- shape 10: a unit file found for module X whose header names Y is
// --- rejected loudly (a mislabeled artifact would silently bind imports
// --- to the wrong identity).

void TestMislabeledUnitRejected() {
    const auto dir = scenarioDir("mislabeled");
    CompiledModule main = MakeUnit("main");
    AddFunc(main, "main.main", 0);
    AddFuncImport(main, "ghosty", "ghosty.f", 0);
    WriteUnit(dir, main, "main.main");
    CompiledModule impostor = MakeUnit("impostor");
    AddFunc(impostor, "impostor.f", 0);
    WriteUnit(dir, impostor);
    fs::rename(dir / "impostor.ncu", dir / "ghosty.ncu");

    const FailResult fr = MustFail((dir / "main.ncu").string(),
                                   { dir.string() });
    CHECK(fr.failed, "mislabeled unit fails the load");
    CHECK(Contains(fr.msg, "ghosty"), "requested module named");
    CHECK(Contains(fr.msg, "impostor"), "actual header module named");
}

// --- shape 11: one target imported by two units loads exactly once ---

void TestDuplicateTargetLoadedOnce() {
    const auto dir = scenarioDir("dedup");
    CompiledModule a = MakeUnit("a");
    AddFunc(a, "a.main", 0);
    AddFuncImport(a, "c", "c.f", 0);
    AddFuncImport(a, "b", "b.g", 0);
    WriteUnit(dir, a, "a.main");
    CompiledModule b = MakeUnit("b");
    AddFunc(b, "b.g", 0);
    AddFuncImport(b, "c", "c.f", 0);
    WriteUnit(dir, b);
    CompiledModule c = MakeUnit("c");
    AddFunc(c, "c.f", 0);
    WriteUnit(dir, c);

    NcuLoader::Options opts;
    opts.searchDirs = { dir.string() };
    const NcuLoader::Result r =
        NcuLoader::LoadClosure((dir / "a.ncu").string(), opts);
    CHECK(r.units.size() == 3, "shared target appears once");
}

// --- shape 12: a corrupt .npkg in a searched dir is named alongside the
// --- not-found target — the archive might have held it (design section 6).

void TestUnreadablePackageNamed() {
    const auto entryDir = scenarioDir("unreadable_entry");
    const auto libDir = scenarioDir("unreadable_lib");
    CompiledModule main = MakeUnit("main");
    AddFunc(main, "main.main", 0);
    AddFuncImport(main, "ghost", "ghost.f", 0);
    WriteUnit(entryDir, main, "main.main");

    //Garbage bytes: the archive fails its magic/format check on open.
    {
        std::ofstream out(libDir / "broken.npkg", std::ios::binary);
        out << "this is not a package";
    }

    const FailResult fr = MustFail((entryDir / "main.ncu").string(),
                                   { libDir.string() });
    CHECK(fr.failed, "missing target with a corrupt archive fails");
    CHECK(Contains(fr.msg, "ghost"), "missing target named");
    CHECK(Contains(fr.msg, "unreadable packages"),
          "unreadable-package note present");
    CHECK(Contains(fr.msg, "broken.npkg"), "corrupt archive named");
}

// --- shape 13: entry .npkg without an entry record — the FIRST member is
// --- the entry unit and its own serialized key becomes the entry key.

void TestEntryPackageNoRecordFirstMember() {
    const auto dir = scenarioDir("pkg_no_record");
    NcuPackageWriter pkg;
    for (const char* mod : { "aaa", "zzz" }) {
        NcuMember member;
        member.modulePath = mod;
        CompiledModule u = MakeUnit(mod);
        AddFunc(u, std::string(mod) + ".main", 0);
        std::ostringstream out;
        WriteCompiledModule(out, u,
            std::string(mod) == "aaa" ? "aaa.main" : "");
        member.bytes = out.str();
        pkg.AddMember(std::move(member));
    }
    std::string pkgError;
    CHECK(pkg.Write((dir / "anon.npkg").string(), "anon",
                    nullptr, &pkgError),
          "record-less package written");

    NcuLoader::Options opts;
    const NcuLoader::Result r =
        NcuLoader::LoadClosure((dir / "anon.npkg").string(), opts);
    CHECK(r.units[0].modulePath == "aaa",
          "first member is the entry unit");
    CHECK(r.entryKey == "aaa.main",
          "serialized entry key used without a record");
}

// --- shape 14: entry package whose entry member cannot be extracted — the
// --- walk has nowhere to start, so the one-shot fires immediately.

void TestEntryMemberFailureOneShot() {
    const auto dir = scenarioDir("pkg_bad_entry");
    NcuPackageWriter pkg;
    NcuMember member;
    member.modulePath = "other";
    {
        CompiledModule other = MakeUnit("other");
        AddFunc(other, "other.f", 0);
        std::ostringstream out;
        WriteCompiledModule(out, other, "");
        member.bytes = out.str();
    }
    pkg.AddMember(std::move(member));
    NcuEntryRecord entry;
    entry.modulePath = "main";   //names a module the package does not hold
    entry.functionName = "main";
    std::string pkgError;
    CHECK(pkg.Write((dir / "prog.npkg").string(), "prog", &entry, &pkgError),
          "package with a dangling entry record written");

    const FailResult fr = MustFail((dir / "prog.npkg").string(), {});
    CHECK(fr.failed, "dangling entry record fails the load");
    CHECK(Contains(fr.msg, "main"), "record's module named");
    CHECK(Contains(fr.msg, "is not a member"),
          "member-missing diagnostic present");
    CHECK(Contains(fr.msg, "nloader failed"),
          "reported through the one-shot prefix");
}

// --- shape 15: an import slot carrying a traversal-shaped module path is
// --- refused as a corrupt image instead of reaching the filesystem probes.

void TestMalformedModulePathRefused() {
    const auto dir = scenarioDir("bad_path");
    CompiledModule main = MakeUnit("main");
    AddFunc(main, "main.main", 0);
    AddFuncImport(main, "..\\evil", "evil.f", 0);
    WriteUnit(dir, main, "main.main");
    fs::create_directories(dir / "outside");

    const FailResult fr = MustFail((dir / "main.ncu").string(),
                                   { (dir / "outside").string() });
    CHECK(fr.failed, "traversal-shaped target fails the load");
    CHECK(Contains(fr.msg, "malformed module path"),
          "malformed-path diagnostic present");
}

} // namespace

int main() {
    std::fprintf(stderr, "=== NcuLoader Closure Tests ===\n");
    TestSiblingDiscoveredFromDir();
    TestTransitiveChain();
    TestCycleLoads();
    TestPackageMemberFromSearchDir();
    TestEntryPackageMembersFirst();
    TestMissingModulesAllReported();
    TestSearchOrderNcuBeforePackage();
    TestChecksumMismatchReported();
    TestVersionMismatchReported();
    TestMislabeledUnitRejected();
    TestDuplicateTargetLoadedOnce();
    TestUnreadablePackageNamed();
    TestEntryPackageNoRecordFirstMember();
    TestEntryMemberFailureOneShot();
    TestMalformedModulePathRefused();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
