// --- ndb debugger tests: .ncu v1.9 sourceFile + loader gates ---
#include "test_debugger_common.h"

using namespace nlang;

void test_v19_sourcefile_single_tu()
{
    TEST(v19_sourcefile_single_tu);
    BuildOutcome b = buildSource("srcfile_single",
        "int helper(int v) { return v * 2; }\n"
        "int main() { return helper(21); }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    CompiledModule mod = loadBuilt("srcfile_single");
    const std::string* mainSrc = nullptr;
    const std::string* helperSrc = nullptr;
    for (const auto& f : mod.functions) {
        if (f.name == "srcfile_single.main") mainSrc = &f.sourceFile;
        if (f.name == "srcfile_single.helper") helperSrc = &f.sourceFile;
    }
    CHECK(mainSrc != nullptr, "main should be in the module");
    CHECK(helperSrc != nullptr, "helper should be in the module");
    CHECK(mainSrc->find("srcfile_single.n") != std::string::npos,
        "main.sourceFile should name the source file, got: " + *mainSrc);
    CHECK(*mainSrc == *helperSrc,
        "same-TU functions should share the source file path");
    PASS();
}

void test_v19_import_roundtrip()
{
    TEST(v19_import_roundtrip);
    //The import target must exist as a compiled .ncu — compile-time
    //resolution needs its signatures (source or image), and the load-time
    //closure resolves the unit image from the search dir, so build the
    //lib artifact first, then the consumer.
    BuildOutcome lib = buildSource("dbgutil_lib",
        "int triple(int v) { int t = v * 3; return t; }\n");
    CHECK(lib.ok, "lib build should succeed: " + lib.diagnostics);
    BuildOutcome b = buildConsumer("dbgutil_main",
        "import dbgutil_lib;\n"
        "int main() { return dbgutil_lib.triple(14); }\n");
    CHECK(b.ok, "import build should succeed: " + b.diagnostics);

    //The consumer artifact carries import slots only — linking happens
    //at load time; the merged shape lives in the LINKED module.
    CompiledModule mod = loadLinked("dbgutil_main");
    int tripleIdx = mod.FindFunction("dbgutil_lib.triple");
    CHECK(tripleIdx >= 0, "triple should be linked into the program");
    const auto& triple = mod.functions[static_cast<size_t>(tripleIdx)];
    CHECK(triple.sourceFile.find("dbgutil_lib.n") != std::string::npos,
        "imported function keeps producer source file, got: "
        + triple.sourceFile);
    CHECK(!triple.locals.empty(),
        "B.1 merge must copy locals (GC roots + debugger visibility)");
    PASS();
}

void test_v19_loader_rejects_v1_8()
{
    TEST(v19_loader_rejects_v1_8);
    BuildOutcome b = buildSource("oldver",
        "int main() { return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    const auto modPath = scratchDir() / "oldver.ncu";

    //Patch the minorVer field (header offset 10, little-endian u16:
    //magic[8] + major(u16) + minor(u16)) down to 8 — the loader must
    //refuse v1.8 modules outright (floor bump discipline).
    std::vector<char> bytes;
    {
        std::ifstream in(modPath, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(bytes.size() >= 12, "module file should have a full header");
    //v2.0: the downgrade is a MAJOR step (1.x predates package identity).
    //Patch the major field (header offset 8, little-endian u16) down to 1.
    bytes[8] = 0x01;
    bytes[9] = 0x00;
    const auto oldPath = scratchDir() / "oldver_v18.ncu";
    {
        std::ofstream out(oldPath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool threw = false;
    try {
        ModuleLoader::Load(oldPath.string());
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw, "loader must reject a v1.8 module (below the format floor)");
    PASS();
}

void test_loader_rejects_v1_9()
{
    TEST(loader_rejects_v1_9);
    //Distinct build tag: ModuleManager::Create keys the process-global
    //loaded map by module name, so reusing "oldver" from the v1.8 test
    //would fail the build with "already exists".
    BuildOutcome b = buildSource("oldver9",
        "int main() { return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    const auto modPath = scratchDir() / "oldver9.ncu";

    //Patch the minorVer field (header offset 10, little-endian u16:
    //magic[8] + major(u16) + minor(u16)) down to 9. v1.11 is a SEMANTIC
    //floor: no layout change, but v1.10 generic-container array elements
    //flow boxed into primitive slots where the VM now expects raw traced
    //handles, which the GC would never trace.
    std::vector<char> bytes;
    {
        std::ifstream in(modPath, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(bytes.size() >= 12, "module file should have a full header");
    //Guard the patch anchor: if a future header change moves the version
    //fields, the patch below would silently hit another field — fail
    //loudly on layout drift instead (fresh build must be stamped 2.1).
    CHECK(static_cast<uint8_t>(bytes[8]) == 0x02
        && static_cast<uint8_t>(bytes[10]) == 0x01,
        "fresh module should be stamped format 2.1");
    //v2.0: the downgrade is a MAJOR step (1.x predates package identity).
    bytes[8] = 0x01;
    bytes[9] = 0x00;
    const auto oldPath = scratchDir() / "oldver_v19.ncu";
    {
        std::ofstream out(oldPath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool threw = false;
    std::string what;
    try {
        ModuleLoader::Load(oldPath.string());
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "loader must reject a v1.9 module (floor is 12)");
    CHECK(what.find("outdated") != std::string::npos,
        "rejection should hit the floor path, got: " + what);
    PASS();
}

void test_loader_rejects_v1_10()
{
    TEST(loader_rejects_v1_10);
    //Distinct build tag: ModuleManager::Create keys the process-global
    //loaded map by module name, so reusing "oldver"/"oldver9" from the
    //other floor tests would fail the build with "already exists".
    BuildOutcome b = buildSource("oldver10",
        "int main() { return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    const auto modPath = scratchDir() / "oldver10.ncu";

    //Patch the minorVer field (header offset 10, little-endian u16:
    //magic[8] + major(u16) + minor(u16)) down to 10. v1.11 is a SEMANTIC
    //floor (see the v1_9 test above for the rationale): a v1.10 module
    //boxes generic-container array elements into primitive slots the GC
    //never traces.
    std::vector<char> bytes;
    {
        std::ifstream in(modPath, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(bytes.size() >= 12, "module file should have a full header");
    CHECK(static_cast<uint8_t>(bytes[8]) == 0x02
        && static_cast<uint8_t>(bytes[10]) == 0x01,
        "fresh module should be stamped format 2.1");
    //v2.0: the downgrade is a MAJOR step.
    bytes[8] = 0x01;
    bytes[9] = 0x00;
    const auto oldPath = scratchDir() / "oldver_v110.ncu";
    {
        std::ofstream out(oldPath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool threw = false;
    std::string what;
    try {
        ModuleLoader::Load(oldPath.string());
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "loader must reject a v1.10 module (floor is 12)");
    CHECK(what.find("outdated") != std::string::npos,
        "rejection should hit the floor path, got: " + what);
    PASS();
}

void test_loader_rejects_v1_11()
{
    TEST(loader_rejects_v1_11);
    //Distinct build tag: ModuleManager::Create keys the process-global
    //loaded map by module name, so reusing the other floor tests' tags
    //would fail the build with "already exists".
    BuildOutcome b = buildSource("oldver11",
        "int main() { return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    const auto modPath = scratchDir() / "oldver11.ncu";

    //Patch the minorVer field (header offset 10, little-endian u16:
    //magic[8] + major(u16) + minor(u16)) down to 11. v1.12 is a LAYOUT
    //floor, unlike the semantic 9/10 ones: every function record now
    //ends in a u16 paramDescCount (+ per-formal / return / field type
    //descriptor bytes), so a v1.11 record would make the loader parse
    //the next record's bytes as a descriptor count — garbage at best,
    //never a module it promised to load. No migration path by design.
    std::vector<char> bytes;
    {
        std::ifstream in(modPath, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(bytes.size() >= 12, "module file should have a full header");
    CHECK(static_cast<uint8_t>(bytes[8]) == 0x02
        && static_cast<uint8_t>(bytes[10]) == 0x01,
        "fresh module should be stamped format 2.1");
    //v2.0: the downgrade is a MAJOR step.
    bytes[8] = 0x01;
    bytes[9] = 0x00;
    const auto oldPath = scratchDir() / "oldver_v111.ncu";
    {
        std::ofstream out(oldPath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool threw = false;
    std::string what;
    try {
        ModuleLoader::Load(oldPath.string());
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "loader must reject a v1.11 module (floor is 12)");
    CHECK(what.find("outdated") != std::string::npos,
        "rejection should hit the floor path, got: " + what);
    PASS();
}

//The minor-floor arm (ported from the pre-2.0 v1.12/v1.13 pins): keep
//the major at 2 and patch the minorVer field (header offset 10,
//little-endian u16) down to 0 -- below the 2.1 floor. The 2.1 minor
//carries the debugger declPc layout (two bytes of declaration PC per
//local in every locals block); a 2.0 record would read every local
//name length from the wrong offset. No migration path by design.
void test_loader_rejects_minor_below_floor()
{
    TEST(loader_rejects_minor_below_floor);
    //Distinct build tag: ModuleManager::Create keys the process-global
    //loaded map by module name, so reusing the other floor tests' tags
    //would fail the build with "already exists".
    BuildOutcome b = buildSource("oldvermin",
        "int main() { return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    const auto modPath = scratchDir() / "oldvermin.ncu";

    std::vector<char> bytes;
    {
        std::ifstream in(modPath, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(bytes.size() >= 12, "module file should have a full header");
    CHECK(static_cast<uint8_t>(bytes[8]) == 0x02
        && static_cast<uint8_t>(bytes[10]) == 0x01,
        "fresh module should be stamped format 2.1");
    bytes[10] = 0x00;
    bytes[11] = 0x00;
    const auto oldPath = scratchDir() / "oldver_v200.ncu";
    {
        std::ofstream out(oldPath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool threw = false;
    std::string what;
    try {
        ModuleLoader::Load(oldPath.string());
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "loader must reject a 2.0 module (minor floor is 1)");
    CHECK(what.find("outdated") != std::string::npos,
        "rejection should hit the floor path, got: " + what);
    PASS();
}

//Boundary negative for BOTH version gates (before this there was only a
//"too old" pin; nothing stopped a floor-only regression from silently
//accepting newer modules). The fresh build is stamped 13; patch the
//header byte up to 14 (ceiling) and assert the newer-ncc wording, then
//down to 12 (floor) and assert the outdated wording — the exact loader
//sentences, not just "it threw" (same reason as the v1.8 pin).
void test_loader_accepts_ceiling_and_floor()
{
    TEST(loader_accepts_ceiling_and_floor);
    //Distinct build tag: ModuleManager::Create keys the process-global
    //loaded map by module name, so reusing the other floor tests' tags
    //would fail the build with "already exists".
    BuildOutcome b = buildSource("verbound",
        "int main() { return 0; }\n");
    CHECK(b.ok, "build should succeed: " + b.diagnostics);
    const auto modPath = scratchDir() / "verbound.ncu";

    std::vector<char> bytes;
    {
        std::ifstream in(modPath, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    CHECK(bytes.size() >= 12, "module file should have a full header");
    CHECK(static_cast<uint8_t>(bytes[8]) == 0x02
        && static_cast<uint8_t>(bytes[10]) == 0x01,
        "fresh module should be stamped format 2.1");

    //Above the ceiling: a v3.0 module — the reader must refuse it (it
    //would misparse every record after the first layout change).
    bytes[8] = 0x03;
    bytes[9] = 0x00;
    const auto newPath = scratchDir() / "verbound_v300.ncu";
    {
        std::ofstream out(newPath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    bool threw = false;
    std::string what;
    try {
        ModuleLoader::Load(newPath.string());
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "loader must reject a v3.0 module (above the ceiling)");
    CHECK(what.find("was written by a newer ncc; upgrade ncc/nvm to run it")
              != std::string::npos,
        "rejection should hit the ceiling path, got: " + what);

    //Below the floor: a v1.x module — misparses every keyed name.
    bytes[8] = 0x01;
    const auto oldPath = scratchDir() / "verbound_v112.ncu";
    {
        std::ofstream out(oldPath, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    threw = false;
    what.clear();
    try {
        ModuleLoader::Load(oldPath.string());
    } catch (const std::exception& e) {
        threw = true;
        what = e.what();
    }
    CHECK(threw, "loader must reject a v1.x module (floor is 2.0)");
    CHECK(what.find("is outdated; recompile with current ncc")
              != std::string::npos,
        "rejection should hit the floor path, got: " + what);
    PASS();
}

void test_v19_import_gc_roots()
{
    TEST(v19_import_gc_roots);
    //Pins the B.1 locals fix: before it, imported frames had no local
    //descriptors, so MarkPhase marked nothing and every imported-frame
    //heap value was swept. The array must be referenced ONLY from the
    //imported frame (a caller-frame local would root it through the
    //caller's own locals table).
    //
    //The scenario works around the sweeper's reuse dynamics: a swept
    //slot re-enters the free list only at the GC that sweeps it (the
    //rebuild skips kind==0 slots), and vector::clear() keeps the old
    //buffer, so a swept-but-never-reallocated slot still reads its old
    //value. To make the bug observable, the program drives the heap
    //past the GC threshold (1024 slots), allocates `keep`, then runs
    //exactly one more loop iteration — its back-edge GC sweeps keep
    //(no roots under the bug) together with that iteration's single
    //scratch, leaving exactly two slots in the free list. Two
    //straight-line allocations then drain the free list: the second
    //pop recycles keep's slot and assign() zeroes its data, so the
    //final read observes 0 instead of 7. With the fix, keep is marked
    //at that GC and the drain cannot touch its slot.
    //Lib must be a compiled .ncu before the consumer can import it
    //(same contract as the roundtrip test above); the consumer executes
    //through the load-time linked module.
    BuildOutcome lib = buildSource("gcutil_lib",
        "int churn() {\n"
        "    int i = 0;\n"
        "    while (i < 1100) {\n"
        "        int[] warm = new int[8];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    int[] keep = new int[1];\n"
        "    keep[0] = 7;\n"
        "    i = 0;\n"
        "    while (i < 1) {\n"
        "        int[] scratch = new int[64];\n"
        "        i = i + 1;\n"
        "    }\n"
        "    int[] drainA = new int[8];\n"
        "    int[] drainB = new int[8];\n"
        "    return keep[0] + drainA[0] + drainB[0];\n"
        "}\n");
    CHECK(lib.ok, "lib build should succeed: " + lib.diagnostics);
    BuildOutcome b = buildConsumer("gcutil_main",
        "import gcutil_lib;\n"
        "int main() { return gcutil_lib.churn(); }\n");
    CHECK(b.ok, "import build should succeed: " + b.diagnostics);
    CompiledModule mod = loadLinked("gcutil_main");
    VmExecutor exec;
    CHECK(exec.Execute(mod) == 7,
        "imported-frame local 'keep' must survive GC (B.1 locals fix)");
    PASS();
}

void run_debugger_format_tests()
{
    test_v19_sourcefile_single_tu();
    test_v19_import_roundtrip();
    test_v19_loader_rejects_v1_8();
    test_loader_rejects_v1_9();
    test_loader_rejects_v1_10();
    test_loader_rejects_v1_11();
    test_loader_rejects_minor_below_floor();
    test_loader_accepts_ceiling_and_floor();
    test_v19_import_gc_roots();
}
