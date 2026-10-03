// Per-unit codegen + nlink integration tests (phase 6 Step 3/4).
//
// Multi-file programs are compiled per translation unit
// (ModuleBuilder::BuildUnitImages -> VmBackend::GenerateUnits), linked by
// NcuLinker from the unit images (+ any loaded external .ncu images), and
// really executed by VmExecutor. Nothing is baked in at compile time: every
// cross-unit reference leaves a placeholder slot that only the linker
// resolves, so each test pins one cross-unit shape end to end (one test,
// one shape). The merged-mode equivalents live in test_library_source.cpp;
// those keep guarding the legacy path until the campaign retires it.

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "NcuLinker.h"
#include "NcuLoader.h"
#include "IHostIo.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

namespace fs = std::filesystem;
using Files = std::vector<std::pair<std::string, std::string>>;

// A fresh, isolated scenario directory under the temp tree.
fs::path scenarioDir(const char* name) {
    auto d = fs::temp_directory_path() / "nlang_test_perunit" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void writeFiles(const fs::path& dir, const Files& files) {
    for (const auto& entry : files) {
        fs::path target = dir / entry.first;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary);
        out << entry.second;
    }
}

struct CapturingIo : IHostIo {
    std::string text;
    void OnOutput(std::string_view v) override { text += v; }
};

// Result of one per-unit build (+ run when the program links).
struct PerUnitRun {
    bool ok = false;      //front-end + per-unit codegen + link succeeded
    int rc = -1;          //program exit code (valid when ok)
    std::string log;      //build diagnostics (empty tail = silent success)
    ModuleBuilder::UnitImages images;   //pre-link unit images (when ok)
};

// Build sources as per-unit images, link the closure, execute the entry.
// One build entry point for the whole file (unique output per scenario:
// the process-wide module registry keeps names across Build() runs).
PerUnitRun perUnitRun(const fs::path& dir, CapturingIo& cap,
                      const std::vector<std::string>& sources) {
    PerUnitRun out;
    BuildParams params;
    for (const auto& s : sources)
        params.m_SourceFiles.push_back((dir / s).string());
    params.m_sProjectDir = dir.string();
    params.m_sOutputModule = dir.filename().string();
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    params.m_ImportDirs = { dir.string(), STDLIB_DIR };

    ListCompileLogger logger;
    ModuleBuilder::UnitImages imgs;
    try {
        ModuleBuilder builder(params, logger);
        imgs = builder.BuildUnitImages();
    } catch (const std::exception& e) {
        out.log = std::string("internal error: ") + e.what();
        return out;
    }
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        out.log += (*it)->Message() + "\n";
    if (imgs.units.empty())
        return out;   //front-end errors are already in the log

    std::vector<CompiledModule> closure = std::move(imgs.units);
    for (auto& ext : imgs.external)
        closure.push_back(std::move(ext));
    CompiledModule linked;
    try {
        linked = NcuLinker::Link(std::move(closure), imgs.entryKey);
    } catch (const std::exception& e) {
        out.log += std::string("link error: ") + e.what();
        return out;
    }
    //Re-derive the pre-link shapes for the structural assertions: the
    //link consumed the images, so keep a copy before linking next time
    //the caller needs them (perUnitBuild below serves that role).
    VmExecutor exec;
    exec.AddNativeSearchDir(dir.string());
    exec.SetHostIo(&cap);
    try {
        out.rc = exec.Execute(linked);
    } catch (const std::exception& e) {
        out.log += std::string("runtime error: ") + e.what();
        return out;
    }
    out.ok = true;
    out.images.entryKey = imgs.entryKey;
    return out;
}

// Front-end + per-unit codegen only (no link, no run): for structural
// assertions on the unit images themselves.
struct PerUnitBuild {
    bool ok = false;
    std::string log;
    ModuleBuilder::UnitImages images;
};

PerUnitBuild perUnitBuild(const fs::path& dir,
                          const std::vector<std::string>& sources) {
    PerUnitBuild out;
    BuildParams params;
    for (const auto& s : sources)
        params.m_SourceFiles.push_back((dir / s).string());
    params.m_sProjectDir = dir.string();
    params.m_sOutputModule = dir.filename().string();
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    params.m_ImportDirs = { dir.string(), STDLIB_DIR };
    ListCompileLogger logger;
    try {
        ModuleBuilder builder(params, logger);
        out.images = builder.BuildUnitImages();
    } catch (const std::exception& e) {
        out.log = std::string("internal error: ") + e.what();
        return out;
    }
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        out.log += (*it)->Message() + "\n";
    out.ok = !out.images.units.empty();
    return out;
}

// --- shape 1: cross-unit free-function call (invoke arm) ---

void TestFreeFunctionCrossUnit() {
    const auto dir = scenarioDir("free_fn");
    writeFiles(dir, {
        { "alib.n",
          "int add(int a, int b) {\n"
          "  return a + b;\n"
          "}\n" },
        { "main.n",
          "import alib;\n"
          "int main() {\n"
          "  return alib.add(20, 3);\n"
          "}\n" } });
    CapturingIo cap;
    const PerUnitRun r = perUnitRun(dir, cap, { "main.n", "alib.n" });
    CHECK(r.ok, "cross-unit free call builds and links");
    CHECK(r.rc == 23, "cross-unit free call returns 23");
}

// --- shape 2: cross-unit class — new + ctor + direct method ---

void TestClassCtorMethodCrossUnit() {
    const auto dir = scenarioDir("class_ctor");
    writeFiles(dir, {
        { "alib.n",
          "class Calc {\n"
          "  public int base;\n"
          "  public int Calc(int b) {\n"
          "    base = b;\n"
          "    return 0;\n"
          "  }\n"
          "  public int twice() {\n"
          "    return base * 2;\n"
          "  }\n"
          "}\n" },
        { "main.n",
          "import alib;\n"
          "int main() {\n"
          "  alib.Calc c = new alib.Calc(21);\n"
          "  return c.twice();\n"
          "}\n" } });
    CapturingIo cap;
    const PerUnitRun r = perUnitRun(dir, cap, { "main.n", "alib.n" });
    CHECK(r.ok, "cross-unit class new/ctor/method builds and links");
    CHECK(r.rc == 42, "cross-unit ctor ran (21*2)");
}

// --- shape 3: cross-unit inheritance — super(args) into the parent ctor ---

void TestSuperCtorCrossUnit() {
    const auto dir = scenarioDir("super_ctor");
    writeFiles(dir, {
        { "alib.n",
          "class Base {\n"
          "  public int v;\n"
          "  public int Base(int x) {\n"
          "    v = x;\n"
          "    return 0;\n"
          "  }\n"
          "  public int get() {\n"
          "    return v + 1;\n"
          "  }\n"
          "}\n" },
        { "main.n",
          "import alib;\n"
          "class D : alib.Base {\n"
          "  public int D(int x) {\n"
          "    super(x);\n"
          "    return 0;\n"
          "  }\n"
          "}\n"
          "int main() {\n"
          "  D d = new D(40);\n"
          "  return d.get();\n"
          "}\n" } });
    CapturingIo cap;
    const PerUnitRun r = perUnitRun(dir, cap, { "main.n", "alib.n" });
    CHECK(r.ok, "cross-unit super() builds and links");
    CHECK(r.rc == 41, "parent ctor ran through super(40)");
}

// --- shape 4: cross-unit struct — value copies at decl/assign/param ---

void TestStructDeepCopyCrossUnit() {
    const auto dir = scenarioDir("struct_copy");
    writeFiles(dir, {
        { "alib.n",
          "struct Vec {\n"
          "  int a;\n"
          "  int b;\n"
          "}\n"
          "int sum(Vec v) {\n"
          "  v.a = 100;\n"   //callee mutation must not leak back
          "  return v.a + v.b;\n"
          "}\n" },
        { "main.n",
          "import alib;\n"
          "int main() {\n"
          "  alib.Vec v;\n"
          "  v.a = 3; v.b = 4;\n"
          "  if (alib.sum(v) != 104) return 1;\n"
          "  if (v.a != 3) return 2;\n"    //param deep copy isolated it
          "  alib.Vec w = v;\n"
          "  w.a = 50;\n"
          "  if (v.a != 3) return 3;\n"    //assign deep copy isolated it
          "  return 0;\n"
          "}\n" } });
    CapturingIo cap;
    const PerUnitRun r = perUnitRun(dir, cap, { "main.n", "alib.n" });
    CHECK(r.ok, "cross-unit struct copies build and link");
    CHECK(r.rc == 0, "cross-unit struct value semantics hold");
}

// --- shape 5: cross-unit enum method (enclosure-qualified function key) ---

void TestEnumMethodCrossUnit() {
    const auto dir = scenarioDir("enum_method");
    writeFiles(dir, {
        { "alib.n",
          "enum Color {\n"
          "  Red,\n"
          "  Green;\n"
          "  public int rank() { return 7; }\n"
          "}\n"
          "enum Shape {\n"
          "  Circle,\n"
          "  Square;\n"
          "  public int rank() { return 9; }\n"
          "}\n" },
        { "main.n",
          "import alib;\n"
          "int main() {\n"
          "  alib.Color c = alib.Color.Red;\n"
          "  alib.Shape s = alib.Shape.Circle;\n"
          "  return c.rank() * 10 + s.rank();\n"
          "}\n" } });   //7*10 + 9 = 79 — both same-named enum methods
    CapturingIo cap;
    const PerUnitRun r = perUnitRun(dir, cap, { "main.n", "alib.n" });
    CHECK(r.ok, "cross-unit enum method builds and links");
    CHECK(r.rc == 79, "both same-named enum methods ran (7*10+9)");
}

// --- shape 6: unit-image structure — own tables stay own, imports recorded ---

void TestUnitImageShape() {
    const auto dir = scenarioDir("shape");
    writeFiles(dir, {
        { "alib.n",
          "int add(int a, int b) {\n"
          "  return a + b;\n"
          "}\n" },
        { "main.n",
          "import alib;\n"
          "int main() {\n"
          "  return alib.add(1, 2);\n"
          "}\n" } });
    const PerUnitBuild b = perUnitBuild(dir, { "main.n", "alib.n" });
    CHECK(b.ok, "shape scenario builds");
    if (!b.ok) return;
    CHECK(b.images.units.size() == 2, "one image per translation unit");
    if (b.images.units.size() != 2) return;
    const CompiledModule& mainUnit = b.images.units[0];
    const CompiledModule& libUnit = b.images.units[1];
    CHECK(mainUnit.modulePath == "main", "entry unit carries its path");
    CHECK(libUnit.modulePath == "alib", "library unit carries its path");
    CHECK(libUnit.FindFunction("alib.add") >= 0,
          "library unit owns alib.add");
    bool libHasMain = false;
    for (const auto& fn : libUnit.functions)
        if (fn.name == "main") libHasMain = true;
    CHECK(!libHasMain, "library unit has no main record");
    bool importRecorded = false;
    for (const auto& imp : mainUnit.functionImports) {
        if (imp.modulePath == "alib" && imp.name == "alib.add"
            && imp.paramCount == 2 && imp.ownerClassKey.empty()) {
            importRecorded = true;
        }
    }
    CHECK(importRecorded,
          "consumer unit records the alib.add import slot");
    CHECK(b.images.entryKey == "main.main",
          "program entry key is the qualified name");
}

// --- shape 7: two project mains — the per-unit path must diagnose it ---

void TestTwoMainsPerUnitRejected() {
    const auto dir = scenarioDir("two_mains");
    writeFiles(dir, {
        { "alib.n",
          "int main() {\n"
          "  return 1;\n"
          "}\n" },
        { "main.n",
          "import alib;\n"
          "int main() {\n"
          "  return 0;\n"
          "}\n" } });
    const PerUnitBuild b = perUnitBuild(dir, { "main.n", "alib.n" });
    CHECK(!b.ok, "two mains rejected (per-unit)");
    //The merged-namespace front end owns this diagnosis: duplicate
    //top-level names across units conflict at resolve, before any
    //per-unit codegen — so the build fails with the duplicate-
    //definition error, not a codegen ambiguity.
    CHECK(b.log.find("conflicted") != std::string::npos,
          "diagnosis names the duplicate definition");
}

// --- shape 8: foreign-typed fields — placeholder appends mid-registration ---
//
// A unit's OWN struct/class may carry FOREIGN field types and bases. Each
// such field slots as an import placeholder, which APPENDS to the very
// table the registration loops walk — the loops must re-index per
// statement instead of holding element references (RegisterStructs pass 2,
// ResolveStructClassRefs descs, ResolveClassMetadata/ResolveClassFieldRefs).
// Pair: two foreign fields so the second write follows the first append;
// Holder: foreign base + foreign class field; ix(): an own struct with a
// foreign-typed field passed across the unit boundary.

void TestForeignTypedFieldsCrossUnit() {
    const auto dir = scenarioDir("foreign_fields");
    writeFiles(dir, {
        { "alib.n",
          "struct Inner {\n"
          "  int x;\n"
          "}\n"
          "int ix(Inner i) {\n"
          "  return i.x;\n"
          "}\n"
          "class Point {\n"
          "  public int px;\n"
          "  public int py;\n"
          "  public int Point(int x, int y) {\n"
          "    px = x;\n"
          "    py = y;\n"
          "    return 0;\n"
          "  }\n"
          "}\n"
          "class Base {\n"
          "  public int bonus;\n"
          "  public int Base() {\n"
          "    bonus = 5;\n"
          "    return 0;\n"
          "  }\n"
          "  public int extra() {\n"
          "    return bonus;\n"
          "  }\n"
          "}\n" },
        { "main.n",
          "import alib;\n"
          "struct Pair {\n"
          "  alib.Inner a;\n"
          "  alib.Inner b;\n"
          "}\n"
          "class Holder : alib.Base {\n"
          "  public alib.Point p;\n"
          "  public int Holder() {\n"
          "    super();\n"
          "    p = new alib.Point(30, 4);\n"
          "    return 0;\n"
          "  }\n"
          "}\n"
          "int main() {\n"
          "  Pair q;\n"
          "  q.a.x = 4;\n"
          "  q.b.x = 2;\n"
          "  Holder h = new Holder();\n"
          "  if (h.p.px != 30) return 1;\n"
          "  if (h.p.py != 4) return 2;\n"
          "  if (h.extra() != 5) return 3;\n"
          "  if (alib.ix(q.a) != 4) return 4;\n"
          "  return q.a.x * 10 + q.b.x;\n"
          "}\n" } });   //4*10 + 2 = 42
    CapturingIo cap;
    const PerUnitRun r = perUnitRun(dir, cap, { "main.n", "alib.n" });
    CHECK(r.ok, "foreign-typed fields build and link");
    CHECK(r.rc == 42, "foreign-typed fields resolve and copy (4*10+2)");
}

// --- shape 9: external .ncu image joins the link closure as a peer ---
//
// The library is compiled FIRST through the production artifact build
// (ModuleBuilder::BuildArtifacts — its artifact is the library's own
// unit image), then a second program imports it as an external .ncu.
// The consumer unit leaves `lib.add` as an import slot; the loaded image
// rides in UnitImages.external and resolves at link time. This is the
// whole phase 6 story end to end: compile-time separation, load-time
// closure.

void TestExternalNcuPeerImage() {
    //Stage 1: produce lib.ncu via the production path.
    const auto libDir = scenarioDir("ext_lib");
    writeFiles(libDir, {
        { "lib.n",
          "int add(int a, int b) {\n"
          "  return a + b;\n"
          "}\n" } });
    {
        BuildParams params;
        params.m_SourceFiles.push_back((libDir / "lib.n").string());
        params.m_sProjectDir = libDir.string();
        params.m_sOutputModule = "lib";
        params.m_sOutputDir = libDir.string();
        params.m_sTempDir = libDir.string();
        params.m_sStdLibDir = STDLIB_DIR;
        params.m_ImportDirs = { STDLIB_DIR };
        ListCompileLogger logger;
        ModuleBuilder builder(params, logger);
        CHECK(builder.BuildArtifacts(), "external library unit image built");
    }
    const fs::path libArtifact = libDir / "lib.ncu";
    CHECK(fs::exists(libArtifact), "library artifact written");
    //Single-file builds pack no package (design section 3: .npkg is
    //project-mode only — inlined library units must not tip a
    //single-file build into packing).
    CHECK(!fs::exists(libDir / "lib.npkg"),
          "single-file builds write no package");

    //Stage 2: consumer imports it; closure = consumer units + lib image.
    const auto appDir = scenarioDir("ext_app");
    fs::copy_file(libArtifact, appDir / "lib.ncu");
    writeFiles(appDir, {
        { "main.n",
          "import lib;\n"
          "int main() {\n"
          "  return lib.add(40, 2);\n"
          "}\n" } });
    CapturingIo cap;
    const PerUnitRun r = perUnitRun(appDir, cap, { "main.n" });
    CHECK(r.ok, "external .ncu consumer builds and links");
    CHECK(r.rc == 42, "external function ran through the link (40+2)");

    //Structural: the consumer records the cross-module call as an import
    //slot naming the external module — nothing of lib is baked in, and
    //the lib image rides along for the linker.
    const auto shapeDir = scenarioDir("ext_shape");
    fs::copy_file(libArtifact, shapeDir / "lib.ncu");
    writeFiles(shapeDir, {
        { "main.n",
          "import lib;\n"
          "int main() {\n"
          "  return lib.add(1, 2);\n"
          "}\n" } });
    const PerUnitBuild b = perUnitBuild(shapeDir, { "main.n" });
    CHECK(b.ok, "shape scenario builds");
    if (!b.ok) return;
    bool importRecorded = false;
    for (const auto& imp : b.images.units[0].functionImports)
        if (imp.modulePath == "lib" && imp.name == "lib.add"
            && imp.paramCount == 2 && imp.ownerClassKey.empty())
            importRecorded = true;
    CHECK(importRecorded, "consumer records the lib.add import slot");
    CHECK(!b.images.external.empty(),
          "loaded lib image rides in external");
    //The placeholder slot record itself is named "lib.add" (that is its
    //job); "baked in" means a record with that name carries LIBRARY
    //BYTECODE — which would mean codegen copied the body over.
    bool bakedIn = false;
    for (const auto& fn : b.images.units[0].functions)
        if (fn.name == "lib.add" && !fn.bytecode.empty())
            bakedIn = true;
    CHECK(!bakedIn, "consumer unit contains no lib code");
}

// --- shape 10: foreign methods compile only into their owning unit ---
//
// Class/enum METHODS carry no owner tag (TagUnitMembers tags namespace
// members), so a flat owner lookup saw NO_OWNER and IsOwnUnit's fallback
// registered — and fully bytecode-compiled — every foreign method into
// EVERY unit image (ghost registration). Calls then bound to the local
// ghost, so nlink's owner-keyed import validation never ran for methods
// or constructors. This shape pins the per-unit invariant: the consumer
// image holds no foreign bodies, and the cross-unit ctor/method/enum-
// method calls are real import records.
void TestForeignMethodsNotGhostCompiled() {
    const Files payload = {
        { "alib.n",
          "class Calc {\n"
          "  public int base;\n"
          "  public int Calc(int b) {\n"
          "    base = b;\n"
          "    return 0;\n"
          "  }\n"
          "  public int twice() {\n"
          "    return base * 2;\n"
          "  }\n"
          "}\n"
          "enum Color {\n"
          "  Red,\n"
          "  Green;\n"
          "  public int rank() { return 7; }\n"
          "}\n" },
        { "main.n",
          "import alib;\n"
          "int main() {\n"
          "  alib.Calc c = new alib.Calc(21);\n"
          "  alib.Color col = alib.Color.Red;\n"
          "  return c.twice() + col.rank();\n"
          "}\n" } };   //42 + 7 = 49
    const auto dir = scenarioDir("no_ghost");
    writeFiles(dir, payload);
    const PerUnitBuild b = perUnitBuild(dir, { "main.n", "alib.n" });
    CHECK(b.ok, "no-ghost scenario builds");
    if (!b.ok) return;
    bool ghostMethod = false, ghostEnumMethod = false;
    for (const auto& fn : b.images.units[0].functions) {
        if (fn.name == "twice" && !fn.bytecode.empty())
            ghostMethod = true;
        if (fn.name == "rank" && !fn.bytecode.empty())
            ghostEnumMethod = true;
    }
    CHECK(!ghostMethod, "consumer image holds no foreign method body");
    CHECK(!ghostEnumMethod,
          "consumer image holds no foreign enum-method body");
    bool ctorImport = false, methodImport = false, enumMethodImport = false;
    for (const auto& imp : b.images.units[0].functionImports) {
        if (imp.modulePath == "alib" && imp.name == "Calc"
            && imp.paramCount == 2 && imp.ownerClassKey == "alib.Calc")
            ctorImport = true;   //2 = formal + this, table convention
        if (imp.modulePath == "alib" && imp.name == "twice"
            && imp.paramCount == 1 && imp.ownerClassKey == "alib.Calc")
            methodImport = true;
        if (imp.modulePath == "alib" && imp.name == "alib.Color.rank"
            && imp.paramCount == 1 && imp.ownerClassKey.empty())
            enumMethodImport = true;
    }
    CHECK(ctorImport, "cross-unit ctor call is an import slot");
    CHECK(methodImport, "cross-unit method call is an import slot");
    CHECK(enumMethodImport,
          "cross-unit enum-method call is an import slot");
    //Fresh scenario for the run: the process-wide runtime ModuleManager
    //keeps module names across builds, so a second build of the same
    //module name would be rejected ("already exists").
    const auto runDir = scenarioDir("no_ghost_run");
    writeFiles(runDir, payload);
    CapturingIo cap;
    const PerUnitRun r = perUnitRun(runDir, cap, { "main.n", "alib.n" });
    CHECK(r.ok, "import-resolved methods link");
    CHECK(r.rc == 49, "ctor/method/enum-method imports run (42+7)");
}

// --- shape 11: cross-unit enum -> string keeps the value name ---
//
// The three enum-to-string emit sites read m_enumIndexMap directly; a
// foreign enum missed the map and the sites either degraded to the
// numeric int->string spelling (cast site) or emitted nothing while
// claiming success (member sites — raw int32 left where a string
// handle belongs). Routing through EnumSlotFor leaves a placeholder
// the linker resolves; toString then prints the value name ("Red").
void TestCrossUnitEnumToString() {
    const auto dir = scenarioDir("enum_tostring");
    writeFiles(dir, {
        { "alib.n",
          "enum Color {\n"
          "  Red,\n"
          "  Green;\n"
          "}\n" },
        { "main.n",
          "import alib;\n"
          "int main() {\n"
          "  alib.Color c = alib.Color.Red;\n"
          "  string s = c.toString();\n"
          "  if (s != \"Red\") return 1;\n"
          "  s = alib.Color.Green.toString();\n"
          "  if (s != \"Green\") return 2;\n"
          "  string t = \"v=\" + c;\n"
          "  if (t != \"v=Red\") return 3;\n"
          "  return 0;\n"
          "}\n" } });
    CapturingIo cap;
    const PerUnitRun r = perUnitRun(dir, cap, { "main.n", "alib.n" });
    CHECK(r.ok, "cross-unit enum toString builds and links");
    CHECK(r.rc == 0,
          "enum toString variable/literal/concat all keep value names");
}

// --- shape 12: project package artifact — identity and entry naming ---
//
// A multi-unit project build packs the .npkg as its ONLY artifact (no
// linked .ncu). Every member is a unit image named by its own module
// path, and the entry record must name the REAL entry unit: the
// runtime loader's identity rule refuses a member whose header names
// anything else, and the record's <module>.<function> override must
// hit a function that unit owns. Pinned with the entry unit listed
// SECOND — a library unit leading the source list must not claim the
// entry — by driving the package through NcuLoader + nlink +
// execution, exactly as nvm runs a program package from the command
// line.
void TestProjectPackageArtifactIdentity() {
    const auto dir = scenarioDir("pkg_artifact");
    writeFiles(dir, {
        { "helper.n",
          "int helper(int x) {\n"
          "  return x * 2;\n"
          "}\n" },
        { "main.n",
          "import helper;\n"
          "int main() {\n"
          "  return helper.helper(21);\n"
          "}\n" } });
    BuildParams params;
    params.m_SourceFiles.push_back((dir / "helper.n").string());
    params.m_SourceFiles.push_back((dir / "main.n").string());
    params.m_sProjectDir = dir.string();
    params.m_bProjectMode = true;   //project mode ships the package
    params.m_sOutputModule = "prog";
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    params.m_ImportDirs = { dir.string(), STDLIB_DIR };
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    CHECK(builder.BuildArtifacts(), "project packs the package");
    const fs::path pkg = dir / "prog.npkg";
    CHECK(fs::exists(pkg), "package artifact written");
    //Project mode writes ONLY the package — linking is the loader's job
    //at run time (design section 3: no linked .ncu beside it).
    CHECK(!fs::exists(dir / "prog.ncu"),
          "project builds write no linked .ncu");

    NcuLoader::Options loaderOpts;
    NcuLoader::Result loaded;
    try {
        loaded = NcuLoader::LoadClosure(pkg.string(), loaderOpts);
    } catch (const std::exception& e) {
        CHECK(false, (std::string("package load failed: ") + e.what()).c_str());
        return;
    }
    CHECK(loaded.units.size() == 2, "one member per unit, both loaded");
    CHECK(loaded.units[0].modulePath == "main",
          "entry record selects the entry unit first");
    CHECK(loaded.units[1].modulePath == "helper",
          "own member table yields the library unit");
    CHECK(loaded.entryKey == "main.main",
          "entry record names the real entry function");
    CompiledModule linked;
    try {
        linked = NcuLinker::Link(std::move(loaded.units), loaded.entryKey);
    } catch (const std::exception& e) {
        CHECK(false, (std::string("link failed: ") + e.what()).c_str());
        return;
    }
    CapturingIo cap;
    VmExecutor exec;
    exec.SetHostIo(&cap);
    CHECK(exec.Execute(linked) == 42, "package entry runs (21*2)");
}

} // namespace

int main() {
    Runtime::StaticInit();
    std::fprintf(stderr, "=== Per-Unit Link Integration Tests ===\n");
    TestFreeFunctionCrossUnit();
    TestClassCtorMethodCrossUnit();
    TestSuperCtorCrossUnit();
    TestStructDeepCopyCrossUnit();
    TestEnumMethodCrossUnit();
    TestUnitImageShape();
    TestTwoMainsPerUnitRejected();
    TestForeignTypedFieldsCrossUnit();
    TestExternalNcuPeerImage();
    TestForeignMethodsNotGhostCompiled();
    TestCrossUnitEnumToString();
    TestProjectPackageArtifactIdentity();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
