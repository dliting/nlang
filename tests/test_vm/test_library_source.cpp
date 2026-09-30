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
        std::ofstream out(dir / entry.first, std::ios::binary);
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
    CompiledModule mod = ModuleLoader::Load(
        (dir / (modName + ".nmod")).string());
    VmExecutor exec;
    exec.AddNativeSearchDir(dir.string());
    exec.SetHostIo(&cap);
    return exec.Execute(mod);
}

// Ordinary NLang functions: same-library bare calls (quad -> twice) and
// recursion (fact). No native keyword, no DLL - every body is compiled.
const char* kLibSource =
    "namespace alib {\n"
    "int twice(int x) {\n"
    "  return x * 2;\n"
    "}\n"
    "int quad(int x) {\n"
    "  return twice(twice(x));\n"
    "}\n"
    "int fact(int n) {\n"
    "  if (n <= 1) return 1;\n"
    "  return n * fact(n - 1);\n"
    "}\n"
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
    const char* lib = "namespace alib {\nint f() { return 1; }\n}\n";
    const char* prog = "int main() {\n  return alib.f();\n}\n";
    writeFiles(dir, { { "alib.n", lib }, { "main.n", prog } });
    CHECK(!compileDir(dir), "unimported library call is rejected");
}

// import exposes only the qualified name (alib.f), never a bare f().
void TestNoBareNames() {
    const auto dir = scenarioDir("bare");
    const char* lib = "namespace alib {\nint f() { return 1; }\n}\n";
    const char* prog = "import alib;\nint main() {\n  return f();\n}\n";
    writeFiles(dir, { { "alib.n", lib }, { "main.n", prog } });
    CHECK(!compileDir(dir), "import does not inject bare names");
}

// A library TU has no same-directory auto-visibility (the project-only D7
// rule): alib.g may not call blib.h unless alib itself imports blib, even
// though the consumer imports both (so both namespaces are inlined).
void TestNoSiblingAutoVisibility() {
    const auto dir = scenarioDir("sibling");
    const char* blib = "namespace blib {\nint h() { return 5; }\n}\n";
    const char* alib =
        "namespace alib {\nint g() { return blib.h(); }\n}\n";
    const char* prog =
        "import alib;\nimport blib;\n"
        "int main() {\n  return alib.g();\n}\n";
    writeFiles(dir, { { "blib.n", blib }, { "alib.n", alib },
                      { "main.n", prog } });
    CHECK(!compileDir(dir),
          "library TU has no same-directory auto-visibility");
}

// A project free function and a library function with the same name bind
// independently (owner tagging keeps the candidates apart).
void TestOwnerIsolation() {
    const auto dir = scenarioDir("owner");
    const char* lib = "namespace alib {\nint val() { return 7; }\n}\n";
    const char* prog =
        "import io;\nimport alib;\n"
        "int val() { return 9; }\n"
        "int main() {\n"
        "  if (val() != 9) return 1;\n"
        "  if (alib.val() != 7) return 2;\n"
        "  io.print(\"ok\");\n"
        "  return 0;\n"
        "}\n";
    writeFiles(dir, { { "alib.n", lib }, { "main.n", prog } });
    CapturingIo cap;
    int rc = compileRun(dir, cap);
    CHECK(rc == 0, "project/library same-named function (rc)");
    CHECK(cap.text == "ok\n", "project/library same-named function output");
}

// Phase 4b-2: a library may define its own TYPES - a class with a ctor and
// methods, an enum, and an NLang factory that returns the class. The consumer
// instantiates the class, reads a field, calls a method, uses an enum value,
// and calls the factory. This exercises two fixes:
//   * the recursive codegen traversal - root -> namespace -> class -> method
//     is THREE levels; the old fixed two-level walk never registered or
//     compiled a method nested inside a namespaced class
//     ("call to method without a body");
//   * the namespace-receiver context fix - `alib.Color.Green` enum-value
//     access must look Color up inside the namespace node, not the meta
//     SnType singleton.
const char* kTypeLibSource =
    "namespace alib {\n"
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
    "}\n"
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
//   * bare method names for classes nested in a namespace - a method's VM
//     name is never namespace-qualified, so `legs`/`kind` resolve.
const char* kInheritLibSource =
    "namespace alib {\n"
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
    "}\n"
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
    "namespace alib {\n"
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
    "}\n"
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
    "namespace alib {\n"
    "enum Rank {\n"
    "  Low = 0, High = 1;\n"
    "  public int doubled() {\n"
    "    return this * 2;\n"
    "  }\n"
    "}\n"
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

// A unit reached by a single-segment import whose file carries NO `namespace`
// wrapper keeps its members at the root, exactly like a project module. Its
// type must still be nameable as `unit.Type` - the container rule is one rule,
// shared with the qualified-call side (which already resolved other.total).
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
          "namespace alib {\n"
          "class B {\n"
          "  public int v;\n"
          "  public int get() { return v + 1; }\n"
          "}\n"
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
        { "alib.n", "namespace alib {\nstruct Vec { int x; }\n}\n" },
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
        { "alib.n", "namespace alib {\nstruct Vec { int x; }\n}\n" },
        { "main.n", "import alib;\nint main() {\n"
          "  alib.Vec v; v.x = 2;\n"
          "  List<int> nums;\n"
          "  return v.x - 2;\n"
          "}\n" } });
    CapturingIo cap;
    CHECK(compileRun(dir, cap) == 0,
          "alib.Vec (no args) compiles and runs beside a builtin generic");
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
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
