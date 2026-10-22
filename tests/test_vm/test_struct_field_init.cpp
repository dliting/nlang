/*---
    test_struct_field_init.cpp - 0.9.11 D4 struct field initializer unit
    tests.

    In-process compile+run of real .n sources (test_struct_ctor's
    runSource shape): field initializers fold to per-field defaults that
    AllocStructOnHeap applies at EVERY allocation — bare declarations,
    array elements, nested fields — while brace stores, constructor
    stores and deep copies never re-apply them. The e2e suite pins the
    script-observable shapes through the ncc/nvm CLI; these tests pin
    the full folded-tag surface (bool/char/float/double/long), the
    constructor interaction, and GC survival of minted string defaults
    (SetGcStressThresholds, same white-box hook as test_strings).
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
        / "nlang_test_struct_field_init";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile + execute one source; returns main()'s value, or -1 when the
//build/link fails. gcStressRecords > 0 arms the white-box GC stress
//thresholds before Execute (same hook as test_strings) so collection
//runs many times inside the program. No stdlib imports are prepended:
//field defaults close over no library signatures.
static int runSource(const std::string& tag, const std::string& source,
                     size_t gcStressRecords = 0,
                     size_t* liveStringsOut = nullptr)
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
        if (gcStressRecords > 0)
            exec.SetGcStressThresholds(gcStressRecords);
        const int rc = exec.Execute(mod);
        if (liveStringsOut)
            *liveStringsOut = exec.LiveStringObjectCount();
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "load/link failed: " << e.what() << "\n";
        return -1;
    }
}

void test_field_init_bare_decl()
{
    TEST(field_init_bare_decl);
    //A bare declaration reads the defaults; explicit brace entries store
    //over them and a partial entry list leaves the rest at the defaults.
    const int rc = runSource("bare_decl",
        "struct Point {\n"
        "    int x = 3;\n"
        "    int y = 4;\n"
        "    string tag = \"pt\";\n"
        "}\n"
        "int main() {\n"
        "    Point p;\n"
        "    if (p.x != 3 || p.y != 4) return 1;\n"
        "    if (p.tag != \"pt\") return 2;\n"
        "    Point q = Point{9, 8, \"q\"};\n"
        "    if (q.x != 9 || q.y != 8 || q.tag != \"q\") return 3;\n"
        "    Point s = Point{7};\n"
        "    if (s.x != 7 || s.y != 4 || s.tag != \"pt\") return 4;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "bare decl reads defaults; brace entries override");
    PASS();
}

void test_field_init_all_folded_tags()
{
    TEST(field_init_all_folded_tags);
    //Pins the materializer's whole folded-tag surface (the e2e corpora
    //only exercise int/string): bool/char ride the int32 channel,
    //long/float/double their own carriers, and a negative constant
    //folds through OP_Neg. The negative long and the float both fold
    //from a narrower/different literal kind, pinning the declared-kind
    //coercion (sign extension into the long carrier, the double-folded
    //literal re-stamped into the float carrier).
    const int rc = runSource("all_tags",
        "struct All {\n"
        "    bool b = true;\n"
        "    char c = 'x';\n"
        "    int i = -5;\n"
        "    long l = -100;\n"
        "    float f = 1.5;\n"
        "    double d = 2.25;\n"
        "    string s = \"hi\";\n"
        "}\n"
        "int main() {\n"
        "    All a;\n"
        "    int r = 0;\n"
        "    if (a.b) r = r + 1;\n"
        "    if (a.c == 'x') r = r + 2;\n"
        "    if (a.i == -5) r = r + 4;\n"
        "    if (a.l == -100) r = r + 8;\n"
        "    if (a.f == 1.5) r = r + 16;\n"
        "    if (a.d == 2.25) r = r + 32;\n"
        "    if (a.s == \"hi\") r = r + 64;\n"
        "    return r;\n"
        "}\n");
    CHECK(rc == 127, "every folded tag must materialize its value");
    PASS();
}

void test_field_init_array_elements()
{
    TEST(field_init_array_elements);
    //AllocArrayOnHeap allocates every RTK_Struct element through
    //AllocStructOnHeap, so each element carries the defaults; later
    //stores touch only that element.
    const int rc = runSource("array_elems",
        "struct Cell {\n"
        "    int v = 7;\n"
        "    string n = \"c\";\n"
        "}\n"
        "int main() {\n"
        "    Cell[] arr = Cell[3];\n"
        "    if (arr[0].v != 7 || arr[1].v != 7 || arr[2].v != 7) return 1;\n"
        "    if (arr[1].n != \"c\") return 2;\n"
        "    arr[1].v = 9;\n"
        "    if (arr[0].v != 7 || arr[1].v != 9) return 3;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "every array element must get the defaults");
    PASS();
}

void test_field_init_nested()
{
    TEST(field_init_nested);
    //Nested struct fields materialize through the recursive allocation,
    //so the inner defaults apply inside every outer instance — direct
    //declaration and array elements alike.
    const int rc = runSource("nested",
        "struct Inner {\n"
        "    int a = 1;\n"
        "}\n"
        "struct Outer {\n"
        "    Inner val;\n"
        "    int b = 2;\n"
        "}\n"
        "int main() {\n"
        "    Outer o;\n"
        "    if (o.b != 2) return 1;\n"
        "    if (o.val.a != 1) return 2;\n"
        "    Outer[] arr = Outer[2];\n"
        "    if (arr[1].val.a != 1 || arr[1].b != 2) return 3;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "nested fields must materialize the inner defaults");
    PASS();
}

void test_field_init_deepcopy_no_replay()
{
    TEST(field_init_deepcopy_no_replay);
    //Deep copy replays stored values only — defaults are an allocation
    //concern, never re-applied over a field modified after allocation.
    const int rc = runSource("deepcopy",
        "struct Point {\n"
        "    int x = 3;\n"
        "}\n"
        "int main() {\n"
        "    Point p;\n"
        "    p.x = 100;\n"
        "    Point a = p;\n"
        "    if (a.x != 100) return 1;\n"
        "    a.x = 5;\n"
        "    if (p.x != 100) return 2;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "deep copy must carry stored values, not defaults");
    PASS();
}

void test_field_init_ctor_interaction()
{
    TEST(field_init_ctor_interaction);
    //D3 x D4: the constructor runs after allocation (defaults already
    //applied), so its stores win on touched fields while untouched
    //fields keep the defaults; a bare declaration with a user
    //constructor present still reads defaults without running it.
    const int rc = runSource("ctor_interaction",
        "struct Point {\n"
        "    int x = 3;\n"
        "    int y = 4;\n"
        "    public Point(int a) { x = a; }\n"
        "}\n"
        "int main() {\n"
        "    Point p(9);\n"
        "    if (p.x != 9) return 1;\n"
        "    if (p.y != 4) return 2;\n"
        "    Point q;\n"
        "    if (q.x != 3 || q.y != 4) return 3;\n"
        "    return 0;\n"
        "}\n");
    CHECK(rc == 0, "ctor stores override defaults; untouched keep them");
    PASS();
}

void test_field_init_string_gc_stress()
{
    TEST(field_init_string_gc_stress);
    //The string default is a runtime-minted object; the struct local
    //roots it through MarkStructFields, so it survives collection while
    //the loop's unreachable garbage is reclaimed (bounded live count
    //proves collection actually ran).
    size_t liveStrings = 0;
    const int rc = runSource("string_gc_stress",
        "struct Point {\n"
        "    string tag = \"pt\";\n"
        "}\n"
        "int main() {\n"
        "    Point p;\n"
        "    for (int i = 0; i < 500; i = i + 1) {\n"
        "        string t = \"g\" + i;\n"
        "    }\n"
        "    if (p.tag != \"pt\") return 1;\n"
        "    return 0;\n"
        "}\n",
        /*gcStressRecords=*/8, &liveStrings);
    CHECK(rc == 0, "string default must survive GC pressure");
    CHECK(liveStrings < 1000, "loop garbage must have been collected");
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

    std::cerr << "=== NLang Struct Field Initializer Unit Tests ===\n\n";

    try {
        test_field_init_bare_decl();
        test_field_init_all_folded_tags();
        test_field_init_array_elements();
        test_field_init_nested();
        test_field_init_deepcopy_no_replay();
        test_field_init_ctor_interaction();
        test_field_init_string_gc_stress();
    } catch (const std::exception& e) {
        std::cerr << "uncaught: " << e.what() << "\n";
        ++g_fail;
    }

    std::cerr << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
