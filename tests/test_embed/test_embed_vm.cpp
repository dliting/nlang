//VM-level embedding entry tests: CallByName/CallFunctionByIdx + the host
//value bridge, driven directly on VmExecutor with a real ncc-built .ncu
//(no mocks). Runtime::StaticInit first — the IdString tables must exist
//before any module work (in-process host init contract).
#include "NcuLoader.h"
#include "NcuLinker.h"
#include "VmExecutor.h"
#include <nlang/runtime/Runtime.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

using namespace nlang;
namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
#define TEST(name) do { std::cerr << "  " << #name << " ... "; } while(0)
#define PASS() do { ++g_pass; std::cerr << "OK\n"; } while(0)
#define FAIL(msg) do { ++g_fail; std::cerr << "FAIL: " << msg << "\n"; } while(0)
#define CHECK(cond, msg) \
    do { if (!(cond)) { FAIL(msg); return; } } while(0)

//The fixture .ncu is built at build time by the nlang_embed_fixtures
//target; the path arrives as a compile definition (forward slashes).
#ifndef EMBED_VALUES_NCU
#error "EMBED_VALUES_NCU must be defined by the build"
#endif

static VmExecutor& LoadedExecutor() {
    //Both static: the executor's m_currModule must outlive every test —
    //the member-lifetime pattern Interpreter::Impl uses (a stack-local
    //module would dangle after the first test returns).
    static CompiledModule module;
    static VmExecutor executor;
    static bool initialized = false;
    if (!initialized) {
        const fs::path artifact(EMBED_VALUES_NCU);
        NcuLoader::Options opts;
        opts.searchDirs = {artifact.parent_path().string()};
        const NcuLoader::Result loaded =
            NcuLoader::LoadClosure(artifact.string(), opts);
        module = NcuLinker::Link(loaded.units, loaded.entryKey);
        executor.InitializeForRun(module);
        initialized = true;
    }
    return executor;
}

static void TestCallByNameScalars() {
    TEST(TestCallByNameScalars);
    VmExecutor& e = LoadedExecutor();
    uint8_t args[2][8] = {};
    const int32_t a0 = 2, a1 = 3;
    std::memcpy(args[0], &a0, sizeof(a0));
    std::memcpy(args[1], &a1, sizeof(a1));
    uint8_t result[8] = {};
    e.CallByName("values_demo.add", &args[0][0], 2, result);
    int32_t sum = 0;
    std::memcpy(&sum, result, sizeof(sum));
    CHECK(sum == 5, "add(2,3) == 5");
    PASS();
}

static void TestCallByNameFillsDefaults() {
    TEST(TestCallByNameFillsDefaults);
    VmExecutor& e = LoadedExecutor();
    uint8_t args[1][8] = {};
    const int32_t a0 = 1;
    std::memcpy(args[0], &a0, sizeof(a0));
    uint8_t result[8] = {};
    //withDefaults(1) → b、c 由 defaultValues 补齐（5、9）→ 159
    e.CallByName("values_demo.withDefaults", &args[0][0], 1, result);
    int32_t got = 0;
    std::memcpy(&got, result, sizeof(got));
    CHECK(got == 159, "withDefaults(1) == 159 (defaults 5,9)");
    PASS();
}

static void TestCallByNameStringRoundTrip() {
    TEST(TestCallByNameStringRoundTrip);
    VmExecutor& e = LoadedExecutor();
    const int32_t handle = e.MintHostString("embed");
    uint8_t args[1][8] = {};
    std::memcpy(args[0], &handle, sizeof(handle));
    uint8_t result[8] = {};
    e.CallByName("values_demo.greet", &args[0][0], 1, result);
    int32_t retHandle = 0;
    std::memcpy(&retHandle, result, sizeof(retHandle));
    CHECK(e.StrValCopy(retHandle) == "hello, embed", "greet round-trip");
    PASS();
}

static void TestCallByNameSurfacesNLangThrow() {
    TEST(TestCallByNameSurfacesNLangThrow);
    VmExecutor& e = LoadedExecutor();
    uint8_t result[8] = {};
    bool threw = false;
    try {
        e.CallByName("values_demo.boom", nullptr, 0, result);
    } catch (const NLangThrow& t) {
        threw = true;
        //Ground truth: a user `throw` raises NLangThrow with the literal
        //"user throw" (VmExecutorOpsObjects.cpp OpThrow) — the script
        //message lives on the heap object (Exception: cell[1]=message
        //handle). The adapter layer (Task 5) reads it via the field
        //bridge, exactly like this assertion does.
        CHECK(t.heapIdx > 0, "NLangThrow carries the exception heap idx");
        uint16_t msgField = 0;
        CHECK(e.HostFieldNameToIndex(t.heapIdx, "message", msgField),
            "Exception field 'message' resolvable");
        uint8_t msgCell[8] = {};
        e.HostFieldCellGet(t.heapIdx, msgField, msgCell);
        int32_t msgHandle = 0;
        std::memcpy(&msgHandle, msgCell, sizeof(msgHandle));
        CHECK(e.StrValCopy(msgHandle) == "embedded-boom",
            "script message readable via the field bridge");
        CHECK(!e.Backtrace().empty(), "backtrace captured");
    }
    CHECK(threw, "uncaught script throw must surface as NLangThrow");
    PASS();
}

static void TestCallByNameTwiceHeapSurvives() {
    TEST(TestCallByNameTwiceHeapSurvives);
    VmExecutor& e = LoadedExecutor();
    uint8_t result[8] = {};
    try { e.CallByName("values_demo.boom", nullptr, 0, result); } catch (...) {}
    uint8_t args[1][8] = {};
    const int32_t a0 = 4;
    std::memcpy(args[0], &a0, sizeof(a0));
    e.CallByName("values_demo.withDefaults", &args[0][0], 1, result);
    int32_t got = 0;
    std::memcpy(&got, result, sizeof(got));
    CHECK(got == 459, "call after an exception still works (459)");
    PASS();
}

//Extension point ③: a host-held string handle stays a live GC root
//across collections triggered by real bytecode. The stress thresholds
//are clamped to 4 and never restored, so this test must stay LAST in
//main().
static void TestHostRootProtectsAcrossGc() {
    TEST(TestHostRootProtectsAcrossGc);
    VmExecutor& e = LoadedExecutor();
    e.SetGcStressThresholds(4);
    const int32_t handle = e.MintHostString("rooted-survivor");
    e.PushHostRoot(RTK_String, handle);
    //Churn: each greet mints fresh cons nodes, pushing the string store
    //past the clamped threshold — CheckGCSafepoint at ExecuteFunction
    //entry collects. The churn arg stays reachable through the callee
    //frame (staged into locals before entry); the survivor is reachable
    //only via the host root — unregistered, the sweep reclaims it and
    //StrValCopy then reads "" (dead handles decode empty).
    uint8_t churnArgs[1][8] = {};
    const int32_t churn = e.MintHostString("churn");
    std::memcpy(churnArgs[0], &churn, sizeof(churn));
    uint8_t result[8] = {};
    for (int i = 0; i < 16; ++i)
        e.CallByName("values_demo.greet", &churnArgs[0][0], 1, result);
    CHECK(e.StrValCopy(handle) == "rooted-survivor",
        "host-rooted string survives GC collections");
    e.PopHostRoot(RTK_String, handle);
    PASS();
}

int main() {
    Runtime::StaticInit();
    TestCallByNameScalars();
    TestCallByNameFillsDefaults();
    TestCallByNameStringRoundTrip();
    TestCallByNameSurfacesNLangThrow();
    TestCallByNameTwiceHeapSurvives();
    TestHostRootProtectsAcrossGc();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
