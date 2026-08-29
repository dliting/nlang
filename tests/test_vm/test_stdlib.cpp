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
#include "nlang/vm/StdLib.h"
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
//Task 7 (D6): every source gets the built-in namespace imports prepended,
//so the tests below exercise stdlib behavior — the import gate itself is
//covered in test_module_import.cpp.
static BuildOutcome buildSource(const std::string& tag,
    const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".nmod");
    std::filesystem::remove(modPath);

    {
        std::ofstream out(nPath, std::ios::binary);
        out << "import io;\nimport math;\nimport fs;\n" << source;
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

void test_stdlib_table_full_dispatch()
{
    TEST(stdlib_table_full_dispatch);
    //Step 1 table<->id<->TU binding guard: compile AND run one real
    //program per math table entry. A table row pointing at an id the
    //VM does not dispatch compiles fine but throws "unknown intrinsic"
    //at run time — the static_asserts in StdLib.h bind the id block,
    //this walk proves every row actually executes.
    int checked = 0;
    for (const auto& entry : kStdLibTable)
    {
        if (std::string(entry.ns) != "math")
            continue;
        //Argument literals per declared kind.
        std::string args;
        for (int i = 0; i < entry.maxArgs; ++i)
            args += (i ? ", " : "")
                + std::string(entry.paramKinds[i] == RTK_Int32
                    ? "1" : "1.5");
        std::string call = std::string(entry.ns) + "."
            + entry.name + "(" + args + ")";
        std::string src;
        if (entry.returnType == SLRT_Void)
            src = "int main() { " + call + "; return 0; }\n";
        else if (entry.returnType == SLRT_Float)
            src = "int main() { float r = " + call + "; return 0; }\n";
        else
            src = "int main() { int r = " + call + "; return 0; }\n";
        //Unique output tag per entry: the builder's module registry
        //rejects a second module with the same name in one process.
        const int rc = runSource(std::string("tbl_") + entry.name, src);
        CHECK(rc == 0, (std::string("dispatch failed for ") + call
            + " (rc=" + std::to_string(rc) + ")").c_str());
        ++checked;
    }
    CHECK(checked == kMathIntrinsicCount,
        "every math table entry must be exercised by this walk");
    PASS();
}

void test_io_print_accepts_primitives()
{
    TEST(io_print_accepts_primitives);
    //coerceToString: literals and typed variables of all three accepted
    //kinds compile and run (stdout noise "a7 0.5" lands in the test log).
    const int rc = runSource("io_print_ok",
        "int main() {\n"
        "    string s = \"a\";\n"
        "    int n = 7;\n"
        "    float f = 0.5;\n"
        "    io.print(s);\n"
        "    io.print(n);\n"
        "    io.print(f);\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "io.print should accept string/int/float");
    PASS();
}

void test_io_print_void_not_assignable()
{
    TEST(io_print_void_not_assignable);
    BuildOutcome outcome = buildSource("io_print_void",
        "int main() { int x = io.print(\"a\"); return x; }\n");
    CHECK(!outcome.ok, "void io.print result must not be assignable");
    PASS();
}

void test_io_print_rejects_nonprintable()
{
    TEST(io_print_rejects_nonprintable);
    //class values must call .toString() explicitly; null would print "0";
    //arrays masquerade as their element kind via EvalDataType and are
    //rejected by the IsArrayValuedExpr guard before the coercion branch.
    BuildOutcome cls = buildSource("io_print_cls",
        "class P { int x; }\n"
        "int main() { P p = new P{1}; io.print(p); return 0; }\n");
    CHECK(!cls.ok, "io.print(class) must not compile");
    CHECK(cls.diagnostics.find("toString") != std::string::npos,
        "diagnostic should point at .toString()");

    BuildOutcome nil = buildSource("io_print_null",
        "int main() { io.print(null); return 0; }\n");
    CHECK(!nil.ok, "io.print(null) must not compile");

    BuildOutcome arr = buildSource("io_print_arr",
        "int main() { int[] a = new int[2]; io.print(a); return 0; }\n");
    CHECK(!arr.ok, "io.print(int[]) must not compile");
    CHECK(arr.diagnostics.find("array") != std::string::npos,
        "diagnostic should name the array problem");
    PASS();
}

void test_io_readfile_missing_catchable()
{
    TEST(io_readfile_missing_catchable);
    //IOException raised by io.readFile is a normal catchable Exception
    //subclass in-process (message field populated by the ctor intrinsic).
    const int rc = runSource("io_rf_catch",
        "int main() {\n"
        "    try {\n"
        "        string s = io.readFile(\"_no_such_file_.txt\");\n"
        "        return 1;\n"
        "    } catch (IOException e) {\n"
        "        if (e.message == \"\") return 2;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "missing-file IOException should be catchable");
    PASS();
}

void test_stdlib_void_arg_rejected()
{
    TEST(stdlib_void_arg_rejected);
    //Step 2 review MAJOR: a resolved void call as a stdlib argument used
    //to pass the !pArgType branch silently (stale pResult in the claim
    //slot). Both the coercing (io.print) and strict (math.sqrt) paths
    //share the guard, so pin both.
    BuildOutcome ioArg = buildSource("void_arg_io",
        "void f() { }\n"
        "int main() { io.print(f()); return 0; }\n");
    CHECK(!ioArg.ok, "io.print(voidCall()) must not compile");
    CHECK(ioArg.diagnostics.find("void") != std::string::npos,
        "diagnostic should name the void problem");

    BuildOutcome mathArg = buildSource("void_arg_math",
        "void f() { }\n"
        "int main() { float x = math.sqrt(f()); return 0; }\n");
    CHECK(!mathArg.ok, "math.sqrt(voidCall()) must not compile");
    PASS();
}

// --- Phase 11 Step 3: string methods ---

void test_string_table_full_dispatch()
{
    TEST(string_table_full_dispatch);
    //Same binding guard as the math walk: compile AND run one real
    //program per kStringMethodTable entry. Receiver "12" survives every
    //method (toInt/toFloat parse it; substring/indexOf/etc are total).
    //substring is called with maxArgs here; the 1-arg synthetic-end path
    //(STD_ReceiverLength) is pinned by the e2e suite.
    int checked = 0;
    for (const auto& entry : kStringMethodTable)
    {
        std::string args;
        for (int i = 0; i < entry.maxArgs; ++i)
            args += (i ? ", " : "")
                + std::string(entry.paramKinds[i] == RTK_Int32
                    ? "1" : "\"x\"");
        std::string call = std::string("\"12\".") + entry.name
            + "(" + args + ")";
        std::string src;
        switch ((StdLibReturnType)entry.returnType)
        {
        case SLRT_String:
            src = "int main() { string r = " + call + "; return 0; }\n";
            break;
        case SLRT_Int32:
            src = "int main() { int r = " + call + "; return 0; }\n";
            break;
        case SLRT_Float:
            src = "int main() { float r = " + call + "; return 0; }\n";
            break;
        case SLRT_ListString:
            src = "int main() { List<string> r = " + call
                + "; return 0; }\n";
            break;
        case SLRT_Void:
            src = "int main() { " + call + "; return 0; }\n";
            break;
        }
        const int rc = runSource(std::string("strtbl_") + entry.name, src);
        CHECK(rc == 0, (std::string("dispatch failed for ") + call
            + " (rc=" + std::to_string(rc) + ")").c_str());
        ++checked;
    }
    CHECK(checked == kStringMethodIntrinsicCount,
        "every string-method table entry must be exercised by this walk");
    PASS();
}

void test_string_equals_gethashcode_migrated()
{
    TEST(string_equals_gethashcode_migrated);
    //Phase 8e-1 protocol methods after the IntrinsicsString.cpp move:
    //ids 42/43 unchanged, value semantics unchanged.
    const int rc = runSource("str_proto",
        "int main() {\n"
        "    if (!(\"a\".equals(\"a\"))) return 1;\n"
        "    if (\"a\".equals(\"b\")) return 2;\n"
        "    if (\"a\".getHashCode() != \"a\".getHashCode()) return 3;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "equals/getHashCode must keep working after the move");
    PASS();
}

void test_string_method_type_error()
{
    TEST(string_method_type_error);
    //Exact kind policy: a float substring offset is a compile error (no
    //int<-float narrowing), and a string arg to an int param is too.
    BuildOutcome floatOff = buildSource("str_type_f",
        "int main() { string r = \"a\".substring(1.5); return 0; }\n");
    CHECK(!floatOff.ok, "\"a\".substring(1.5) must not compile");
    CHECK(floatOff.diagnostics.find("substring") != std::string::npos,
        "diagnostic should name the method");

    BuildOutcome strArg = buildSource("str_type_s",
        "int main() { int r = \"a\".indexOf(5); return 0; }\n");
    CHECK(!strArg.ok, "\"a\".indexOf(5) must not compile");
    CHECK(strArg.diagnostics.find("indexOf") != std::string::npos,
        "diagnostic should name the method");
    PASS();
}

void test_string_array_arg_rejected()
{
    TEST(string_array_arg_rejected);
    //string[] masquerades as string via EvalDataType (element kind) —
    //the IsArrayValuedExpr guard must reject it before the kind check.
    BuildOutcome outcome = buildSource("str_arg_arr",
        "int main() {\n"
        "    string[] a = new string[2];\n"
        "    int r = \"x\".indexOf(a);\n"
        "    return 0;\n"
        "}\n");
    CHECK(!outcome.ok, "\"x\".indexOf(string[]) must not compile");
    CHECK(outcome.diagnostics.find("array") != std::string::npos,
        "diagnostic should name the array problem");
    PASS();
}

void test_string_void_arg_rejected()
{
    TEST(string_void_arg_rejected);
    //Same void-arg guard as the namespace path, on the method branch.
    BuildOutcome outcome = buildSource("str_arg_void",
        "void f() { }\n"
        "int main() { int r = \"a\".indexOf(f()); return 0; }\n");
    CHECK(!outcome.ok, "\"a\".indexOf(voidCall()) must not compile");
    CHECK(outcome.diagnostics.find("void") != std::string::npos,
        "diagnostic should name the void problem");
    PASS();
}

void test_string_arity_error()
{
    TEST(string_arity_error);
    //substring's 1..2 range gets the range wording; indexOf's exact 1
    //gets the exact wording.
    BuildOutcome range = buildSource("str_arity_r",
        "int main() { string r = \"a\".substring(); return 0; }\n");
    CHECK(!range.ok, "\"a\".substring() must not compile");
    CHECK(range.diagnostics.find("substring") != std::string::npos,
        "diagnostic should name the method");

    BuildOutcome exact = buildSource("str_arity_e",
        "int main() { int r = \"a\".indexOf(); return 0; }\n");
    CHECK(!exact.ok, "\"a\".indexOf() must not compile");
    CHECK(exact.diagnostics.find("indexOf") != std::string::npos,
        "diagnostic should name the method");
    PASS();
}

void test_string_unknown_method_still_errors()
{
    TEST(string_unknown_method_still_errors);
    //Regression guard: an unknown string method must fall through to the
    //normal member-resolution error, not be silently accepted.
    BuildOutcome outcome = buildSource("str_unknown",
        "int main() { int r = \"a\".nope(); return 0; }\n");
    CHECK(!outcome.ok, "\"a\".nope() must not compile");
    CHECK(outcome.diagnostics.find("nope") != std::string::npos,
        "diagnostic should name the unknown method");
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
        test_stdlib_table_full_dispatch();
        test_io_print_accepts_primitives();
        test_io_print_void_not_assignable();
        test_io_print_rejects_nonprintable();
        test_io_readfile_missing_catchable();
        test_stdlib_void_arg_rejected();
        test_string_table_full_dispatch();
        test_string_equals_gethashcode_migrated();
        test_string_method_type_error();
        test_string_array_arg_rejected();
        test_string_void_arg_rejected();
        test_string_arity_error();
        test_string_unknown_method_still_errors();
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
