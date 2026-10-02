// Source-library integration tests. A third-party library shipped as a
// single <name>.n file with ORDINARY NLang function bodies is compiled
// inline into the consumer and really run by VmExecutor.
//
// Coverage (Phase 4a):
//   * happy path  - ordinary bodies, same-library bare calls, recursion;
//   * visibility  - an unimported library call is rejected;
//   * no bare names - import never injects the callee name unqualified;
//   * no sibling auto-visibility - a library TU does not see other files
//     in its directory unless it imports them (the D7 rule is project-only);
//   * owner isolation - a project function and a library function sharing
//     a name resolve independently.
//
// The library is really compiled by ModuleBuilder and really executed by
// VmExecutor (no VM internals mocked); it resolves through the same
// library-index + inline path as the standard library.

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "IHostIo.h"
#include "ModuleLoader.h"

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
    auto d = fs::temp_directory_path() / "nlang_test_source_lib" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void writeFiles(const fs::path& dir, const Files& files) {
    for (const auto& entry : files) {
        fs::path target = dir / entry.first;
        fs::create_directories(target.parent_path());   //nested fixtures need parents
        std::ofstream out(target, std::ios::binary);
        out << entry.second;
    }
}

//One build entry point for the whole file: fills params, runs Build(),
//and leaves the diagnostics in the caller's logger. Never a second copy.
bool runBuild(const fs::path& dir, ListCompileLogger& logger,
              const std::vector<std::string>& vrSources = { "main.n" }) {
    BuildParams params;
    //Unique output module per scenario: several Build() runs share the
    //process-wide module registry, so a repeated name would collide.
    for (const auto& s : vrSources) params.m_SourceFiles.push_back((dir / s).string());
    //The scenario root IS the project root: without it DeriveModulePath
    //falls back to the file stem for every source, so two nested main.n
    //files would collapse to one bare pool and never reach the entry scan.
    params.m_sProjectDir = dir.string();
    params.m_sOutputModule = dir.filename().string();
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    params.m_ImportDirs = { dir.string(), STDLIB_DIR };
    return ModuleBuilder(params, logger).Build();
}

//Text view: joins every logged item. ListCompileLogger only exposes
//cbegin()/cend() and stores CompileLogItem* (Logger.h:97-119).
std::string compileLog(const fs::path& dir,
                       const std::vector<std::string>& vrSources = { "main.n" }) {
    ListCompileLogger logger;
    std::string out;
    try { runBuild(dir, logger, vrSources); }
    catch (const std::exception& e) {      //kept because today's compileDir
        out += " internal error: ";        //already has this catch
        out += e.what();
        return out;
    }
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        out += (*it)->Message() + "\n";
    return out;
}

// Compile main.n in dir with dir + the stdlib dir on the import path.
// Returns the builder result (false = compile errors diagnosed).
bool compileDir(const fs::path& dir,
                const std::vector<std::string>& vrSources = { "main.n" }) {
    ListCompileLogger logger;
    try { return runBuild(dir, logger, vrSources); }
    catch (const std::exception&) { return false; }
}

struct CapturingIo : IHostIo {
    std::string text;
    void OnOutput(std::string_view v) override { text += v; }
};

// Compile then run main.n; returns the program code (-1 on build failure).
int compileRun(const fs::path& dir, CapturingIo& cap,
               const std::vector<std::string>& vrSources = { "main.n" }) {
    ListCompileLogger logger;
    if (!runBuild(dir, logger, vrSources))
        return -1;
    const std::string modName = dir.filename().string();
    //A keyed-layout symptom (or a VM-side table break) surfaces as an
    //exception, not a diagnostic; catch it so the scenario reports a
    //failed CHECK instead of taking down the whole binary (compileDir
    //protects its own call the same way).
    try {
        CompiledModule mod = ModuleLoader::Load(
            (dir / (modName + ".ncu")).string());
        VmExecutor exec;
        exec.AddNativeSearchDir(dir.string());
        exec.SetHostIo(&cap);
        return exec.Execute(mod);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "internal error: %s\n", e.what());
        return -1;
    }
}

// Ordinary NLang functions: same-library bare calls (quad -> twice) and
// recursion (fact). No native keyword, no DLL - every body is compiled.
const char* kLibSource =
    "int twice(int x) {\n"
    "  return x * 2;\n"
    "}\n"
    "int quad(int x) {\n"
    "  return twice(twice(x));\n"
    "}\n"
    "int fact(int n) {\n"
    "  if (n <= 1) return 1;\n"
    "  return n * fact(n - 1);\n"
    "}\n";

const char* kHappyProgram =
    "import io;\n"
    "import alib;\n"
    "int main() {\n"
    "  if (alib.twice(5) != 10) return 1;\n"
    "  if (alib.quad(3) != 12) return 2;\n"
    "  if (alib.fact(5) != 120) return 3;\n"
    "  io.print(\"ok\");\n"
    "  return 0;\n"
    "}\n";

void TestHappyPath() {
    const auto dir = scenarioDir("happy");
    writeFiles(dir, { { "alib.n", kLibSource }, { "main.n", kHappyProgram } });
    CapturingIo cap;
    int rc = compileRun(dir, cap);
    CHECK(rc == 0, "source-library program self-checks (rc)");
    CHECK(cap.text == "ok\n", "source-library program output");
}

// A library the program never imports cannot be reached by qualification.
void TestUnimportedRejected() {
    const auto dir = scenarioDir("unimported");
    const char* lib = "int f() { return 1; }\n";
    const char* prog = "int main() {\n  return alib.f();\n}\n";
    writeFiles(dir, { { "alib.n", lib }, { "main.n", prog } });
    CHECK(!compileDir(dir), "unimported library call is rejected");
}

// A library file in the consumer's directory shares the directory's bare
// pool (D7): once `import alib;` pulls the file into the build, the bare
// f() resolves alongside the qualified form. (The pre-shell pin — "import
// injects no bare names" — held only while the shell kept the members out
// of every directory pool; a library NOT in the build stays
// bare-unreachable, which TestUnimportedRejected pins from the other
// side. Task 6's dotted imports restore a cross-directory variant.)
void TestNoBareNames() {
    const auto dir = scenarioDir("bare");
    const char* lib = "int f() { return 1; }\n";
    const char* prog = "import alib;\nint main() {\n  return f();\n}\n";
    writeFiles(dir, { { "alib.n", lib }, { "main.n", prog } });
    CHECK(compileDir(dir),
          "a same-directory imported library joins the bare pool (D7)");
}

// A library TU has no same-directory auto-visibility (the project-only D7
// rule): alib.g may not call blib.h unless alib itself imports blib, even
// though the consumer imports both (so both packages are inlined).
void TestNoSiblingAutoVisibility() {
    const auto dir = scenarioDir("sibling");
    const char* blib = "int h() { return 5; }\n";
    const char* alib =
        "int g() { return blib.h(); }\n";
    const char* prog =
        "import alib;\nimport blib;\n"
        "int main() {\n  return alib.g();\n}\n";
    writeFiles(dir, { { "blib.n", blib }, { "alib.n", alib },
                      { "main.n", prog } });
    CHECK(!compileDir(dir),
          "library TU has no same-directory auto-visibility");
}

// A project free function and a same-name library function in the SAME
// directory are one bare pool (D7) — same signature, so the duplicate
// rule rejects the pair once the import pulls the file in. (The pre-shell
// pin — "owner tagging binds them independently" — held only while the
// shell kept the library's members out of the pool; cross-directory owner
// isolation is pinned by test_module_import's moduleFunctionsProjectBranch,
// and Task 6's dotted imports restore a library variant.)
void TestOwnerIsolation() {
    const auto dir = scenarioDir("owner");
    const char* lib = "int val() { return 7; }\n";
    const char* prog =
        "import alib;\n"
        "int val() { return 9; }\n"
        "int main() { return 0; }\n";
    writeFiles(dir, { { "alib.n", lib }, { "main.n", prog } });
    CHECK(!compileDir(dir),
          "same-directory project/library same-name functions conflict");
}

// Phase 4b-2: a library may define its own TYPES - a class with a ctor and
// methods, an enum, and an NLang factory that returns the class. The consumer
// instantiates the class, reads a field, calls a method, uses an enum value,
// and calls the factory. This exercises two fixes:
//   * the recursive codegen traversal - root -> class -> method is
//     THREE levels for a library type; the old fixed two-level walk
//     never registered or compiled a method nested inside a packaged
//     class ("call to method without a body");
//   * the package-qualified receiver context fix - `alib.Color.Green`
//     enum-value access must resolve Color through alib's package
//     entry, not the meta SnType singleton.
const char* kTypeLibSource =
    "class Point {\n"
    "  public int x;\n"
    "  public int y;\n"
    "  public int Point(int x, int y) {\n"
    "    this.x = x;\n"
    "    this.y = y;\n"
    "    return 0;\n"
    "  }\n"
    "  public int manhattan() {\n"
    "    return x + y;\n"
    "  }\n"
    "}\n"
    "enum Color { Red = 0, Green = 1, Blue = 2 }\n"
    "Point makePoint(int x, int y) {\n"
    "  return new Point(x, y);\n"
    "}\n";

const char* kTypeProgram =
    "import io;\n"
    "import alib;\n"
    "int main() {\n"
    "  alib.Point p = new alib.Point(2, 5);\n"
    "  if (p.manhattan() != 7) return 1;\n"
    "  if (p.x != 2) return 2;\n"
    "  alib.Point q = alib.makePoint(3, 4);\n"
    "  if (q.manhattan() != 7) return 3;\n"
    "  alib.Color c = alib.Color.Green;\n"
    "  if (c != alib.Color.Green) return 4;\n"
    "  io.print(\"ok\");\n"
    "  return 0;\n"
    "}\n";

void TestLibraryDefinedTypes() {
    const auto dir = scenarioDir("types");
    writeFiles(dir, { { "alib.n", kTypeLibSource },
                      { "main.n", kTypeProgram } });
    CapturingIo cap;
    int rc = compileRun(dir, cap);
    CHECK(rc == 0, "library-defined class/enum/factory (rc)");
    CHECK(cap.text == "ok\n", "library-defined types program output");
}

// Phase 4b-2 (inheritance): a library value struct plus a class hierarchy
// with virtual methods. The consumer uses the struct, upcasts a derived
// instance to the library base type, and relies on virtual dispatch to the
// override. This pins two fixes:
//   * the class-base pre-pass - the user TU is merged BEFORE the library TU,
//     so the upcast in main is visited before the library class's super
//     chain would otherwise exist and was wrongly rejected ("Incompatible
//     type"); bases are now resolved for every class before any body runs;
//   * bare method names for every class, packaged or not - a method's
//     VM name is never package-qualified, so `legs`/`kind` resolve.
const char* kInheritLibSource =
    "struct Vec {\n"
    "  int a;\n"
    "  int b;\n"
    "}\n"
    "class Animal {\n"
    "  public virtual int legs() { return 0; }\n"
    "  public virtual string kind() { return \"animal\"; }\n"
    "}\n"
    "class Dog : Animal {\n"
    "  public int legs() { return 4; }\n"
    "  public string kind() { return \"dog\"; }\n"
    "}\n";

const char* kInheritProgram =
    "import io;\n"
    "import alib;\n"
    "int main() {\n"
    "  alib.Vec v;\n"
    "  v.a = 3; v.b = 4;\n"
    "  if (v.a + v.b != 7) return 1;\n"
    "  alib.Dog d = new alib.Dog();\n"
    "  alib.Animal a = d;\n"
    "  if (a.legs() != 4) return 2;\n"
    "  if (a.kind() != \"dog\") return 3;\n"
    "  io.print(\"ok\");\n"
    "  return 0;\n"
    "}\n";

void TestLibraryInheritance() {
    const auto dir = scenarioDir("inherit");
    writeFiles(dir, { { "alib.n", kInheritLibSource },
                      { "main.n", kInheritProgram } });
    CapturingIo cap;
    int rc = compileRun(dir, cap);
    CHECK(rc == 0, "library struct + inheritance/virtual dispatch (rc)");
    CHECK(cap.text == "ok\n", "library inheritance program output");
}

// Phase 4b-2 (interface): a library declares an interface and a class that
// implements it. The consumer upcasts the instance to the library interface
// type and calls through it; the call resolves to the interface method and
// dispatches virtually to the implementing class. Interface method
// declarations carry `public` and end with ';' (no body).
const char* kIfaceLibSource =
    "interface IShape {\n"
    "  public int area();\n"
    "}\n"
    "class Square implements IShape {\n"
    "  public int side;\n"
    "  public int Square(int s) {\n"
    "    this.side = s;\n"
    "    return 0;\n"
    "  }\n"
    "  public int area() {\n"
    "    return side * side;\n"
    "  }\n"
    "}\n";

const char* kIfaceProgram =
    "import io;\n"
    "import alib;\n"
    "int main() {\n"
    "  alib.Square s = new alib.Square(3);\n"
    "  alib.IShape sh = s;\n"
    "  if (sh.area() != 9) return 1;\n"
    "  io.print(\"ok\");\n"
    "  return 0;\n"
    "}\n";

void TestLibraryInterface() {
    const auto dir = scenarioDir("iface");
    writeFiles(dir, { { "alib.n", kIfaceLibSource },
                      { "main.n", kIfaceProgram } });
    CapturingIo cap;
    int rc = compileRun(dir, cap);
    CHECK(rc == 0, "library interface implementation/dispatch (rc)");
    CHECK(cap.text == "ok\n", "library interface program output");
}

// Phase 4b-2 (enum methods): a library enum may declare methods (this is the
// enum's int value). The consumer calls one through an enum-typed variable.
// This pins the enum branches of the recursive traversal - enum value
// members are not a container; only its Methods() are visited/compiled.
const char* kEnumMethodLibSource =
    "enum Rank {\n"
    "  Low = 0, High = 1;\n"
    "  public int doubled() {\n"
    "    return this * 2;\n"
    "  }\n"
    "}\n";

const char* kEnumMethodProgram =
    "import io;\n"
    "import alib;\n"
    "int main() {\n"
    "  alib.Rank r = alib.Rank.High;\n"
    "  if (r.doubled() != 2) return 1;\n"
    "  io.print(\"ok\");\n"
    "  return 0;\n"
    "}\n";

void TestLibraryEnumMethod() {
    const auto dir = scenarioDir("enum_method");
    writeFiles(dir, { { "alib.n", kEnumMethodLibSource },
                      { "main.n", kEnumMethodProgram } });
    CapturingIo cap;
    int rc = compileRun(dir, cap);
    CHECK(rc == 0, "library enum method dispatch (rc)");
    CHECK(cap.text == "ok\n", "library enum method program output");
}

// Every library unit is a bare source file now (the `namespace` wrapper
// syntax is gone): members sit at the root, exactly like a project
// module. Its type must still be nameable as `unit.Type` - the owner
// rule is one rule, shared with the qualified-call side (which already
// resolved other.total).
const char* kRootModuleSource =
    "struct Box {\n"
    "  int a;\n"
    "  int b;\n"
    "}\n"
    "int total(Box x) {\n"
    "  return x.a + x.b;\n"
    "}\n";

const char* kRootModuleProgram =
    "import io;\n"
    "import other;\n"
    "int main() {\n"
    "  other.Box b;\n"
    "  b.a = 2;\n"
    "  b.b = 5;\n"
    "  io.print(other.total(b));\n"
    "  return 0;\n"
    "}\n";

void TestModuleTypeWithoutNamespaceWrapper() {
    const auto dir = scenarioDir("root_module_type");
    writeFiles(dir, { { "other.n", kRootModuleSource },
                      { "main.n", kRootModuleProgram } });
    CapturingIo cap;
    int rc = compileRun(dir, cap);
    CHECK(rc == 0, "qualified type of a wrapper-less unit (rc)");
    CHECK(cap.text == "7\n", "wrapper-less unit type program output");
}

// The same container holds the CONSUMER's own root types, so a container
// lookup without an owner check would bind `other.Box` to main's Box. The
// owner tag is what keeps one unit's qualification inside that unit.
void TestModuleTypeOwnerIsolation() {
    const auto dir = scenarioDir("root_module_owner");
    const char* other = "int seven() {\n  return 7;\n}\n";
    const char* prog =
        "import other;\n"
        "struct Box {\n"
        "  int a;\n"
        "}\n"
        "int main() {\n"
        "  other.Box b;\n"
        "  b.a = 1;\n"
        "  return 0;\n"
        "}\n";
    writeFiles(dir, { { "other.n", other }, { "main.n", prog } });
    CHECK(!compileDir(dir),
          "a foreign unit cannot borrow the consumer's root type");
}

//Phase 5 audit I2 control: a member access in EXPRESSION position must stay
//legal. The Step 3 guard only rejects a non-dotted chain in TYPE-head
//position — if this case goes red, the guard widened too far.
//The malformed-chain negatives are deliberately NOT here: they are
//undefined behaviour on current dev (blind static_cast in
//CollectQualifiedSegments, nlang.y:160-172) and are pinned as the e2e
//subprocess cases qhead_call / qhead_deep / qhead_index instead, so a crash can take out
//one e2e entry rather than this whole binary.
static void TestQualifiedTypeHeadKeepsLegalMemberAccess() {
    auto dir3 = scenarioDir("qhead_ok");
    writeFiles(dir3, { { "alib.n", kLibSource }, { "main.n",
        "import alib;\n"
        "int main() {\n"
        "  return alib.twice(1) - 2;\n"
        "}\n" } });
    CHECK(compileDir(dir3), "member access in expression stays legal");
}

//Phase 5 audit I4: `class D : alib.B` and `x as alib.B` were NameExpr-only,
//so a library type could not be a base class or a cast target.
static void TestQualifiedBaseAndCast() {
    auto dir = scenarioDir("qbase");
    writeFiles(dir, {
        { "alib.n",
          "class B {\n"
          "  public int v;\n"
          "  public int get() { return v + 1; }\n"
          "}\n" },
        { "main.n",
          "import io;\n"
          "import alib;\n"
          "class D : alib.B {\n"
          "  public int bump() { v = v + 10; return this.get(); }\n"
          "}\n"
          "int main() {\n"
          "  D d = new D();\n"
          "  alib.B b = d as alib.B;\n"
          "  io.print(b.get());\n"
          "  return d.bump() - 11;\n"
          "}\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0, "qualified base + cast compile/run");
    CHECK(cap.text == "1\n", "inherited method through qualified base");
}

//Phase 5 §11: a three-segment type in the ':' position must reach the
//resolver, not the parser. `a.b` is not importable in this phase (dotted
//library imports land in Task 6), so the reachable, stable diagnosis is
//the unimported-module one naming the exact intermediate path.
static void TestThreeSegmentBaseGivesNamedDiagnosis() {
    auto dir = scenarioDir("qbase_three_seg");
    writeFiles(dir, { { "main.n",
        "class D : a.b.C {\n  int z;\n}\n"
        "int main() { D d; return 0; }\n" } });
    const std::string log = compileLog(dir);
    CHECK(log.find("a.b") != std::string::npos,
          "the diagnosis names the exact package path a.b");
    CHECK(log.find("syntax error") == std::string::npos,
          "the ':' slot accepts a dotted type (else Step 3 did not land)");
}

//(§4-10 负例) D9：限定类型上写泛型实参今天得到裸语法错误。本阶段只钉「不误接受」，
//指名文案是阶段 7 的缺口——所以断言吃的是实测到的那句，不是愿望。
static void TestQualifiedGenericArgStaysRejected() {
    const Files files = {
        { "alib.n", "struct Vec { int x; }\n" },
        { "main.n", "import alib;\nint main() {\n"
          "  alib.Vec<int> v;\n"
          "  return 0;\n}\n" } };
    auto dir = scenarioDir("qgen_neg");
    writeFiles(dir, files);
    CHECK(!compileDir(dir), "a generic argument on a qualified type is rejected");
    //runBuild is one-build-per-output-name (the process-wide module registry
    //keeps the output module even after a failed build), so the log view
    //rebuilds the same sources in a twin scenario dir.
    auto dirLog = scenarioDir("qgen_neg_log");
    writeFiles(dirLog, files);
    const std::string log = compileLog(dirLog);
    CHECK(log.find("syntax error") != std::string::npos,
          "the measured rejection is the grammar one (D9 deferred wording)");
    CHECK(log.find("Compiler internal error") == std::string::npos,
          "and it is a diagnostic, not an internal failure");
}

//(§4-10 正例) 同一份库源，不带实参的限定拼写要能声明、能读写字段；
//内建泛型的擦除键（"List"/"Dict"）在同一条程序里一起用，钉住「限定面没有动到内建」。
static void TestQualifiedLibraryTypeWithoutArgsWorks() {
    auto dir = scenarioDir("qgen_pos");
    writeFiles(dir, {
        { "alib.n", "struct Vec { int x; }\n" },
        { "main.n", "import alib;\nint main() {\n"
          "  alib.Vec v; v.x = 2;\n"
          "  List<int> nums;\n"
          "  return v.x - 2;\n"
          "}\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0,
          "alib.Vec (no args) compiles and runs beside a builtin generic");
}

//Phase 4b-2 audit I3 (reproduced in temp/rb3): with bare-name keys, a
//library Point and a root Point collided and the later one silently got
//the first one's field layout. The shell syntax is gone now, so the pair
//cannot coexist at all: both declarations land on the merged root under
//one name, and the duplicate-class rule (spec 5.5 name-equality) rejects
//the build before any layout question can arise. The key-isolation pin
//this scenario used to carry lives on in TestBuiltinAndUserTypeNameCoexist
//(builtin bare key vs alib.Object) and the entry/stream-key pins.
static void TestSameNameLibraryAndRootTypeIsolated() {
    auto dir = scenarioDir("rb3_named");
    writeFiles(dir, {
        { "alib.n",
          "class Point { public int x; public int y;\n"
          "  public int sum() { return x + y; } }\n" },
        { "main.n",
          "import io;\n"
          "import alib;\n"
          "class Point { public int a; public int b; public int c;\n"
          "  public int id() { return a + b + c; } }\n"
          "int main() {\n"
          "  alib.Point p = new alib.Point();\n"
          "  p.x = 1; p.y = 2;\n"
          "  io.print(p.sum());\n"
          "  return p.sum() - 3;\n"
          "}\n" } });
    CHECK(!compileDir(dir),
          "a library Point and a root Point are a duplicate-class error");
}

//(1) D8 anchor: the package name has exactly one source — the file's
//path. The shell-name relationship this test used to pin ("path beats
//shell") is no longer expressible: `namespace` shells cannot be written
//at all, so there is nothing for the path to "beat". The positive form
//(a bare library source reached as `zlib.twice`) stays as the pin.
static void TestPathBeatsShellName() {
    auto dir = scenarioDir("path_beats_shell");
    writeFiles(dir, {
        { "zlib.n",
          "int twice(int x) { return x * 2; }\n" },
        { "main.n",
          "import zlib;\n"
          "int main() { return zlib.twice(3) - 6; }\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0, "bare zlib.n builds and resolves");
    //(1b) reverse: qualifying with a name that is not an imported package.
    //`alib` is neither a path-derived package nor an import here, so the
    //call must be rejected as not-imported. (Pre-Task 5 this resolved
    //through the legacy `namespace` container; the shell syntax is gone,
    //so the container route is gone with it and the rejection is total.)
    auto dir2 = scenarioDir("path_beats_shell_neg");
    writeFiles(dir2, { { "zlib.n",
          "int twice(int x) { return x * 2; }\n" },
        { "main.n", "import zlib;\n"
          "int main() { return alib.twice(3); }\n" } });
    CHECK(!compileDir(dir2), "the shell name is not a use-site qualifier");
    //Twin dir for the log: one build per output module name (the
    //process-global registry refuses a second Create of the same name),
    //and the failed build above already registered this scenario's name.
    auto dir2Log = scenarioDir("path_beats_shell_neg_log");
    writeFiles(dir2Log, { { "zlib.n",
          "int twice(int x) { return x * 2; }\n" },
        { "main.n", "import zlib;\n"
          "int main() { return alib.twice(3); }\n" } });
    CHECK(compileLog(dir2Log).find("Cannot resolve the field: alib")
              != std::string::npos,
          "a name that is no package and no import resolves as nothing");
}

//(2) Design §6 second rule: two same-named types in one package are a
//diagnostic, not a silent first-wins. The reachable check is the
//compiler's DuplicateFieldChecker (measured today's wording).
static void TestDuplicateTypeInOnePackageRejected() {
    auto dir = scenarioDir("dup_type");
    writeFiles(dir, { { "main.n",
        "class Box { public int a; }\n"
        "struct Box { int b; }\n"      //same unit, same name -> collision
        "int main() { return 0; }\n" } });
    CHECK(!compileDir(dir), "two same-named types in one package fail");
    auto dirLog = scenarioDir("dup_type_log");   //twin: log after a failed build
    writeFiles(dirLog, { { "main.n",
        "class Box { public int a; }\n"
        "struct Box { int b; }\n"
        "int main() { return 0; }\n" } });
    CHECK(compileLog(dirLog).find("is conflicted with a exist field definition")
              != std::string::npos,
          "the reachable duplicate-type diagnostic still fires (measured today)");
    //(2b) cross-package same names used to be legal — the shell kept the
    //declarations apart. Both land on the merged root now, so the
    //duplicate-class rule (spec 5.5 name-equality) rejects the pair
    //regardless of package.
    auto dir2 = scenarioDir("dup_type_ok");
    writeFiles(dir2, { { "alib.n", "struct Box { int a; }\n" },
        { "main.n", "import alib;\nstruct Box { int b; }\n"
          "int main() { return 0; }\n" } });
    CHECK(!compileDir(dir2),
          "same type name across packages is a duplicate-class error now");
}

//(3) D10: an ambiguous literal type name must be diagnosed; a unique hit
//must be rewritten to the table key the VM sees.
static void TestStreamLiteralResolvesAtCompileTime() {
    //(3a) unique hit: S lives in alib; the constant pool carries the
    //table key alib.S, never the bare name.
    auto dir = scenarioDir("stream_literal_one");
    writeFiles(dir, {
        { "alib.n", "struct S { int a; }\n" },
        { "main.n", "import alib;\n"
          "int main() {\n"
          "  ByteStream bs = new ByteStream();\n"
          "  alib.S w; w.a = 1; bs.writeStruct(w); bs.reset();\n"
          "  alib.S r = bs.readStruct(\"S\");\n"
          "  return r.a - 1;\n"
          "}\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0, "unique readStruct(\"S\") round-trips");
    CompiledModule mod = ModuleLoader::Load(
        (dir / (dir.filename().string() + ".ncu")).string());
    bool keyed = false;
    for (const auto& s : mod.stringConstants)
        if (s == "alib.S") keyed = true;
    CHECK(keyed, "the literal that reaches the VM is the qualified table key");
    bool bare = false;
    for (const auto& s : mod.stringConstants)
        if (s == "S") bare = true;
    CHECK(!bare, "no bare-name type literal survives");
    //(3b) two visible same-named types => ambiguity diagnostic, not a
    //silent first-wins (Step 9's hits>1 branch). Same body as (3a); the
    //only difference is the extra import, so the pair is a real control.
    auto dir2 = scenarioDir("stream_literal_two");
    writeFiles(dir2, {
        { "alib.n", "struct S { int a; }\n" },
        { "blib.n", "struct S { int b; }\n" },
        { "main.n", "import alib;\nimport blib;\n"
          "int main() {\n"
          "  ByteStream bs = new ByteStream();\n"
          "  alib.S w; w.a = 1; bs.writeStruct(w); bs.reset();\n"
          "  alib.S r = bs.readStruct(\"S\");\n"
          "  return r.a - 1;\n"
          "}\n" } });
    CHECK(!compileDir(dir2), "two visible types named S fail");
    auto dir2Log = scenarioDir("stream_literal_two_log");   //twin for the log
    writeFiles(dir2Log, {
        { "alib.n", "struct S { int a; }\n" },
        { "blib.n", "struct S { int b; }\n" },
        { "main.n", "import alib;\nimport blib;\n"
          "int main() {\n"
          "  ByteStream bs = new ByteStream();\n"
          "  alib.S w; w.a = 1; bs.writeStruct(w); bs.reset();\n"
          "  alib.S r = bs.readStruct(\"S\");\n"
          "  return r.a - 1;\n"
          "}\n" } });
    CHECK(compileLog(dir2Log).find("ambiguous") != std::string::npos,
          "the ambiguity diagnostic is the one Step 9 adds");
    //(3c) miss: today no test pins the not-found wording; keep it nailed
    //while the lookup source changes.
    auto dir3 = scenarioDir("stream_literal_none");
    writeFiles(dir3, { { "main.n",
        "int main() {\n"
        "  ByteStream bs = new ByteStream();\n"
        "  return bs.readStruct(\"NoSuchType\");\n"
        "}\n" } });
    CHECK(!compileDir(dir3), "an unknown literal type name fails");
    auto dir3Log = scenarioDir("stream_literal_none_log");   //twin for the log
    writeFiles(dir3Log, { { "main.n",
        "int main() {\n"
        "  ByteStream bs = new ByteStream();\n"
        "  return bs.readStruct(\"NoSuchType\");\n"
        "}\n" } });
    CHECK(compileLog(dir3Log).find("type not found: NoSuchType")
              != std::string::npos,
          "the preserved bare wording is still the not-found path");
    //(3d) a dotted literal names its package, so it round-trips with no
    //ambiguity at all — the unique-hit shape in the qualified spelling.
    //(The ambiguous advice scenario itself is a hard spec-5.5 conflict
    //now — see (2b) — so the qualified form is pinned on its own.)
    auto dir4 = scenarioDir("stream_literal_qualified");
    writeFiles(dir4, {
        { "alib.n", "struct S { int a; }\n" },
        { "main.n", "import alib;\n"
          "int main() {\n"
          "  ByteStream bs = new ByteStream();\n"
          "  alib.S w; w.a = 4; bs.writeStruct(w); bs.reset();\n"
          "  alib.S r = bs.readStruct(\"alib.S\");\n"
          "  return r.a - 4;\n"
          "}\n" } });
    CapturingIo cap4;
    CHECK(compileRun(dir4, cap4) == 0,
          "a dotted literal round-trips on its own");
}

//(4) Step 8: the entryPoint write/read round trip.
static void TestEntryPointRoundTrip() {
    auto dir = scenarioDir("entry_round");
    writeFiles(dir, { { "main.n", "int main() { return 7; }\n" } });
    CHECK(compileDir(dir), "single-file program builds");
    CompiledModule mod = ModuleLoader::Load(
        (dir / (dir.filename().string() + ".ncu")).string());
    //.ncu 2.0: the header carries the module's dotted path; the
    //transitional producer spells it as the output module name.
    CHECK(mod.modulePath == dir.filename().string(),
          "the module path is recorded in the header");
    CHECK(mod.entryPoint >= 0, "entry point index recorded");
    CHECK(mod.entryPoint < static_cast<int32_t>(mod.functions.size()),
          "entry point index in range");
    //After ModuleLoader::Load, the index must still point at the entry:
    CHECK(mod.functions[mod.entryPoint].name == "main.main",
          "the entry key is the path-derived qualified name");
    //Library-only module (the compile entry is alib.n, not main.n):
    //no PROJECT unit declares main -> -1.
    auto dir2 = scenarioDir("entry_lib");
    writeFiles(dir2, { { "alib.n", kLibSource } });
    CHECK(compileDir(dir2, { "alib.n" }), "a library unit builds on its own");
    CompiledModule lib = ModuleLoader::Load(
        (dir2 / (dir2.filename().string() + ".ncu")).string());
    CHECK(lib.entryPoint == -1, "a library unit has no entry point");
}

//Design §4 "two TUs each writing main()": different directories, because
//same directory is already rejected by the existing duplicate-name check
//(round 6 measurement) - that guard would swallow this case before the
//entry scan runs.
static void TestTwoMainsRejected() {
    auto dir = scenarioDir("two_mains");
    writeFiles(dir, {
        { "x/main.n", "int main() { return 1; }\n" },
        { "y/main.n", "int main() { return 2; }\n" } });
    CHECK(!compileDir(dir, { "x/main.n", "y/main.n" }),
          "two project entry candidates are a build error");
    auto dirLog = scenarioDir("two_mains_log");   //twin: log after a failed build
    writeFiles(dirLog, {
        { "x/main.n", "int main() { return 1; }\n" },
        { "y/main.n", "int main() { return 2; }\n" } });
    const std::string log = compileLog(dirLog, { "x/main.n", "y/main.n" });
    CHECK(log.find("x.main") != std::string::npos
          && log.find("y.main") != std::string::npos,
          "the diagnostic names both candidates by their qualified key");
}

//(5) Design §8: the builtin Object and a library Object are two keys on
//disk. The shell syntax is gone, so the old third shape — a ROOT-level
//user Object beside a library one — is a cross-module duplicate-class
//conflict now (spec 5.5 name-equality; the shell was the only thing that
//kept those two declarations apart). Only the key shape is pinned here -
//the runtime use of a user Object is blocked by the compiler-side
//bare-name short-circuits, which no task touches, so fields only.
static void TestBuiltinAndUserTypeNameCoexist() {
    auto dir = scenarioDir("object_key");
    writeFiles(dir, {
        { "alib.n", "class Object { public int tag; }\n" },
        { "main.n", "import alib;\n"
          "int main() {\n"
          "  alib.Object y = new alib.Object();\n"
          "  y.tag = 2;\n"
          "  return y.tag - 2;\n"
          "}\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0,
          "a library Object builds and runs beside the builtin one");
    CompiledModule mod = ModuleLoader::Load(
        (dir / (dir.filename().string() + ".ncu")).string());
    int nBare = 0, nLib = 0;
    for (const auto& c : mod.classes) {
        if (c.name == "Object") ++nBare;       //builtin: NO_OWNER => bare key
        if (c.name == "alib.Object") ++nLib;   //library: path-derived key
    }
    CHECK(nLib == 1, "the library declares exactly one alib.Object key");
    CHECK(nBare == 1, "the builtin keeps its bare Object key and nothing else shares it");
    //Keys apart, the layouts must point apart too: the builtin carries 0
    //fields, the library declaration 1 (CompiledClass::fieldCount).
    for (const auto& c : mod.classes) {
        if (c.name == "Object")
            CHECK(c.fieldCount == 0, "the bare key is the field-less builtin");
        if (c.name == "alib.Object")
            CHECK(c.fieldCount == 1, "alib.Object carries the library field");
    }
    //The old root-level user Object no longer coexists with a library
    //Object: both land on root under one name, and the duplicate-class
    //rule rejects the pair.
    auto dir2 = scenarioDir("object_key_dup");
    writeFiles(dir2, {
        { "alib.n", "class Object { public int tag; }\n" },
        { "main.n", "import alib;\n"
          "class Object { public int marker; }\n"
          "int main() { return 0; }\n" } });
    CHECK(!compileDir(dir2),
          "a root Object beside a library Object is a duplicate-class error");
}

//(5b) Design §8 runtime half: a packaged class named `Object` dispatches
//its own methods and its toString override (receiver dispatch goes
//through the receiver's own compiled class, keyed alib.Object — the
//bare-name short-circuits only ever name the ownerless builtin). The
//builtin root's own rendering is pinned by the object_ref e2e case.
static void TestUserObjectDispatch() {
    auto dir = scenarioDir("object_dispatch");
    writeFiles(dir, {
        { "alib.n",
          "class Object {\n"
          "  public int tag;\n"
          "  public int bumped() { return tag + 1; }\n"
          "  public string toString() { return \"user-object\"; }\n"
          "}\n" },
        { "main.n",
          "import io;\n"
          "import alib;\n"
          "int main() {\n"
          "  alib.Object o = new alib.Object();\n"
          "  o.tag = 5;\n"
          "  io.print(o.bumped());\n"
          "  io.print(o.toString());\n"
          "  return o.bumped() - 6;\n"
          "}\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0,
          "a packaged Object builds with its own methods");
    //print keeps its documented contract for class values (explicit
    //.toString()) regardless of the class's name.
    CHECK(cap.text == "6\nuser-object\n",
          "user methods and the toString override win");
}

//Phase 6 section 2 ruling (enum-method keys): enum methods carry
//ENCLOSURE-QUALIFIED function-table keys ("alib.Color.rank"). Enum
//methods never dispatch by name — direct calls go through table
//indices — so a bare "rank" cannot disambiguate two same-named methods
//of different enums in one unit. The qualified key resolves cross-unit
//enum-method imports by construction, not by bare-name luck.
const char* kEnumKeyLibSource =
    "enum Color {\n"
    "  Red,\n"
    "  Green;\n"
    "  public int rank() { return 7; }\n"
    "}\n"
    "enum Shape {\n"
    "  Circle,\n"
    "  Square;\n"
    "  public int rank() { return 9; }\n"
    "}\n";

const char* kEnumKeyProgram =
    "import alib;\n"
    "int main() {\n"
    "  alib.Color c = alib.Color.Red;\n"
    "  alib.Shape s = alib.Shape.Circle;\n"
    "  return c.rank() * 10 + s.rank();\n"
    "}\n";   //7*10 + 9 = 79

void TestEnumMethodQualifiedKeys() {
    const auto dir = scenarioDir("enum_method_keys");
    writeFiles(dir, {
        { "alib.n", kEnumKeyLibSource },
        { "main.n", kEnumKeyProgram } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 79,
          "both same-named enum methods run (7*10+9)");
    //The produced module's function table carries the qualified keys,
    //and no bare "rank" record may survive them.
    try {
        CompiledModule mod = ModuleLoader::Load(
            (dir / "enum_method_keys.ncu").string());
        CHECK(mod.FindFunction("alib.Color.rank") >= 0,
              "Color.rank keyed by its enclosure path");
        CHECK(mod.FindFunction("alib.Shape.rank") >= 0,
              "Shape.rank keyed by its enclosure path");
        bool bareRemains = false;
        for (const auto& fn : mod.functions)
            if (fn.name == "rank") bareRemains = true;
        CHECK(!bareRemains, "no bare 'rank' function record remains");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "internal error: %s\n", e.what());
        CHECK(false, "loading the enum-key scenario module");
    }
}

} // namespace

int main() {
    Runtime::StaticInit();
    std::fprintf(stderr, "=== Source Library Integration Tests ===\n");
    TestHappyPath();
    TestUnimportedRejected();
    TestNoBareNames();
    TestNoSiblingAutoVisibility();
    TestOwnerIsolation();
    TestLibraryDefinedTypes();
    TestLibraryInheritance();
    TestLibraryInterface();
    TestLibraryEnumMethod();
    TestModuleTypeWithoutNamespaceWrapper();
    TestModuleTypeOwnerIsolation();
    TestQualifiedTypeHeadKeepsLegalMemberAccess();
    TestQualifiedBaseAndCast();
    TestThreeSegmentBaseGivesNamedDiagnosis();
    TestQualifiedGenericArgStaysRejected();
    TestQualifiedLibraryTypeWithoutArgsWorks();
    TestSameNameLibraryAndRootTypeIsolated();
    TestPathBeatsShellName();
    TestDuplicateTypeInOnePackageRejected();
    TestStreamLiteralResolvesAtCompileTime();
    TestEntryPointRoundTrip();
    TestTwoMainsRejected();
    TestBuiltinAndUserTypeNameCoexist();
    TestUserObjectDispatch();
    TestEnumMethodQualifiedKeys();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
