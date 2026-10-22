/*---
    test_struct_ctor.cpp - 0.9.11 D2 struct constructor unit tests.

    In-process compile+run of real .n sources (test_stdlib's runSource
    shape): the struct constructor runs only on explicit construction
    calls — direct-init `S s(a, b);`, bare invoke `S s = S(a, b);`, and
    the zero-arg `S()` form — while a bare declaration `S s;` stays
    zero-initialized without invoking it (D3). The e2e suite pins the
    same shapes through the ncc/nvm CLI; these tests pin the own-unit
    semantics in-process for fast failure localization.
---*/
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include <nlang/compiler/CastInfo.h>
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

//Scratch directory for the temp sources and modules.
static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_struct_ctor";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile + execute one source; returns main()'s value, or -1 when the
//build/link fails. No stdlib imports are prepended: struct constructors
//close over no library signatures, so the closure is the own unit alone.
static int runSource(const std::string& tag, const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
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
    params.m_sStdLibDir = STDLIB_DIR;

    ListCompileLogger logger;
    try {
        ModuleBuilder builder(params, logger);
        if (!builder.BuildArtifacts())
        {
            for (auto it = logger.cbegin(); it != logger.cend(); ++it)
                std::cerr << "build: " << (*it)->Message() << "\n";
            return -1;
        }
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << "\n";
        return -1;
    }

    try {
        NcuLoader::Options loaderOpts;
        loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
        NcuLoader::Result loaded =
            NcuLoader::LoadClosure(modPath.string(), loaderOpts);
        CompiledModule mod = NcuLinker::Link(std::move(loaded.units),
                                             loaded.entryKey);
        VmExecutor exec;
        return exec.Execute(mod);
    } catch (const std::exception& e) {
        std::cerr << "load/link failed: " << e.what() << "\n";
        return -1;
    }
}

void test_struct_ctor_direct_init()
{
    TEST(struct_ctor_direct_init);
    const int rc = runSource("direct_init",
        "struct Point {\n"
        "    int x;\n"
        "    int y;\n"
        "    public Point(int a, int b) { x = a; y = b; }\n"
        "}\n"
        "int main() {\n"
        "    Point p(2, 3);\n"
        "    if (p.x != 2 || p.y != 3) return 1;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "direct-init `Point p(2, 3);` must run the ctor");
    PASS();
}

void test_struct_ctor_invoke_assign()
{
    TEST(struct_ctor_invoke_assign);
    const int rc = runSource("invoke_assign",
        "struct Point {\n"
        "    int x;\n"
        "    int y;\n"
        "    public Point(int a, int b) { x = a; y = b; }\n"
        "}\n"
        "int main() {\n"
        "    Point p = Point(1, 4);\n"
        "    if (p.x != 1 || p.y != 4) return 1;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "bare invoke `Point p = Point(1, 4);` must run the ctor");
    PASS();
}

void test_struct_ctor_default_args()
{
    TEST(struct_ctor_default_args);
    const int rc = runSource("default_args",
        "struct Point {\n"
        "    int x;\n"
        "    int y;\n"
        "    public Point(int a, int b = 7) { x = a; y = b; }\n"
        "}\n"
        "int main() {\n"
        "    Point p(2);\n"
        "    if (p.x != 2 || p.y != 7) return 1;\n"
        "    Point q(2, 3);\n"
        "    if (q.y != 3) return 2;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "ctor default arguments must fill omitted formals");
    PASS();
}

void test_struct_ctor_empty_ctor_not_run_on_decl()
{
    TEST(struct_ctor_empty_ctor_not_run_on_decl);
    //D3: the zero-arg ctor runs on `Flag()` but never on the bare
    //declaration `Flag b;` — the declaration stays zero-initialized.
    const int rc = runSource("empty_ctor",
        "struct Flag {\n"
        "    int v;\n"
        "    public Flag() { v = 9; }\n"
        "}\n"
        "int main() {\n"
        "    Flag a = Flag();\n"
        "    if (a.v != 9) return 1;\n"
        "    Flag b;\n"
        "    if (b.v != 0) return 2;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "`Flag()` runs the zero-arg ctor; `Flag b;` must not");
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
    //interned-name table the resolver indexes fields by.
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    std::cerr << "=== NLang Struct Constructor Unit Tests ===\n\n";

    try {
        test_struct_ctor_direct_init();
        test_struct_ctor_invoke_assign();
        test_struct_ctor_default_args();
        test_struct_ctor_empty_ctor_not_run_on_decl();
    } catch (const std::exception& e) {
        std::cerr << "uncaught: " << e.what() << "\n";
        ++g_fail;
    }

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
