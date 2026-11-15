//v0.13.9 phase ② Task 1: the record-stabilization write primitive.
//OP_CopyStructInPlace + VmExecutor::CopyStructInPlaceLocked overwrite an
//existing struct record's cells in place (zero allocation); a null dst
//establishes exactly one fresh record. The primitive is exercised
//C++-directly on records a real program built (the test_generic_runtime_info
//harness pattern, including its static-init contract); the emitted-bytecode
//path (dispatch + operand read) is validated in Task 3's e2e once the
//emitter reroutes to the new opcode.
#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/CastInfo.h"
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
        / "nlang_test_stabilize_copy";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile <tag>.n, load the closure, link, execute, keep the executor
//alive so the heap stays inspectable after the run.
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

static const char kSrc[] = R"(struct P {
    int x;
    int y;
}

int main() {
    P p;
    p.x = 1;
    p.y = 2;
    P q;
    q.x = 3;
    q.y = 4;
    return q.x + q.y;
}
)";

//Locate the two P records by their cell fingerprints (p.x == 1, q.x == 3)
//and report P's struct-table index. Field i lives at cell 2i (the uniform
//2-cell stride), so x is cell 0 and y is cell 2.
static bool findRecords(VmExecutor& exec, int32_t& pH, int32_t& qH,
                        uint16_t& structIdx)
{
    const int32_t count =
        static_cast<int32_t>(exec.HeapSlotCountForTests());
    for (int32_t h = 1; h < count; ++h) {
        if (exec.HeapSlotKindForTests(h) != RTK_Struct)
            continue;
        if (exec.ReadHeapRecordCellForTests(h, 0) == 1
            && exec.ReadHeapRecordCellForTests(h, 2) == 2)
            pH = h;
        if (exec.ReadHeapRecordCellForTests(h, 0) == 3
            && exec.ReadHeapRecordCellForTests(h, 2) == 4)
            qH = h;
        if (pH != 0 && qH != 0) {
            structIdx = exec.HeapStructIdxForTests(pH);
            return structIdx != 0xFFFF;
        }
    }
    return false;
}

//1000 in-place assignments allocate nothing and land the src cells in dst.
static void testInPlaceAssignZeroAlloc()
{
    TEST(inplaceAssignZeroAlloc);
    ExecResult r = runProgram("stabilize_copy_basic", kSrc);
    REQUIRE(r.built);
    CHECK(r.exitCode == 7, "program exit");
    int32_t pH = 0, qH = 0;
    uint16_t structIdx = 0;
    REQUIRE(findRecords(*r.exec, pH, qH, structIdx));
    const size_t before = r.exec->HeapAllocCountForTests();
    for (int i = 0; i < 1000; ++i)
        (void)r.exec->CopyStructInPlaceLocked(qH, pH, structIdx);
    CHECK(r.exec->HeapAllocCountForTests() - before == 0,
          "1000 in-place assigns must allocate nothing");
    CHECK(r.exec->ReadHeapRecordCellForTests(qH, 0) == 1, "q.x copied");
    CHECK(r.exec->ReadHeapRecordCellForTests(qH, 2) == 2, "q.y copied");
    CHECK(r.exec->ReadHeapRecordCellForTests(pH, 0) == 1, "p.x untouched");
    PASS();
}

//Self-assignment is a value-preserving no-op: no allocation, cells intact.
static void testSelfAssignNoOp()
{
    TEST(selfAssignNoOp);
    ExecResult r = runProgram("stabilize_copy_self", kSrc);
    REQUIRE(r.built);
    int32_t pH = 0, qH = 0;
    uint16_t structIdx = 0;
    REQUIRE(findRecords(*r.exec, pH, qH, structIdx));
    const size_t before = r.exec->HeapAllocCountForTests();
    (void)r.exec->CopyStructInPlaceLocked(pH, pH, structIdx);
    CHECK(r.exec->HeapAllocCountForTests() - before == 0,
          "self-assign must allocate nothing");
    CHECK(r.exec->ReadHeapRecordCellForTests(pH, 0) == 1, "p.x intact");
    CHECK(r.exec->ReadHeapRecordCellForTests(pH, 2) == 2, "p.y intact");
    PASS();
}

//A null dst establishes exactly one fresh record and returns its handle.
static void testEstablishOnNullDst()
{
    TEST(establishOnNullDst);
    ExecResult r = runProgram("stabilize_copy_establish", kSrc);
    REQUIRE(r.built);
    int32_t pH = 0, qH = 0;
    uint16_t structIdx = 0;
    REQUIRE(findRecords(*r.exec, pH, qH, structIdx));
    const size_t before = r.exec->HeapAllocCountForTests();
    const int32_t fresh =
        r.exec->CopyStructInPlaceLocked(0, pH, structIdx);
    CHECK(r.exec->HeapAllocCountForTests() - before == 1,
          "establish allocates exactly one record");
    CHECK(fresh > 0 && fresh != pH, "fresh handle returned");
    CHECK(r.exec->HeapSlotKindForTests(fresh) == RTK_Struct,
          "fresh record is a struct");
    CHECK(r.exec->ReadHeapRecordCellForTests(fresh, 0) == 1, "fresh.x");
    CHECK(r.exec->ReadHeapRecordCellForTests(fresh, 2) == 2, "fresh.y");
    CHECK(r.exec->ReadHeapRecordCellForTests(pH, 0) == 1, "src untouched");
    PASS();
}

int main()
{
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();
    testInPlaceAssignZeroAlloc();
    testSelfAssignNoOp();
    testEstablishOnNullDst();
    std::cerr << "stabilize_copy: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
