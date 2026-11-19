//v0.13.9 phase 3 Task 5: & parameters and reference bindings. A & formal
//binds the caller's storage (handle pass-through, no copy, no deep copy):
//writes inside the callee land in the caller's local/field/element, stay
//visible when an exception escapes, and rvalue arguments bind a temporary
//record (writes lost, legal and documented — spec §3.9). Local & decls
//bind fields, elements and chains through the same MakeRef family.
//The modules are REAL-compiled (ModuleBuilder → NcuLoader closure →
//NcuLinker → VmExecutor) with no bytecode surgery: these pins are
//source-level behavior and heap-allocation counts.
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)
#define REQUIRE(cond) \
    do { if (!(cond)) { FAIL("precondition: " #cond); return; } } while(0)

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_ref_params";
    std::filesystem::create_directories(dir);
    return dir;
}

struct ExecResult {
    std::unique_ptr<VmExecutor> exec;
    std::unique_ptr<CompiledModule> mod;
    int exitCode = -1;
    bool built = false;
    bool ran = false;
    std::string error;
};

//Compile <tag>.n, link and execute; keep the executor alive so the
//heap stays inspectable. An executor throw is captured in error.
static ExecResult runProgram(const std::string& tag,
                             const std::string& source)
{
    ExecResult out;
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);
    {
        std::ofstream file(nPath, std::ios::binary);
        file << source;
    }
    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    ListCompileLogger logger;
    try {
        ModuleBuilder builder(params, logger);
        if (!builder.BuildArtifacts()) {
            for (auto it = logger.cbegin(); it != logger.cend(); ++it)
                std::cerr << "    " << (*it)->Message() << "\n";
            return out;
        }
    } catch (const std::exception& e) {
        //Backend throws (a form the emitter has not landed yet) surface
        //here, not as diagnostics.
        out.error = e.what();
        return out;
    }
    out.built = true;
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    out.mod = std::make_unique<CompiledModule>(
        NcuLinker::Link(std::move(loaded.units), loaded.entryKey));
    out.exec = std::make_unique<VmExecutor>();
    try {
        out.exitCode = out.exec->Execute(*out.mod);
        out.ran = true;
    } catch (const std::exception& e) {
        out.error = e.what();
    }
    return out;
}

static size_t allocCount(const ExecResult& r)
{
    return r.exec ? r.exec->HeapAllocCountForTests() : 0;
}

//The write-through floor: bump(int&) changes the caller's local.
//The alloc pin doubles as the argument-side box-suppression check: x is
//promoted exactly once (one box) and the & argument adds nothing — a
//boundary re-box would show up as a second allocation.
static void refParamLocalWriteThrough()
{
    std::cerr << "  refParamLocalWriteThrough ... ";
    auto r = runProgram("refparam_local",
        "void bump(int& v) {\n"
        "    v = v + 1;\n"
        "}\n"
        "int main() {\n"
        "    int x = 10;\n"
        "    bump(x);\n"
        "    return x;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 11, "write-through (exit=" +
        std::to_string(r.exitCode) + ")");
    CHECK(allocCount(r) == 1, "one promotion box, no per-call boxing (alloc=" +
        std::to_string(allocCount(r)) + ")");
    PASS();
}

//Single-level field target: the & formal binds the field's cell inside
//the caller's record; the write lands in obj.f.
static void refParamFieldWriteThrough()
{
    std::cerr << "  refParamFieldWriteThrough ... ";
    auto r = runProgram("refparam_field",
        "struct Box {\n"
        "    int f;\n"
        "}\n"
        "void bump(int& v) {\n"
        "    v = v + 1;\n"
        "}\n"
        "int main() {\n"
        "    Box b;\n"
        "    b.f = 41;\n"
        "    bump(b.f);\n"
        "    return b.f;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 42, "field write-through (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//Element target: the & formal binds the array element's cell.
static void refParamElementWriteThrough()
{
    std::cerr << "  refParamElementWriteThrough ... ";
    auto r = runProgram("refparam_element",
        "void bump(int& v) {\n"
        "    v = v + 1;\n"
        "}\n"
        "int main() {\n"
        "    int a[3];\n"
        "    a[0] = 1;\n"
        "    bump(a[0]);\n"
        "    return a[0];\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 2, "element write-through (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//struct & formal: no deep copy — the callee mutates the caller's record.
static void structRefParamWriteThrough()
{
    std::cerr << "  structRefParamWriteThrough ... ";
    auto r = runProgram("refparam_struct",
        "struct Point {\n"
        "    int x;\n"
        "    int y;\n"
        "}\n"
        "void scale(Point& p) {\n"
        "    p.x = p.x * 2;\n"
        "    p.y = p.y * 2;\n"
        "}\n"
        "int main() {\n"
        "    Point pt;\n"
        "    pt.x = 1;\n"
        "    pt.y = 5;\n"
        "    scale(pt);\n"
        "    return pt.x + pt.y;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 12, "struct write-through, no deep copy (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//Rvalue arguments bind a temporary record: legal, the write is lost,
//and a later read of the same spelling sees the fresh value.
static void rvalueRefParamLost()
{
    std::cerr << "  rvalueRefParamLost ... ";
    auto r = runProgram("refparam_rvalue",
        "void bump(int& v) {\n"
        "    v = v + 1;\n"
        "}\n"
        "int read(int& v) {\n"
        "    return v;\n"
        "}\n"
        "int main() {\n"
        "    bump(1 + 2);\n"
        "    return read(3);\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 3, "rvalue binding legal, writes lost (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//Exception visibility (spec §3.9): writes through a & formal survive an
//exception escaping the callee — no writeback-time rollback exists.
static void exceptionPartialWriteVisible()
{
    std::cerr << "  exceptionPartialWriteVisible ... ";
    auto r = runProgram("refparam_exception",
        "void setThenThrow(int& v) {\n"
        "    v = 5;\n"
        "    throw Exception(\"after write\");\n"
        "}\n"
        "int main() {\n"
        "    int x = 10;\n"
        "    try {\n"
        "        setThenThrow(x);\n"
        "    } catch (Exception e) {\n"
        "    }\n"
        "    return x;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 5, "partial write visible after throw (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//f(x, x): two & formals alias the same variable — the caller's duty,
//documented, no diagnostic. The sequential writes compose.
static void doubleRefAliasNoDiagnostic()
{
    std::cerr << "  doubleRefAliasNoDiagnostic ... ";
    auto r = runProgram("refparam_alias",
        "void setBoth(int& a, int& b) {\n"
        "    a = 5;\n"
        "    b = b + 1;\n"
        "}\n"
        "int main() {\n"
        "    int x = 10;\n"
        "    setBoth(x, x);\n"
        "    return x;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 6, "aliased writes compose (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//Local & decl binding a field: the binding points at the field's cell;
//stores through r land in b.f.
static void localBindFieldWriteThrough()
{
    std::cerr << "  localBindFieldWriteThrough ... ";
    auto r = runProgram("refbind_field",
        "struct Box {\n"
        "    int f;\n"
        "}\n"
        "int main() {\n"
        "    Box b;\n"
        "    b.f = 1;\n"
        "    int& r = b.f;\n"
        "    r = 9;\n"
        "    return b.f;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 9, "field binding write-through (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//Local & decl binding an array element: stores land in the element cell.
static void localBindElementWriteThrough()
{
    std::cerr << "  localBindElementWriteThrough ... ";
    auto r = runProgram("refbind_element",
        "int main() {\n"
        "    int a[2];\n"
        "    a[1] = 1;\n"
        "    int& r = a[1];\n"
        "    r = 8;\n"
        "    return a[1];\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 8, "element binding write-through (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//Local & decl binding a chained member path (o.b.c): the pair names the
//inner record and c's cell; stores land in the chain target.
static void localBindChainWriteThrough()
{
    std::cerr << "  localBindChainWriteThrough ... ";
    auto r = runProgram("refbind_chain",
        "struct Inner {\n"
        "    int c;\n"
        "}\n"
        "struct Outer {\n"
        "    Inner b;\n"
        "}\n"
        "int main() {\n"
        "    Outer o;\n"
        "    o.b.c = 1;\n"
        "    int& r = o.b.c;\n"
        "    r = 9;\n"
        "    return o.b.c;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 9, "chained binding write-through (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//Local & decl binding an rvalue: legal (the temp record spec §3.9);
//writes land in the temp and are lost — the program still behaves.
static void localBindRvalueLegal()
{
    std::cerr << "  localBindRvalueLegal ... ";
    auto r = runProgram("refbind_rvalue",
        "struct Point {\n"
        "    int x;\n"
        "    int y;\n"
        "}\n"
        "int main() {\n"
        "    Point& r = Point();\n"
        "    r.x = 9;\n"
        "    int& n = 1 + 2;\n"
        "    n = 7;\n"
        "    return 3;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 3, "rvalue bindings run, writes lost (exit=" +
        std::to_string(r.exitCode) + ")");
    PASS();
}

//The no-& floor: a program without references keeps value semantics and
//allocates nothing.
static void noRefZeroAllocFloor()
{
    std::cerr << "  noRefZeroAllocFloor ... ";
    auto r = runProgram("refparam_none",
        "void byVal(int v) {\n"
        "    v = 99;\n"
        "}\n"
        "int main() {\n"
        "    int x = 1;\n"
        "    byVal(x);\n"
        "    return x;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.ran, "executes: " + r.error);
    CHECK(r.exitCode == 1, "value semantics unchanged (exit=" +
        std::to_string(r.exitCode) + ")");
    CHECK(allocCount(r) == 0, "no allocations (alloc=" +
        std::to_string(allocCount(r)) + ")");
    PASS();
}

int main()
{
    //In-process host: the id-string table and cast matrix must exist
    //before any ModuleBuilder touches them.
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();
    refParamLocalWriteThrough();
    refParamFieldWriteThrough();
    refParamElementWriteThrough();
    structRefParamWriteThrough();
    rvalueRefParamLost();
    exceptionPartialWriteVisible();
    doubleRefAliasNoDiagnostic();
    localBindFieldWriteThrough();
    localBindElementWriteThrough();
    localBindChainWriteThrough();
    localBindRvalueLegal();
    noRefZeroAllocFloor();
    std::cerr << "ref_params: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
