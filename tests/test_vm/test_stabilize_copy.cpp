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
#include <thread>
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

//Locate the two P records by their cell fingerprints and report P's
//struct-table index. Field i lives at cell 2i (the uniform 2-cell
//stride), so x is cell 0 and y is cell 2.
static bool findRecords(VmExecutor& exec, int32_t pX, int32_t pY,
                        int32_t qX, int32_t qY, int32_t& pH, int32_t& qH,
                        uint16_t& structIdx)
{
    const int32_t count =
        static_cast<int32_t>(exec.HeapSlotCountForTests());
    for (int32_t h = 1; h < count; ++h) {
        if (exec.HeapSlotKindForTests(h) != RTK_Struct)
            continue;
        if (exec.ReadHeapRecordCellForTests(h, 0) == pX
            && exec.ReadHeapRecordCellForTests(h, 2) == pY)
            pH = h;
        if (exec.ReadHeapRecordCellForTests(h, 0) == qX
            && exec.ReadHeapRecordCellForTests(h, 2) == qY)
            qH = h;
        if (pH != 0 && qH != 0) {
            structIdx = exec.HeapStructIdxForTests(pH);
            return structIdx != 0xFFFF;
        }
    }
    return false;
}

static const char kSrcNested[] = R"(struct Inner {
    int a;
    int b;
}

struct Outer {
    Inner i;
    int c;
}

int main() {
    Outer p;
    p.i.a = 11;
    p.i.b = 12;
    p.c = 103;
    Outer q;
    q.i.a = 21;
    q.i.b = 22;
    q.c = 203;
    return q.c;
}
)";

//Locate the two Outer records: c at cell 2 (103/203 disambiguate from
//any Inner record) and the Inner child at cell 0 carrying (a, b) at its
//own cells (0, 2).
static bool findOuterRecords(VmExecutor& exec, int32_t& pH, int32_t& qH,
                             uint16_t& structIdx)
{
    const int32_t count =
        static_cast<int32_t>(exec.HeapSlotCountForTests());
    for (int32_t h = 1; h < count; ++h) {
        if (exec.HeapSlotKindForTests(h) != RTK_Struct)
            continue;
        const int32_t c = exec.ReadHeapRecordCellForTests(h, 2);
        if (c != 103 && c != 203)
            continue;
        const int32_t innerH = exec.ReadHeapRecordCellForTests(h, 0);
        if (innerH <= 0 || innerH >= count
            || exec.HeapSlotKindForTests(innerH) != RTK_Struct)
            continue;
        const int32_t a = exec.ReadHeapRecordCellForTests(innerH, 0);
        const int32_t b = exec.ReadHeapRecordCellForTests(innerH, 2);
        if (a == 11 && b == 12 && c == 103)
            pH = h;
        if (a == 21 && b == 22 && c == 203)
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
    REQUIRE(findRecords(*r.exec, 1, 2, 3, 4, pH, qH, structIdx));
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
    REQUIRE(findRecords(*r.exec, 1, 2, 3, 4, pH, qH, structIdx));
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
    REQUIRE(findRecords(*r.exec, 1, 2, 3, 4, pH, qH, structIdx));
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

//Nested records: establish allocates the whole subtree fresh (outer +
//inner); the in-place path reuses the dst sub-records — handle stable,
//cells overwritten, zero allocation. A naive recursive entry lock would
//self-deadlock on the child walk — this completing at all is the
//no-self-deadlock guard for the StructCopy leaf lock split.
static void testNestedRecords()
{
    TEST(nestedRecords);
    ExecResult r = runProgram("stabilize_copy_nested", kSrcNested);
    REQUIRE(r.built);
    CHECK(r.exitCode == 203, "program exit");
    int32_t pH = 0, qH = 0;
    uint16_t structIdx = 0;
    REQUIRE(findOuterRecords(*r.exec, pH, qH, structIdx));
    const int32_t pInner = r.exec->ReadHeapRecordCellForTests(pH, 0);
    const size_t before = r.exec->HeapAllocCountForTests();
    const int32_t fresh =
        r.exec->CopyStructInPlaceLocked(0, pH, structIdx);
    CHECK(r.exec->HeapAllocCountForTests() - before == 2,
          "establish allocates the whole subtree (outer + inner)");
    CHECK(r.exec->HeapSlotKindForTests(fresh) == RTK_Struct, "fresh outer");
    CHECK(r.exec->ReadHeapRecordCellForTests(fresh, 2) == 103, "fresh.c");
    const int32_t freshInner =
        r.exec->ReadHeapRecordCellForTests(fresh, 0);
    CHECK(freshInner != pInner, "fresh subtree, not shared with src");
    CHECK(r.exec->HeapSlotKindForTests(freshInner) == RTK_Struct,
          "fresh inner");
    CHECK(r.exec->ReadHeapRecordCellForTests(freshInner, 0) == 11,
          "fresh inner.a");
    CHECK(r.exec->ReadHeapRecordCellForTests(freshInner, 2) == 12,
          "fresh inner.b");

    //In-place nested copy: q's own inner record is REUSED.
    const int32_t qInner = r.exec->ReadHeapRecordCellForTests(qH, 0);
    (void)r.exec->CopyStructInPlaceLocked(qH, pH, structIdx);
    CHECK(r.exec->HeapAllocCountForTests() - before == 2,
          "in-place nested copy allocates nothing");
    CHECK(r.exec->ReadHeapRecordCellForTests(qH, 0) == qInner,
          "dst sub-record reused");
    CHECK(r.exec->ReadHeapRecordCellForTests(qInner, 0) == 11, "q inner.a");
    CHECK(r.exec->ReadHeapRecordCellForTests(qInner, 2) == 12, "q inner.b");
    CHECK(r.exec->ReadHeapRecordCellForTests(qH, 2) == 103, "q.c copied");
    PASS();
}

//Pair-shaped source for the concurrency fixture: both records carry
//x == y so a torn snapshot is detectable as x != y.
static const char kSrcPairs[] = R"(struct P {
    int x;
    int y;
}

int main() {
    P p;
    p.x = 7;
    p.y = 7;
    P q;
    q.x = 9;
    q.y = 9;
    return q.x + q.y;
}
)";

//Concurrent contract fixture: a writer hammers in-place writes of the
//(7,7)/(9,9) pair records at one target while a reader refreshes a
//private snapshot record and asserts the pair invariant. With the
//StructCopy leaf lock every snapshot is an atomic whole-record read, so
//x == y always — the green direction is unconditional (deterministic-
//test ruling). The unlocked failure direction is probabilistic; its
//runnable red is Task 9's e2e stress — this fixture pins the contract
//and guards lock removal regressions.
static void testConcurrentWriterReaderInvariant()
{
    TEST(concurrentWriterReaderInvariant);
    ExecResult r = runProgram("stabilize_copy_race", kSrcPairs);
    REQUIRE(r.built);
    int32_t srcA = 0, srcB = 0;
    uint16_t structIdx = 0;
    REQUIRE(findRecords(*r.exec, 7, 7, 9, 9, srcA, srcB, structIdx));
    const int32_t target =
        r.exec->CopyStructInPlaceLocked(0, srcA, structIdx);
    CHECK(target > 0, "target established");
    const int32_t snapshot =
        r.exec->CopyStructInPlaceLocked(0, srcA, structIdx);
    CHECK(snapshot > 0 && snapshot != target, "snapshot established");
    const size_t before = r.exec->HeapAllocCountForTests();
    const int kIterations = 20000;
    auto writer = [&]() {
        for (int i = 0; i < kIterations; ++i) {
            (void)r.exec->CopyStructInPlaceLocked(
                target, (i & 1) ? srcA : srcB, structIdx);
        }
    };
    auto reader = [&]() {
        for (int i = 0; i < kIterations; ++i) {
            (void)r.exec->CopyStructInPlaceLocked(
                snapshot, target, structIdx);
            if (r.exec->ReadHeapRecordCellForTests(snapshot, 0)
                != r.exec->ReadHeapRecordCellForTests(snapshot, 2))
                ++g_fail;   //torn snapshot — the invariant broke
        }
    };
    std::thread wt(writer), rt(reader);
    wt.join();
    rt.join();
    CHECK(r.exec->HeapAllocCountForTests() - before == 0,
          "steady-state concurrent in-place traffic allocates nothing");
    const int32_t tx = r.exec->ReadHeapRecordCellForTests(target, 0);
    const int32_t ty = r.exec->ReadHeapRecordCellForTests(target, 2);
    CHECK(tx == ty, "target ends on a valid pair");
    PASS();
}

//Local assignment pin (Task 3): the emitted `q = p` must write the
//EXISTING record in place — a 1000-iteration assignment loop allocates
//nothing, so the two program records are the only allocations. The old
//OP_CopyStruct path allocates per assignment (1002) — this pin is the
//task's runnable red.
static void testLocalAssignZeroAlloc()
{
    TEST(localAssignZeroAlloc);
    ExecResult r = runProgram("stabilize_copy_local_assign",
        "struct P {\n"
        "    int x;\n"
        "    int y;\n"
        "}\n"
        "\n"
        "int main() {\n"
        "    P p;\n"
        "    p.x = 1;\n"
        "    p.y = 2;\n"
        "    P q;\n"
        "    q.x = 3;\n"
        "    q.y = 4;\n"
        "    for (int i = 0; i < 1000; i = i + 1) {\n"
        "        q = p;\n"
        "    }\n"
        "    if (q.x == 1 && q.y == 2) {\n"
        "        return 42;\n"
        "    }\n"
        "    return 1;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.exitCode == 42, "program exit");
    CHECK(r.exec->HeapAllocCountForTests() == 2,
          "1000 local assigns allocate nothing (2 records total)");
    PASS();
}

//Field-store pin (Task 4): `obj.f = p` on a class-typed field must
//update the field's EXISTING record in place — a 1000-iteration loop
//allocates nothing. Fixed allocations: obj (class) + its pre-established
//f sub-record (AllocClassOnHeap recursion) + p = 3; the old
//deep-copy-per-store path adds 1000.
static void testFieldStoreZeroAlloc()
{
    TEST(fieldStoreZeroAlloc);
    ExecResult r = runProgram("stabilize_copy_field_store",
        "struct P {\n"
        "    int x;\n"
        "    int y;\n"
        "}\n"
        "class Box {\n"
        "    public P f;\n"
        "}\n"
        "int main() {\n"
        "    Box obj = Box();\n"
        "    P p;\n"
        "    p.x = 1;\n"
        "    p.y = 2;\n"
        "    for (int i = 0; i < 1000; i = i + 1) {\n"
        "        obj.f = p;\n"
        "    }\n"
        "    if (obj.f.x == 1 && obj.f.y == 2) {\n"
        "        return 42;\n"
        "    }\n"
        "    return 1;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.exitCode == 42, "program exit");
    CHECK(r.exec->HeapAllocCountForTests() == 3,
          "1000 field stores allocate nothing (3 records total)");
    PASS();
}

//Element-store pin (Task 5): `arr[0] = p` must update the element's
//EXISTING record in place — a 1000-iteration loop allocates nothing.
//Fixed base (RED-observed): 8 records (e0, e1, the array, its element
//records, init-list staging copies, p); the old deep-copy-per-store
//path adds 1000 on top.
static void testElementStoreZeroAlloc()
{
    TEST(elementStoreZeroAlloc);
    ExecResult r = runProgram("stabilize_copy_elem_store",
        "struct P {\n"
        "    int x;\n"
        "    int y;\n"
        "}\n"
        "int main() {\n"
        "    P e0;\n"
        "    e0.x = 1;\n"
        "    e0.y = 2;\n"
        "    P e1;\n"
        "    e1.x = 3;\n"
        "    e1.y = 4;\n"
        "    P[] arr = [e0, e1];\n"
        "    P p;\n"
        "    p.x = 1;\n"
        "    p.y = 2;\n"
        "    for (int i = 0; i < 1000; i = i + 1) {\n"
        "        arr[0] = p;\n"
        "    }\n"
        "    if (arr[0].x == 1 && arr[0].y == 2) {\n"
        "        return 42;\n"
        "    }\n"
        "    return 1;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.exitCode == 42, "program exit");
    const size_t allocs = r.exec->HeapAllocCountForTests();
    if (allocs != 8) {
        FAIL("1000 element stores allocate nothing (expected 8 fixed "
             "records, got " + std::to_string(allocs) + ")");
        return;
    }
    PASS();
}

//Explicit List.set allocation pin (Task 6): `li.set(0, p)` suppresses
//the emit-side staging copy — the runtime copies into the element's
//EXISTING record in place, so a 1000-iteration loop allocates nothing.
//Fixed base (list + p + the element record from add); the old
//staging-copy-per-call path adds 1000.
static void testListSetExplicitZeroAlloc()
{
    TEST(listSetExplicitZeroAlloc);
    ExecResult r = runProgram("stabilize_copy_list_set",
        "struct P {\n"
        "    int x;\n"
        "    int y;\n"
        "}\n"
        "int main() {\n"
        "    List<P> li = {};\n"
        "    P p;\n"
        "    p.x = 1;\n"
        "    p.y = 2;\n"
        "    li.add(p);\n"
        "    for (int i = 0; i < 1000; i = i + 1) {\n"
        "        li.set(0, p);\n"
        "    }\n"
        "    if (li[0].x == 1 && li[0].y == 2) {\n"
        "        return 42;\n"
        "    }\n"
        "    return 1;\n"
        "}\n");
    REQUIRE(r.built);
    CHECK(r.exitCode == 42, "program exit");
    const size_t allocs = r.exec->HeapAllocCountForTests();
    if (allocs != 3) {
        FAIL("1000 explicit List.set calls allocate nothing (expected 3 "
             "fixed records, got " + std::to_string(allocs) + ")");
        return;
    }
    PASS();
}

int main()
{
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();
    testInPlaceAssignZeroAlloc();
    testSelfAssignNoOp();
    testEstablishOnNullDst();
    testNestedRecords();
    testConcurrentWriterReaderInvariant();
    testLocalAssignZeroAlloc();
    testFieldStoreZeroAlloc();
    testElementStoreZeroAlloc();
    testListSetExplicitZeroAlloc();
    std::cerr << "stabilize_copy: " << g_pass << " passed, "
              << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
