// RTK_ArrayView view-record unit tests (v0.12.7 Task 8): real VmExecutor +
// real heap. Direct AllocViewOnHeap calls assert the record layout (shared
// header prefix, unpadded view tail), the creation validation matrix
// (rank bounds, negative dims, cap, coverage, type identity, null/wrong
// kind), depth-one folding, and the GC edge (a rooted view holds its
// target through collections; dropped together they free together).
// Mirrors test_native_array_abi.cpp's in-process compile+load+link
// pipeline — including its static-init contract: Runtime::StaticInit owns
// the IdString intern tables and TypeCastInfo::StaticInit the cast table;
// both are required before any in-process ModuleBuilder use.

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include <nlang/compiler/CastInfo.h>
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/vm/NativeHost.h"
#include "VmExecutor.h"
#include "VmExecutorNativeHost.h"
#include "NcuLoader.h"
#include "NcuLinker.h"
#include <cstring>
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

#define TEST(name) \
    do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() \
    do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) \
    do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

static std::filesystem::path scratchDir()
{
    static const auto dir = std::filesystem::temp_directory_path()
        / "nlang_test_array_view_record";
    std::filesystem::create_directories(dir);
    return dir;
}

struct ExecWithModule {
    std::unique_ptr<VmExecutor> exec;
    std::unique_ptr<CompiledModule> mod;
    VmNativeHost host;
};

//CHECK does a bare `return`, so this helper returns void and fills an
//out-param (no value-returning function may CHECK mid-way). The fixture
//declares every (element, rank) row the tests mint against, and imports
//io so the Exception family registers (the rejection matrix catches the
//creation family).
static void makeExecutorWithArrayModule(const std::string& tag,
                                        ExecWithModule& out)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);
    {
        std::ofstream out(nPath, std::ios::binary);
        out << "import io;\n"
               "int main() {\n"
               "    int[] a1 = int[4];\n"
               "    int[,] a2 = int[2, 3];\n"
               "    int[,,] a3 = int[2, 2, 2];\n"
               "    double[] d1 = double[2];\n"
               "    double[,] d2 = double[1, 1];\n"
               "    return 0;\n"
               "}\n";
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
        FAIL("view fixture must build");
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            std::cerr << "    " << (*it)->Message() << "\n";
        return;
    }
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
    NcuLoader::Result loaded =
        NcuLoader::LoadClosure(modPath.string(), loaderOpts);
    out.mod = std::make_unique<CompiledModule>(
        NcuLinker::Link(std::move(loaded.units), loaded.entryKey));
    out.exec = std::make_unique<VmExecutor>();
    out.exec->InitializeForRun(*out.mod);
    out.exec->InitNativeHost(out.host);
}

//Mint an owned scalar array through the production-shaped host callback
//(the v9 shaped-mint face) and return its heap handle. The minting
//callback roots nothing outside a blocking-native window, so handles are
//collectible exactly when the test's host roots say so.
static int32_t MintOwnedArray(ExecWithModule& env, uint8_t elemKind,
                              int32_t rank, const int32_t* dims)
{
    return env.host.c.newArrayShaped(&env.host.c, elemKind, rank, dims);
}

//ArrayTypes row of the given minted record ([1] = arrayTypeIdx).
static uint16_t TypeIdxOf(ExecWithModule& env, int32_t handle)
{
    return static_cast<uint16_t>(
        env.exec->ReadHeapRecordCellForTests(handle, 1));
}

//arrayTypes row for a scalar element kind at a given rank: mint an owned
//array of that shape through the host callback and read its [1] cell
//(the row registers when the fixture's declarations compile).
static uint16_t RowFor(ExecWithModule& env, uint8_t elemKind, int32_t rank,
                       const int32_t* dims)
{
    return TypeIdxOf(env, MintOwnedArray(env, elemKind, rank, dims));
}

//True iff AllocViewOnHeap rejects the arguments with the creation family
//(catchable IndexOutOfBoundsException). A clean return or a host-level
//error both count as "not the creation family".
static bool RejectsWithCreationFamily(ExecWithModule& env,
                                      uint16_t typeIdx, uint8_t rank,
                                      const int32_t* dims,
                                      const int32_t* strides,
                                      int32_t offset, int32_t target)
{
    try {
        env.exec->AllocViewOnHeap(typeIdx, rank, dims, strides,
                                  offset, target);
        return false;
    } catch (const NLangThrow& e) {
        return std::strstr(e.what(), "IndexOutOfBoundsException") != nullptr;
    }
}

//One keep/drop GC round: mint a fresh target and a view over it, root
//ONLY the view, churn garbage, collect — target and view must both
//survive with the target edge intact; drop the root, collect again —
//both must free (the view without its target, or surviving alone, would
//mean the edge or the view arm is wrong).
static void GcChurnRound(ExecWithModule& env, const std::string& roundTag,
                         bool& ok, std::string& why)
{
    const int32_t dims1[1] = {4};
    const int32_t strides1[1] = {1};
    const int32_t garbageDims[1] = {64};
    const int32_t t = MintOwnedArray(env, NEK_Int32, 1, dims1);
    const uint16_t typeIdx = TypeIdxOf(env, t);
    const int32_t v = env.exec->AllocViewOnHeap(typeIdx, 1, dims1, strides1,
                                                0, t);
    env.exec->PushHostRoot(RTK_ArrayView, v);
    //Churn: fresh garbage the sweep must free around the kept pair.
    for (int i = 0; i < 4; ++i)
        MintOwnedArray(env, NEK_Int32, 1, garbageDims);
    env.exec->CollectGarbageForTests();
    const uint8_t tKind = env.exec->HeapSlotKindForTests(t);
    const uint8_t vKind = env.exec->HeapSlotKindForTests(v);
    if (tKind != RTK_Array || vKind != RTK_ArrayView) {
        ok = false;
        why = roundTag + ": premature collection (target kind "
            + std::to_string(static_cast<int>(tKind)) + ", view kind "
            + std::to_string(static_cast<int>(vKind)) + ")";
        env.exec->PopHostRoot(RTK_ArrayView, v);
        return;
    }
    if (env.exec->ReadHeapRecordCellForTests(v, ArrayViewTargetCell(1)) != t) {
        ok = false;
        why = roundTag + ": view target edge broken after collection";
        env.exec->PopHostRoot(RTK_ArrayView, v);
        return;
    }
    env.exec->PopHostRoot(RTK_ArrayView, v);
    env.exec->CollectGarbageForTests();
    if (env.exec->HeapSlotKindForTests(t) != 0
        || env.exec->HeapSlotKindForTests(v) != 0) {
        ok = false;
        why = roundTag + ": dropped pair did not free together (target kind "
            + std::to_string(static_cast<int>(
                env.exec->HeapSlotKindForTests(t)))
            + ", view kind " + std::to_string(static_cast<int>(
                env.exec->HeapSlotKindForTests(v))) + ")";
        return;
    }
    ok = true;
}

//--- tests --------------------------------------------------------------------

void test_rank1_layout_no_padding()
{
    TEST(rank1_layout_no_padding);
    ExecWithModule env;
    makeExecutorWithArrayModule("view_rank1", env);
    CHECK(env.exec != nullptr, "fixture must initialize");
    const int32_t dims[1] = {4};
    const int32_t strides[1] = {1};
    const int32_t t = MintOwnedArray(env, NEK_Int32, 1, dims);
    const uint16_t typeIdx = TypeIdxOf(env, t);
    const int32_t v = env.exec->AllocViewOnHeap(typeIdx, 1, dims, strides,
                                                0, t);
    CHECK(v > 0, "rank-1 view must mint");
    //Rank 1 pins the unpadded tail: the owned record's padded payload
    //base (ArrayHeaderCells(1) = 6) would put the stride one cell later.
    //Exact shape: [0]kind [1]type [2]rank [3]elemCount [4]dim [5]stride
    //[6]offset [7]target — 8 cells, no pad cell.
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 0) == RTK_ArrayView,
          "cell 0 must carry RTK_ArrayView");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 1) == typeIdx,
          "cell 1 must carry the arrayTypes row");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 2) == 1,
          "cell 2 must carry the rank");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 3) == 4,
          "cell 3 must carry elemCount");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 4) == 4,
          "cell 4 must carry dim 0");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, ArrayViewStridesBase(1)) == 1,
          "stride must sit at 4+rank (no even-padding)");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 6) == 0,
          "offset cell must follow the strides run");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 7) == t,
          "target cell must name the owned record");
    PASS();
}

void test_rank2_layout_full_fields()
{
    TEST(rank2_layout_full_fields);
    ExecWithModule env;
    makeExecutorWithArrayModule("view_rank2", env);
    CHECK(env.exec != nullptr, "fixture must initialize");
    const int32_t dims[2] = {2, 2};
    const int32_t strides[2] = {3, 1};
    const int32_t targetDims[2] = {2, 3};
    const int32_t target = MintOwnedArray(env, NEK_Int32, 2, targetDims);
    const uint16_t typeIdx = TypeIdxOf(env, target);
    const int32_t v = env.exec->AllocViewOnHeap(typeIdx, 2, dims, strides,
                                                1, target);
    CHECK(v > 0, "rank-2 view must mint");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 0) == RTK_ArrayView,
          "cell 0 must carry RTK_ArrayView");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 1) == typeIdx,
          "cell 1 must carry the arrayTypes row");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 2) == 2,
          "cell 2 must carry the rank");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 3) == 4,
          "cell 3 must carry elemCount (2x2)");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 4) == 2
          && env.exec->ReadHeapRecordCellForTests(v, 5) == 2,
          "cells 4..5 must carry the dims run");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 6) == 3
          && env.exec->ReadHeapRecordCellForTests(v, 7) == 1,
          "cells 6..7 must carry the strides run (4+rank base)");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 8) == 1,
          "cell 8 must carry the offset");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 9) == target,
          "cell 9 must name the target");
    PASS();
}

void test_negative_stride_accepted()
{
    TEST(negative_stride_accepted);
    ExecWithModule env;
    makeExecutorWithArrayModule("view_negstride", env);
    CHECK(env.exec != nullptr, "fixture must initialize");
    //Reverse-last-axis view of [2,2,2]: element (i,j,k) = source
    //(i,j,1-k) -> offset 1, strides {4, 2, -1}. min address 0, max 7.
    const int32_t dims[3] = {2, 2, 2};
    const int32_t strides[3] = {4, 2, -1};
    const int32_t target = MintOwnedArray(env, NEK_Int32, 3, dims);
    const uint16_t typeIdx = TypeIdxOf(env, target);
    const int32_t v = env.exec->AllocViewOnHeap(typeIdx, 3, dims, strides,
                                                1, target);
    CHECK(v > 0, "a negative-stride view must create (negative axes are "
                 "writable, spec 4.2)");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 9) == -1,
          "the negative stride must store verbatim");
    PASS();
}

void test_zero_stride_accepted()
{
    TEST(zero_stride_accepted);
    ExecWithModule env;
    makeExecutorWithArrayModule("view_zerostride", env);
    CHECK(env.exec != nullptr, "fixture must initialize");
    //Broadcast view: axis 0 repeats (stride 0) — legal at creation, the
    //write-through rejection lives in the store op (Task 10).
    const int32_t dims[2] = {2, 3};
    const int32_t strides[2] = {0, 1};
    const int32_t target = MintOwnedArray(env, NEK_Int32, 2, dims);
    const uint16_t typeIdx = TypeIdxOf(env, target);
    const int32_t v = env.exec->AllocViewOnHeap(typeIdx, 2, dims, strides,
                                                0, target);
    CHECK(v > 0, "a zero-stride (broadcast) view must create");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, 6) == 0,
          "the broadcast stride must store verbatim");
    PASS();
}

void test_depth_one_folding()
{
    TEST(depth_one_folding);
    ExecWithModule env;
    makeExecutorWithArrayModule("view_fold", env);
    CHECK(env.exec != nullptr, "fixture must initialize");
    const int32_t dimsT[2] = {2, 3};
    const int32_t dimsV[2] = {1, 2};
    const int32_t stridesV[2] = {3, 1};
    const int32_t target = MintOwnedArray(env, NEK_Int32, 2, dimsT);
    const uint16_t typeIdx = TypeIdxOf(env, target);
    const int32_t v = env.exec->AllocViewOnHeap(typeIdx, 2, dimsV, stridesV,
                                                2, target);
    CHECK(v > 0, "intermediate view must mint");
    //A view created over a view folds to the OWNED record (depth one):
    //the target cell names the target, never the intermediate view. W
    //is rank 1, so its row is the rank-1 int row — a rank-reduced view
    //carries its own row (cells [1] and [2] stay consistent).
    const int32_t dimsW[1] = {1};
    const int32_t stridesW[1] = {1};
    const uint16_t rowW = RowFor(env, NEK_Int32, 1, dimsW);
    const int32_t w = env.exec->AllocViewOnHeap(rowW, 1, dimsW, stridesW,
                                                5, v);
    CHECK(w > 0, "composed view must mint");
    CHECK(env.exec->ReadHeapRecordCellForTests(w, ArrayViewTargetCell(1))
              == target,
          "composed view's target must be the OWNED record (depth one)");
    CHECK(env.exec->ReadHeapRecordCellForTests(w, 3) == 1,
          "composed view keeps its own elemCount");
    CHECK(env.exec->ReadHeapRecordCellForTests(w, 1) == rowW,
          "composed view carries its own (rank-1) arrayTypes row");
    //Owner-space contract pin: W's window ({5}) lies outside V's window
    //({2, 3}) but inside the owner — legal, because view numbers are
    //owner-payload positions and coverage validates against the OWNER.
    bool rejected = false;
    try {
        const int32_t dimsFar[1] = {1};
        const int32_t stridesFar[1] = {1};
        env.exec->AllocViewOnHeap(rowW, 1, dimsFar, stridesFar, 99, v);
    } catch (const NLangThrow&) {
        rejected = true;
    }
    CHECK(rejected, "coverage beyond the OWNER must still reject "
                    "(99 lies outside the 6-element owner)");
    //Row-rank consistency: the owner's rank-2 row for a rank-1 view is
    //a row/type mismatch ([1] and [2] would disagree) — rejected.
    CHECK(RejectsWithCreationFamily(env, typeIdx, 1, dimsW, stridesW,
                                    5, v),
          "a rank-2 row on a rank-1 view must reject (row rank must "
          "match the view's rank)");
    PASS();
}

void test_validation_reject_matrix()
{
    TEST(validation_reject_matrix);
    ExecWithModule env;
    makeExecutorWithArrayModule("view_reject", env);
    CHECK(env.exec != nullptr, "fixture must initialize");
    const int32_t dims23[2] = {2, 3};
    const int32_t target = MintOwnedArray(env, NEK_Int32, 2, dims23);
    const uint16_t typeIdx = TypeIdxOf(env, target);

    const int32_t stridesFull[2] = {3, 1};
    //rank 0 and rank beyond the cap.
    {
        const int32_t dims0[1] = {4};
        CHECK(RejectsWithCreationFamily(env, typeIdx, 0, dims0, dims0,
                                        0, target),
              "rank 0 must reject");
        const int32_t dimsBig[65] = {0};
        const int32_t stridesBig[65] = {0};
        CHECK(RejectsWithCreationFamily(env, typeIdx, 65, dimsBig,
                                        stridesBig, 0, target),
              "rank 65 (> kMaxArrayRank) must reject");
    }
    //Negative dim and product over the element cap.
    {
        const int32_t dimsNeg[2] = {2, -3};
        CHECK(RejectsWithCreationFamily(env, typeIdx, 2, dimsNeg,
                                        stridesFull, 0, target),
              "a negative dim must reject");
        const int32_t dimsCap[2] = {10000, 10001};
        const int32_t stridesCap[2] = {1, 1};
        CHECK(RejectsWithCreationFamily(env, typeIdx, 2, dimsCap,
                                        stridesCap, 0, target),
              "elemCount over the 100M cap must reject");
    }
    //Coverage: past the end, and below zero.
    {
        CHECK(RejectsWithCreationFamily(env, typeIdx, 2, dims23,
                                        stridesFull, 7, target),
              "max address 12 in a 6-element target must reject");
        const int32_t stridesRev[2] = {3, -1};
        CHECK(RejectsWithCreationFamily(env, typeIdx, 2, dims23,
                                        stridesRev, 0, target),
              "min address -2 must reject");
    }
    //Type identity at the element level: a same-rank double row over an
    //int target must reject (rank alone would not differ).
    {
        const int32_t dbl2Dims[2] = {1, 1};
        const uint16_t dbl2Idx = RowFor(env, NEK_Float64, 2, dbl2Dims);
        CHECK(RejectsWithCreationFamily(env, dbl2Idx, 2, dims23,
                                        stridesFull, 0, target),
              "a same-rank double row over an int target must reject "
              "(element mismatch)");
    }
    //A cross-rank row of the SAME element over the int target is legal
    //when the row's rank equals the view's rank (the rank-reduced view
    //of the fold test exercised this; pin it here in isolation).
    {
        const int32_t dims1[1] = {4};
        const int32_t strides1[1] = {1};
        const uint16_t row1 = RowFor(env, NEK_Int32, 1, dims1);
        CHECK(!RejectsWithCreationFamily(env, row1, 1, dims1, strides1,
                                         0, target),
              "a rank-1 int row over the rank-2 int owner must ACCEPT "
              "(rank may change; the element must not)");
    }
    //Null target: the null-array family (NPE), not the creation family.
    {
        bool caught = false;
        try {
            const int32_t dims1[1] = {1};
            const int32_t strides1[1] = {1};
            env.exec->AllocViewOnHeap(typeIdx, 1, dims1, strides1, 0, 0);
        } catch (const NLangThrow& e) {
            caught = std::strstr(e.what(),
                "NullPointerException") != nullptr;
        }
        CHECK(caught, "a null target handle must raise the NPE family");
    }
    //A non-array heap record (a List instance) as target: creation family.
    {
        const int32_t listIdx = env.exec->AllocHostList();
        CHECK(RejectsWithCreationFamily(env, typeIdx, 1, dims23,
                                        stridesFull, 0, listIdx),
              "a non-array target record must reject");
    }
    //An out-of-range target handle is a host-level defect, not a
    //language error: std::runtime_error, like the other record APIs.
    {
        bool hostErr = false;
        try {
            const int32_t dims1[1] = {1};
            const int32_t strides1[1] = {1};
            env.exec->AllocViewOnHeap(typeIdx, 1, dims1, strides1, 0,
                                      0x7FFFFFF0);
        } catch (const NLangThrow&) {
            hostErr = false;
        } catch (const std::runtime_error&) {
            hostErr = true;
        }
        CHECK(hostErr, "an out-of-range target handle must be a "
                       "host-level std::runtime_error");
    }
    PASS();
}

void test_gc_view_holds_target()
{
    TEST(gc_view_holds_target);
    ExecWithModule env;
    makeExecutorWithArrayModule("view_gc", env);
    CHECK(env.exec != nullptr, "fixture must initialize");
    //Payload marker on the target proves the sweep did not free (or
    //disturb) the element storage while the view held it.
    const int32_t dims1[1] = {4};
    const int32_t t = MintOwnedArray(env, NEK_Int32, 1, dims1);
    uint8_t cell[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const int32_t marker = 42;
    std::memcpy(cell, &marker, sizeof(marker));
    env.exec->HostArraySetCell(t, 1, cell);
    const uint16_t typeIdx = TypeIdxOf(env, t);
    const int32_t strides1[1] = {1};
    const int32_t v = env.exec->AllocViewOnHeap(typeIdx, 1, dims1, strides1,
                                                0, t);
    env.exec->PushHostRoot(RTK_ArrayView, v);
    env.exec->CollectGarbageForTests();
    CHECK(env.exec->HeapSlotKindForTests(t) == RTK_Array,
          "target must survive while its view is rooted");
    CHECK(env.exec->HeapSlotKindForTests(v) == RTK_ArrayView,
          "rooted view must survive");
    CHECK(env.exec->ReadHeapRecordCellForTests(v, ArrayViewTargetCell(1))
              == t,
          "the target edge must survive collection");
    uint8_t readBack[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    env.exec->HostArrayGetCell(t, 1, readBack);
    int32_t decoded = 0;
    std::memcpy(&decoded, readBack, sizeof(decoded));
    CHECK(decoded == 42, "target payload must survive collection");
    env.exec->PopHostRoot(RTK_ArrayView, v);
    env.exec->CollectGarbageForTests();
    CHECK(env.exec->HeapSlotKindForTests(v) == 0,
          "the dropped view must free");
    CHECK(env.exec->HeapSlotKindForTests(t) == 0,
          "the target must free once no view holds it");
    PASS();
}

void test_gc_churn_50_rounds()
{
    TEST(gc_churn_50_rounds);
    ExecWithModule env;
    makeExecutorWithArrayModule("view_gc_churn", env);
    CHECK(env.exec != nullptr, "fixture must initialize");
    //Review Focus 2: keep/drop stress without the --gc-stress flag (the
    //flag's micro-collections act as a scheduling barrier and compress
    //the race window). Fifty keep/drop rounds, garbage churn in both
    //phases, every round asserting survival, the intact edge, and the
    //paired free.
    for (int round = 0; round < 50; ++round) {
        bool ok = false;
        std::string why;
        GcChurnRound(env, "round " + std::to_string(round), ok, why);
        if (!ok) {
            FAIL(why);
            return;
        }
    }
    PASS();
}

int main()
{
    //Static tables before any compiler work (test_stdlib.cpp / ncc /
    //ScriptRunner.h contract): the IdString intern tables and the cast
    //table have no lazy guards — in-process builds dereference both.
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    std::cerr << "=== array view record tests ===\n";
    try {
        test_rank1_layout_no_padding();
        test_rank2_layout_full_fields();
        test_negative_stride_accepted();
        test_zero_stride_accepted();
        test_depth_one_folding();
        test_validation_reject_matrix();
        test_gc_view_holds_target();
        test_gc_churn_50_rounds();
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
