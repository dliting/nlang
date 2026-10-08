//Interpreter-level embedding tests: lifecycle, load/run, call, host
//functions, proxies, I/O — all through the public header against real
//ncc-built fixtures (no mocks).
#include "nlang/embed/NLang.h"
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
#define CHECK_THROWS(excType, stmt, msg) \
    do { bool threw = false; \
         try { stmt; } catch (const excType&) { threw = true; } \
         if (!threw) { FAIL(msg); return; } } while(0)

#ifndef EMBED_VALUES_NCU
#error "EMBED_VALUES_NCU must be defined by the build"
#endif

static void TestRunReturnsMainExitCode() {
    TEST(TestRunReturnsMainExitCode);
    initialize();
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    CHECK(itp.run() == 7, "run() returns main()'s int result (7)");
    PASS();
}

static void TestRunTwiceThrowsBadValue() {
    TEST(TestRunTwiceThrowsBadValue);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    itp.run();
    CHECK_THROWS(BadValue, itp.run(), "second run() must throw BadValue");
    PASS();
}

static void TestLoadTwiceThrowsBadValue() {
    TEST(TestLoadTwiceThrowsBadValue);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    CHECK_THROWS(BadValue, itp.load(EMBED_VALUES_NCU),
        "second load() must throw BadValue");
    PASS();
}

static void TestMissingArtifactThrowsLoadError() {
    TEST(TestMissingArtifactThrowsLoadError);
    Interpreter itp;
    CHECK_THROWS(LoadError, itp.load("Z:/no/such/artifact.ncu"),
        "missing path must throw LoadError");
    PASS();
}

static void TestAddImportDirAfterLoadThrows() {
    TEST(TestAddImportDirAfterLoadThrows);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    CHECK_THROWS(BadValue, itp.addImportDir("."),
        "addImportDir after load must throw BadValue");
    PASS();
}

static void TestUncaughtScriptThrowSurfacesException() {
    TEST(TestUncaughtScriptThrowSurfacesException);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    //values_demo.boom throws new Exception("embedded-boom"); call it
    //uncaught → nlang::Exception with message/backtrace/class
    bool threw = false;
    try {
        (void)itp.call("values_demo.boom", {});
    } catch (const Exception& e) {
        threw = true;
        CHECK(e.message().find("embedded-boom") != std::string::npos,
            "message carried");
        CHECK(e.exceptionClass() == "Exception", "exception class");
        CHECK(!e.backtrace().empty(), "backtrace captured");
    }
    CHECK(threw, "uncaught throw must surface as nlang::Exception");
    PASS();
}

static void TestCallScalarsAndString() {
    TEST(TestCallScalarsAndString);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    Value r = itp.call("values_demo.add", {Value(int32_t(20)),
                                           Value(int32_t(22))});
    CHECK(r.kind() == Value::Kind::Int && r.asInt() == 42, "add(20,22)");
    Value g = itp.call("values_demo.greet", {Value("embed")});
    CHECK(g.kind() == Value::Kind::String && g.asString() == "hello, embed",
        "greet string round-trip");
    PASS();
}

static void TestCallFillsDefaults() {
    TEST(TestCallFillsDefaults);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    Value r = itp.call("values_demo.withDefaults", {Value(int32_t(2))});
    CHECK(r.asInt() == 259, "withDefaults(2) == 259 (5,9 defaults)");
    //与 NLang 侧默认路径对拍（usesDefaults 走编译器 staging）
    Value n = itp.call("values_demo.usesDefaults", {Value(int32_t(2))});
    CHECK(n.asInt() == 259, "NLang-side default path agrees");
    PASS();
}

static void TestCallVoidFunctionReturnsNull() {
    TEST(TestCallVoidFunctionReturnsNull);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    Value r = itp.call("values_demo.logNothing", {});
    CHECK(r.kind() == Value::Kind::Null,
        "void function returns a Null Value");
    PASS();
}

static void TestCallBeforeRunAndAfter() {
    TEST(TestCallBeforeRunAndAfter);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    (void)itp.call("values_demo.add", {Value(int32_t(1)),
                                       Value(int32_t(1))});   //先于 run
    CHECK(itp.run() == 7, "run still works after call");
    (void)itp.call("values_demo.add", {Value(int32_t(2)),
                                       Value(int32_t(2))});   //后于 run
    PASS();
}

static void TestCallRejections() {
    TEST(TestCallRejections);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    CHECK_THROWS(BadValue,
        (void)itp.call("values_demo.nope", {}),
        "unknown function → BadValue");
    CHECK_THROWS(BadValue,
        (void)itp.call("values_demo.add", {Value(int32_t(1))}),
        "too few args (no defaults) → BadValue");
    CHECK_THROWS(BadValue,
        (void)itp.call("values_demo.add",
            {Value(int32_t(1)), Value(std::string("x"))}),
        "kind-incompatible arg → BadValue");
    CHECK_THROWS(BadValue,
        (void)itp.call("values_demo.withOut",
            {Value(int32_t(1)), Value(int32_t(0))}),
        "out-parameter target → BadValue (Value model has no out concept)");
    CHECK_THROWS(BadValue,
        (void)itp.call("add", {Value(int32_t(1)), Value(int32_t(2))}),
        "unqualified name → BadValue");
    PASS();
}

static void TestCallBeforeLoadThrows() {
    TEST(TestCallBeforeLoadThrows);
    Interpreter itp;
    CHECK_THROWS(BadValue, (void)itp.call("values_demo.add", {}),
        "call before load → BadValue");
    PASS();
}

static void TestFailedCallLeavesInterpreterUsable() {
    TEST(TestFailedCallLeavesInterpreterUsable);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    //失败路径（BadValue 家族）不得遗留再入标志——否则同解释器上
    //后续调用全部误报 re-entering（回归守卫：RAII 化前真实发生）
    CHECK_THROWS(BadValue, (void)itp.call("values_demo.nope", {}),
        "unknown function → BadValue");
    Value r = itp.call("values_demo.add",
        {Value(int32_t(1)), Value(int32_t(2))});
    CHECK(r.asInt() == 3, "interpreter still usable after a failed call");
    PASS();
}

int main() {
    TestRunReturnsMainExitCode();
    TestRunTwiceThrowsBadValue();
    TestLoadTwiceThrowsBadValue();
    TestMissingArtifactThrowsLoadError();
    TestAddImportDirAfterLoadThrows();
    TestUncaughtScriptThrowSurfacesException();
    TestCallScalarsAndString();
    TestCallFillsDefaults();
    TestCallVoidFunctionReturnsNull();
    TestCallBeforeRunAndAfter();
    TestCallRejections();
    TestCallBeforeLoadThrows();
    TestFailedCallLeavesInterpreterUsable();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
