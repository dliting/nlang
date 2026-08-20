// --- Phase 11 stdlib unit tests ---
// In-process compile+run via the public ModuleBuilder API: write a temp
// .n source, build it to a temp .nmod (Build() has no in-memory module
// accessor), load it back with ModuleLoader, execute with VmExecutor.
// Diagnostics come from ListCompileLogger. The e2e suite remains the
// full-pipeline gate; these tests pin resolver/diagnostic behavior that
// is awkward to assert from exit codes alone.

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "ModuleLoader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
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

//Scratch directory for the temp sources and modules.
static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_stdlib";
    std::filesystem::create_directories(dir);
    return dir;
}

struct BuildOutcome
{
    bool ok = false;
    std::string diagnostics;
};

//Compile source text; on success returns true. The module is written to
//<tag>.nmod in the scratch dir, ready to be loaded by the caller.
static BuildOutcome buildSource(const std::string& tag,
    const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".nmod");
    std::filesystem::remove(modPath);

    {
        std::ofstream out(nPath, std::ios::binary);
        out << source;
    }

    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();

    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);

    BuildOutcome outcome;

    //Exception boundary (same contract as ncc main): codegen internal
    //errors throw std::runtime_error; without the catch the unhandled
    //exception aborts the whole test binary.
    bool built = false;
    try {
        built = builder.Build();
    } catch (const std::exception& e) {
        outcome.diagnostics += std::string("internal error: ") + e.what();
        return outcome;
    }

    outcome.ok = built && std::filesystem::exists(modPath);
    //ListCompileLogger only exposes cbegin/cend (no begin/end).
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        outcome.diagnostics += (*it)->Message() + "\n";
    return outcome;
}

//Compile + execute; returns the value main() returned, or -1 when the
//build failed.
static int runSource(const std::string& tag, const std::string& source)
{
    const auto modPath = scratchDir() / (tag + ".nmod");
    BuildOutcome outcome = buildSource(tag, source);
    if (!outcome.ok)
        return -1;
    CompiledModule mod = ModuleLoader::Load(modPath.string());
    VmExecutor exec;
    return exec.Execute(mod);
}

void test_stdlib_sqrt_value()
{
    TEST(stdlib_sqrt_value);
    const int rc = runSource("sqrt_value",
        "int main() {\n"
        "    float a = math.sqrt(4.0);\n"
        "    float b = math.sqrt(4);\n"
        "    if (a == 2.0 && b == 2.0) return 0;\n"
        "    return 1;\n"
        "}\n");
    CHECK(rc == 0, "math.sqrt(4.0)/math.sqrt(4) should both yield 2.0");
    PASS();
}

void test_stdlib_unknown_function()
{
    TEST(stdlib_unknown_function);
    BuildOutcome outcome = buildSource("unknown_fn",
        "int main() { float a = math.nope(1.0); return 0; }\n");
    CHECK(!outcome.ok, "math.nope must not compile");
    //The diagnostic must name the function — a bare "cannot resolve the
    //field: math" (today's message) does not tell the user what was wrong.
    CHECK(outcome.diagnostics.find("nope") != std::string::npos,
        "diagnostic should name the unknown function");
    PASS();
}

void test_stdlib_arity_error()
{
    TEST(stdlib_arity_error);
    BuildOutcome outcome = buildSource("arity_err",
        "int main() { float a = math.sqrt(); return 0; }\n");
    CHECK(!outcome.ok, "math.sqrt() must not compile");
    CHECK(outcome.diagnostics.find("sqrt") != std::string::npos,
        "diagnostic should name the function");
    PASS();
}

void test_stdlib_type_error()
{
    TEST(stdlib_type_error);
    BuildOutcome outcome = buildSource("type_err",
        "int main() { float a = math.sqrt(\"x\"); return 0; }\n");
    CHECK(!outcome.ok, "math.sqrt(\"x\") must not compile");
    CHECK(outcome.diagnostics.find("sqrt") != std::string::npos,
        "diagnostic should name the function");
    PASS();
}

void test_stdlib_array_arg_rejected()
{
    TEST(stdlib_array_arg_rejected);
    //Regression guard (Step 0 review MAJOR): EvalDataType of an array
    //valued expression returns the ELEMENT kind, so without the
    //IsArrayValuedExpr guard `int[]` masqueraded as int and the intrinsic
    //reinterpreted the array handle as a float — silently wrong code.
    //One case per masquerade shape: lvalue, new-array, array-returning
    //call (the e2e canary keeps the lvalue representative).
    BuildOutcome lvalue = buildSource("array_arg_lvalue",
        "int main() {\n"
        "    int[] arr = new int[3];\n"
        "    float f = math.sqrt(arr);\n"
        "    return 0;\n"
        "}\n");
    CHECK(!lvalue.ok, "math.sqrt(int[] local) must not compile");
    CHECK(lvalue.diagnostics.find("array") != std::string::npos,
        "diagnostic should name the array problem");

    BuildOutcome newArray = buildSource("array_arg_new",
        "int main() {\n"
        "    float f = math.sqrt(new int[3]);\n"
        "    return 0;\n"
        "}\n");
    CHECK(!newArray.ok, "math.sqrt(new int[3]) must not compile");
    CHECK(newArray.diagnostics.find("array") != std::string::npos,
        "diagnostic should name the array problem");

    BuildOutcome callRet = buildSource("array_arg_call",
        "int[] mk() { int[] a = new int[2]; return a; }\n"
        "int main() {\n"
        "    float f = math.sqrt(mk());\n"
        "    return 0;\n"
        "}\n");
    CHECK(!callRet.ok, "math.sqrt(array-returning call) must not compile");
    CHECK(callRet.diagnostics.find("array") != std::string::npos,
        "diagnostic should name the array problem");
    PASS();
}

void test_stdlib_unknown_namespace_still_errors()
{
    TEST(stdlib_unknown_namespace_still_errors);
    //Regression guard: the namespace intercept must not swallow the
    //normal error path for non-namespace member expressions.
    BuildOutcome outcome = buildSource("unknown_ns",
        "int main() { float a = nope.sqrt(1.0); return 0; }\n");
    CHECK(!outcome.ok, "nope.sqrt must not compile");
    CHECK(outcome.diagnostics.find("Cannot resolve") != std::string::npos,
        "should keep the normal unresolved-field diagnostic");
    PASS();
}

int main()
{
#ifdef _WIN32
    //Keep stray crashes silent in CI contexts (same as test_vm).
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS
                             | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    //Hidden host contract (same as ncc/nvm main): driving the compiler
    //requires Runtime::StaticInit() first — it sets up the IdString
    //interned-name table the resolver indexes fields by. Without it any
    //Build() segfaults in IdString lookup rather than failing loudly.
    Runtime::StaticInit();

    std::cerr << "=== NLang StdLib Unit Tests ===\n\n";

    try {
        test_stdlib_sqrt_value();
        test_stdlib_unknown_function();
        test_stdlib_arity_error();
        test_stdlib_type_error();
        test_stdlib_array_arg_rejected();
        test_stdlib_unknown_namespace_still_errors();
    } catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
