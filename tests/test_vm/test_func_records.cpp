// --- lambda/closure plan Task 5: 4-slot function records + paired
// instantiation descriptors (NCU v2.23). Real programs compile and
// execute in-process (the test_generic_runtime_info harness pattern,
// including its static-init contract); the heap stays inspectable so
// the record cells pin the ABI: {target, receiver, form, descIdx}.
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include "Disassembler.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)
//Like CHECK but for shape preconditions whose failure would make the
//following assertions meaningless.
#define REQUIRE(cond) \
    do { if (!(cond)) { FAIL("precondition: " #cond); return; } } while(0)

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_func_records";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile <tag>.n, load the closure, link, execute, keep the executor
//alive so the heap (the function records) stays inspectable.
struct ExecResult {
    std::unique_ptr<VmExecutor> exec;
    std::unique_ptr<CompiledModule> mod;
    int exitCode = -1;
    bool built = false;
    std::string diagnostics;
};

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
    ModuleBuilder builder(params, logger);
    if (!builder.BuildArtifacts()) {
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            out.diagnostics += (*it)->Message() + "\n";
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
    out.exitCode = out.exec->Execute(*out.mod);
    return out;
}

//The RTK_Func heap records of a finished run — the function handles the
//program materialized (small deterministic programs allocate a known
//count; GC never reuses a live slot mid-run).
static std::vector<int32_t> funcRecordHandles(const VmExecutor& exec)
{
    std::vector<int32_t> hits;
    const size_t slots = exec.HeapSlotCountForTests();
    for (int32_t i = 1; i < static_cast<int32_t>(slots); ++i)
        if (exec.HeapSlotKindForTests(i) == RTK_Func)
            hits.push_back(i);
    return hits;
}

static int32_t cellOf(const VmExecutor& exec, int32_t handle, size_t cell)
{
    return exec.ReadHeapRecordCellForTests(handle, cell);
}

//A non-generic value reference allocates the 4-slot record with the
//no-descriptor sentinel in the new cell: {target=inc, this=0, form=0,
//descIdx=0xFFFF}. Before Task 5 the record has 3 cells — reading the
//descriptor cell fails hard, which is the RED this test exists for.
static void testFuncRecordFourSlots()
{
    TEST(funcRecordFourSlots);
    ExecResult r = runProgram("funcval_four",
        "int inc(int x) { return x + 1; }\n"
        "int main() {\n"
        "    func<int, int> f = inc;\n"
        "    if (f(40) != 41) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    CHECK(r.built, "build should succeed: " + r.diagnostics);
    REQUIRE(r.exec != nullptr);
    CHECK(r.exitCode == 0, "call through the handle should return 0, got "
        + std::to_string(r.exitCode));
    const auto handles = funcRecordHandles(*r.exec);
    CHECK(handles.size() == 1, "exactly one function handle expected, got "
        + std::to_string(handles.size()));
    if (handles.size() != 1)
    {
        PASS();
        return;
    }
    const int32_t target = cellOf(*r.exec, handles[0], 0);
    const int32_t incIdx = r.mod->FindFunction("funcval_four.inc");
    REQUIRE(incIdx >= 0);
    CHECK(target == incIdx, "record target should be inc's function index");
    CHECK(cellOf(*r.exec, handles[0], 1) == 0,
        "free-function record receiver cell stays 0");
    CHECK(cellOf(*r.exec, handles[0], 2) == 0,
        "free-function record form cell is the static form");
    CHECK(cellOf(*r.exec, handles[0], 3) == 0xFFFF,
        "non-generic record carries the no-descriptor sentinel");
    PASS();
}

//Spec §五 acceptance, in-process: a generic free function's value binds
//through the expected type's instantiation; two different instantiations'
//handles differ (the descIdx dimension of the four-tuple), two references
//to the same instantiation stay equal, and both call correctly.
static void testGenericFuncValueRecords()
{
    TEST(genericFuncValueRecords);
    ExecResult r = runProgram("funcval_generic",
        "T pick<T>(T a, T b) { return b; }\n"
        "int main() {\n"
        "    func<int, int, int> f = pick;\n"
        "    func<float, float, float> g = pick;\n"
        "    if (f == g) { return 1; }\n"
        "    func<int, int, int> h = pick;\n"
        "    if (h != f) { return 2; }\n"
        "    if (f(3, 5) != 5) { return 3; }\n"
        "    if (g(3.5, 7.5) != 7.5) { return 4; }\n"
        "    return 0;\n"
        "}\n");
    CHECK(r.built, "the generic function value should bind: "
        + r.diagnostics);
    REQUIRE(r.exec != nullptr);
    CHECK(r.exitCode == 0, "equality/call verdicts should hold, got "
        + std::to_string(r.exitCode));
    const auto handles = funcRecordHandles(*r.exec);
    CHECK(handles.size() == 3, "three function handles expected, got "
        + std::to_string(handles.size()));
    if (handles.size() != 3)
    {
        PASS();
        return;
    }
    //All three records share the template-row target; the int and float
    //instantiations part ways in the descriptor cell.
    const int32_t target = cellOf(*r.exec, handles[0], 0);
    int sameDesc = 0, otherDesc = 0;
    for (size_t i = 0; i < handles.size(); ++i)
    {
        CHECK(cellOf(*r.exec, handles[i], 0) == target,
            "every record targets the same template row");
        if (cellOf(*r.exec, handles[i], 3)
            == cellOf(*r.exec, handles[0], 3))
            ++sameDesc;
        else
            ++otherDesc;
    }
    CHECK(sameDesc == 2 && otherDesc == 1,
        "two handles share the int row, one carries the float row");
    PASS();
}

//A bound (receiver-carrying) reference keeps the sentinel: the
//descriptor rides the receiver's head channel, the record cell is
//meaningless for bound forms (spec §4.2).
static void testBoundRecordDescSentinel()
{
    TEST(boundRecordDescSentinel);
    ExecResult r = runProgram("funcval_bound",
        "class Box {\n"
        "    public int v;\n"
        "    public Box(int n) { v = n; }\n"
        "    public int get() { return v; }\n"
        "}\n"
        "int main() {\n"
        "    Box b = Box(7);\n"
        "    func<int> g = b.get;\n"
        "    if (g() != 7) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    CHECK(r.built, "build should succeed: " + r.diagnostics);
    REQUIRE(r.exec != nullptr);
    CHECK(r.exitCode == 0, "bound call should return 0, got "
        + std::to_string(r.exitCode));
    const auto handles = funcRecordHandles(*r.exec);
    CHECK(handles.size() == 1, "exactly one bound handle expected, got "
        + std::to_string(handles.size()));
    if (handles.size() != 1)
    {
        PASS();
        return;
    }
    CHECK(cellOf(*r.exec, handles[0], 3) == 0xFFFF,
        "bound records carry the no-descriptor sentinel");
    PASS();
}

//Spec §五 acceptance: a generic class's static method binds as a value
//through the expected type's instantiation (the 0.13.1a named reject
//retires) — the signature must mention the class's parameter, and the
//handle pairs the template-row clone slot with the inferred descriptor
//row (the call resolves T through it). The T-less CS0411 shape is a
//compile-time cannot-infer reject, pinned in testGenericStaticTless.
static void testGenericStaticValueRef()
{
    TEST(genericStaticValueRef);
    ExecResult r = runProgram("funcval_generic_static",
        "class Tag<T> {\n"
        "    public static string kind(T v) { return typeName(T); }\n"
        "}\n"
        "int main() {\n"
        "    func<string, int> f = Tag.kind;\n"
        "    if (f(42) != \"int\") { return 1; }\n"
        "    func<string, string> g = Tag.kind;\n"
        "    if (g(\"x\") != \"string\") { return 2; }\n"
        "    if (f == g) { return 3; }\n"
        "    func<string, float> h = Tag<float>.kind;\n"
        "    if (h(1.5) != \"float\") { return 4; }\n"
        "    return 0;\n"
        "}\n");
    CHECK(r.built, "the generic static reference should bind: "
        + r.diagnostics);
    REQUIRE(r.exec != nullptr);
    CHECK(r.exitCode == 0, "T should resolve through the descriptor row, "
        "got " + std::to_string(r.exitCode));
    PASS();
}

//The CS0411 shape: a signature that never mentions the class's
//parameter leaves the instantiation underdetermined from any expected
//type — the compile-time cannot-infer reject (same invariant fragment
//as the invoke channel's inference diagnostic), never a silent
//sentinel-descriptor handle the runtime would throw on.
static void testGenericStaticTless()
{
    TEST(genericStaticTless);
    ExecResult r = runProgram("funcval_generic_static_tless",
        "class Tag<T> {\n"
        "    public static string tag() { return typeName(T); }\n"
        "}\n"
        "int main() {\n"
        "    func<string> f = Tag.tag;\n"
        "    return 0;\n"
        "}\n");
    CHECK(!r.built, "the T-less reference must be a compile-time reject");
    CHECK(r.diagnostics.find("cannot infer type argument")
        != std::string::npos,
        "expected the cannot-infer diagnostic, got: " + r.diagnostics);
    PASS();
}

//The emission shape is observable in the disassembly: every OP_MakeFunc
//line carries the descriptor word next to the function index (the
//OP_CallStatic line's desc= convention).
static void testMakeFuncDisassemblyCarriesDesc()
{
    TEST(makeFuncDisassemblyCarriesDesc);
    ExecResult r = runProgram("funcval_disasm",
        "int inc(int x) { return x + 1; }\n"
        "int main() {\n"
        "    func<int, int> f = inc;\n"
        "    if (f(40) != 41) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    CHECK(r.built, "build should succeed: " + r.diagnostics);
    REQUIRE(r.mod != nullptr);
    REQUIRE(r.mod->entryPoint >= 0
        && static_cast<size_t>(r.mod->entryPoint)
            < r.mod->functions.size());
    const auto lines = DisassembleCode(
        r.mod->functions[static_cast<size_t>(r.mod->entryPoint)], *r.mod);
    bool sawMakeFunc = false;
    for (const auto& line : lines)
    {
        if (line.text.find("make_func") == std::string::npos)
            continue;
        sawMakeFunc = true;
        CHECK(line.text.find("desc=0x") != std::string::npos,
            "make_func line should render the descriptor word: "
            + line.text);
    }
    CHECK(sawMakeFunc, "main should contain a make_func");
    PASS();
}

int main() {
#ifdef _WIN32
    //Prevent CRT abort/error dialogs from blocking the test runner.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS
                             | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    std::cerr << "=== NLang function-record Unit Tests ===\n\n";

    //The in-process build host's static-init contract (ModuleBuilder
    //creates IdString-indexed modules).
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    //The OOB-shape probes run last: under the pre-Task-5 3-slot ABI the
    //descriptor-cell read fails hard (a Debug bounds abort), which would
    //cut the run short.
    try
    {
        testMakeFuncDisassemblyCarriesDesc();
        testGenericStaticValueRef();
        testGenericStaticTless();
        testGenericFuncValueRecords();
        testFuncRecordFourSlots();
        testBoundRecordDescSentinel();
    }
    catch (const std::exception& e)
    {
        ++g_fail;
        std::cerr << "FAIL: uncaught exception: " << e.what() << "\n";
    }

    std::cerr << "\n=== " << g_pass << " passed, " << g_fail
              << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
