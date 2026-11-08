// v0.13.1 Task 4: object-side instantiation descriptors (D1). The
// executor carries one GenericInstDescriptor* per heap slot (a parallel
// array like m_slotKinds); OP_New's descIdx operand resolves through the
// module's descriptor table (interned from the canonical spelling at run
// start) and AllocClassOnHeap writes the pointer. Real programs compile
// and execute in-process (the test_array_view_record harness pattern,
// including its static-init contract).
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "GenericInstDescriptor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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
        / "nlang_test_generic_runtime_info";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile <tag>.n, load the closure, link, execute, keep the executor
//alive so the heap (and the descriptor parallel array) stays inspectable.
struct ExecResult {
    std::unique_ptr<VmExecutor> exec;
    std::unique_ptr<CompiledModule> mod;
    int exitCode = -1;
    bool built = false;
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
            std::cerr << "    " << (*it)->Message() << "\n";
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

//Heap indices whose parallel-array slot carries a descriptor.
static std::vector<int32_t> heapIndicesWithDescriptor(VmExecutor& exec)
{
    std::vector<int32_t> hits;
    const size_t slots = exec.HeapSlotCountForTests();
    for (int32_t i = 1; i < static_cast<int32_t>(slots); ++i)
        if (exec.InstDescriptorOf(i))
            hits.push_back(i);
    return hits;
}

//A concrete Box<int> construction writes its descriptor on the object
//(the spelling round-trips through the .ncu descriptor table and the
//load-time interning).
static void testBoxIntRidesConstruction()
{
    TEST(boxIntRidesConstruction);
    {
        ExecResult r = runProgram("gri_box_int",
            "class Box<T> {\n"
            "    public T value;\n"
            "    public Box(T v) { value = v; }\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box(41);\n"
            "    return b.value + 1;\n"
            "}\n");
        REQUIRE(r.built);
        CHECK(r.exitCode == 42, "program ran to completion");
        const auto hits = heapIndicesWithDescriptor(*r.exec);
        CHECK(hits.size() == 1, "exactly one descriptor-carrying object");
        if (hits.size() == 1) {
            const GenericInstDescriptor* desc =
                r.exec->InstDescriptorOf(hits[0]);
            CHECK(desc->Spelling() == "Box<int>",
                  "spelling is the canonical Box<int>");
            CHECK(desc->TemplateName() == "Box", "template name is bare");
            CHECK(desc->TypeArgs().size() == 1, "one type argument");
        }
    }
    PASS();
}

//Non-instantiation objects carry no descriptor (0xFFFF descIdx).
static void testPlainObjectCarriesNone()
{
    TEST(plainObjectCarriesNone);
    {
        ExecResult r = runProgram("gri_plain",
            "class Plain {\n"
            "    public int p;\n"
            "    public Plain(int v) { p = v; }\n"
            "}\n"
            "int main() {\n"
            "    Plain q = Plain(5);\n"
            "    return q.p;\n"
            "}\n");
        REQUIRE(r.built);
        CHECK(r.exitCode == 5, "program ran to completion");
        CHECK(heapIndicesWithDescriptor(*r.exec).empty(),
              "no descriptor on a non-generic object");
    }
    PASS();
}

//The probe is total: out-of-range and sentinel handles yield nullptr
//instead of crashing.
static void testProbeBoundsAreNull()
{
    TEST(probeBoundsAreNull);
    {
        ExecResult r = runProgram("gri_bounds",
            "class Box<T> {\n"
            "    public T value;\n"
            "    public Box(T v) { value = v; }\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box(41);\n"
            "    return 0;\n"
            "}\n");
        REQUIRE(r.built);
        CHECK(r.exec->InstDescriptorOf(0) == nullptr,
              "sentinel slot 0 carries no descriptor");
        CHECK(r.exec->InstDescriptorOf(-1) == nullptr,
              "negative handle yields nullptr");
        CHECK(r.exec->InstDescriptorOf(
                  static_cast<int32_t>(r.exec->HeapSlotCountForTests()) + 8)
                  == nullptr,
              "past-end handle yields nullptr");
    }
    PASS();
}

int main()
{
    //Static tables before any compiler work (the test_array_view_record
    //contract): the IdString intern tables and the cast table have no
    //lazy guards — in-process builds dereference both.
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    std::cerr << "=== generic runtime info tests ===\n";
    try {
        testBoxIntRidesConstruction();
        testPlainObjectCarriesNone();
        testProbeBoundsAreNull();
    } catch (const std::exception& e) {
        std::cerr << "FAILED (exception: " << e.what() << ")\n";
        g_fail++;
    }

    std::error_code ec;
    std::filesystem::remove_all(scratchDir(), ec);

    std::cerr << "\n=== Results: " << g_pass << " passed "
              << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
