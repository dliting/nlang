//Interpreter-level embedding tests: lifecycle, load/run, call, host
//functions, proxies, I/O — all through the public header against real
//ncc-built fixtures (no mocks).
#include "nlang/embed/NLang.h"
#include <cctype>
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
#ifndef EMBED_HOSTFNS_NCU
#error "EMBED_HOSTFNS_NCU must be defined by the build"
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

static void TestHostFunctionDispatch() {
    TEST(TestHostFunctionDispatch);
    Interpreter itp;
    itp.registerHostFunction("hostfns", "now",
        [](const std::vector<Value>& args) {
            return Value(int32_t(41));
        });
    itp.load(EMBED_HOSTFNS_NCU);
    Value r = itp.call("hostfns.useNow", {});
    CHECK(r.asInt() == 42, "useNow() == now()+1 == 42");
    PASS();
}

static void TestHostFunctionStringArgs() {
    TEST(TestHostFunctionStringArgs);
    Interpreter itp;
    itp.registerHostFunction("hostfns", "shouted",
        [](const std::vector<Value>& args) {
            std::string s = args.at(0).asString();
            for (auto& c : s)
                c = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(c)));
            return Value(s + "!");
        });
    itp.load(EMBED_HOSTFNS_NCU);
    Value r = itp.call("hostfns.useShouted", {});
    CHECK(r.asString() == "ABC!", "string args/return both ways");
    PASS();
}

static void TestHostExceptionBecomesScriptException() {
    TEST(TestHostExceptionBecomesScriptException);
    Interpreter itp;
    itp.registerHostFunction("hostfns", "willFail",
        [](const std::vector<Value>& args) -> Value {
            throw std::runtime_error("host-side failure");
        });
    itp.load(EMBED_HOSTFNS_NCU);
    //catchHostFailure catches and returns message length (>0)
    Value r = itp.call("hostfns.catchHostFailure", {});
    CHECK(r.asInt() > 0,
        "host C++ exception surfaced as catchable Exception");
    PASS();
}

static void TestHostFunctionArgMarshalByDeclaration() {
    TEST(TestHostFunctionArgMarshalByDeclaration);
    Interpreter itp;
    int64_t observed = 0;
    itp.registerHostFunction("hostfns", "shouted",
        [&](const std::vector<Value>& args) -> Value {
            observed = args.at(0).kind() == Value::Kind::String ? 1 : 0;
            return Value(std::string("x"));
        });
    itp.load(EMBED_HOSTFNS_NCU);
    (void)itp.call("hostfns.useShouted", {});
    CHECK(observed == 1, "declared string formal arrives as Kind::String");
    PASS();
}

static void TestReentryThrowsBadValue() {
    TEST(TestReentryThrowsBadValue);
    Interpreter itp;
    //reenter() calls back into the SAME interpreter from inside a
    //HostFn; the running guard must fire as BadValue — a host usage
    //error thrown before any function resolution, passing raw through
    //the translation chain (BadValue never becomes a script exception)
    Interpreter* pItp = &itp;
    itp.registerHostFunction("hostfns", "reenter",
        [pItp](const std::vector<Value>& args) {
            return pItp->call("hostfns.useNow", {});
        });
    itp.load(EMBED_HOSTFNS_NCU);
    CHECK_THROWS(BadValue, (void)itp.call("hostfns.useReenter", {}),
        "re-entering call() from inside a HostFn must throw BadValue");
    PASS();
}

static void TestScriptExceptionRethrownFromHostFn() {
    TEST(TestScriptExceptionRethrownFromHostFn);
    Interpreter itp;
    itp.load(EMBED_HOSTFNS_NCU);
    //②翻译链的可达场景＝跨调用重抛：宿主先在一次独立 call() 里捕获
    //脚本异常（Exception 携堆实例，heapIdx>0），再从 HostFn 抛出——
    //钩子按同实例重抛（NLangThrow），脚本 catch 看到原始异常对象。
    //（HostFn 内嵌套 call() 不在此列：再入守卫先抛 BadValue 穿透。）
    Exception saved("", "", "");
    try {
        (void)itp.call("hostfns.fail", {});
    } catch (const Exception& e) {
        saved = e;
    }
    CHECK(saved.message().find("hf-boom") != std::string::npos,
        "the earlier call surfaced the script exception");
    itp.registerHostFunction("hostfns", "callBoom",
        [saved](const std::vector<Value>& args) -> Value {
            throw saved;
        });
    Value r = itp.call("hostfns.catchRethrown", {});
    CHECK(r.asInt() == 7,
        "rethrown script exception keeps its identity (message 7 chars)");
    PASS();
}

static void TestHostMadeExceptionSurfaces() {
    TEST(TestHostMadeExceptionSurfaces);
    Interpreter itp;
    //③翻译链：宿主手造 nlang::Exception（无堆实例）→铸成脚本 Exception
    itp.registerHostFunction("hostfns", "madeUp",
        [](const std::vector<Value>& args) -> Value {
            throw Exception("made", "", "Exception");
        });
    itp.load(EMBED_HOSTFNS_NCU);
    Value r = itp.call("hostfns.catchMade", {});
    CHECK(r.asInt() == 4,
        "host-made Exception surfaces as catchable script Exception");
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
    TestHostFunctionDispatch();
    TestHostFunctionStringArgs();
    TestHostExceptionBecomesScriptException();
    TestHostFunctionArgMarshalByDeclaration();
    TestReentryThrowsBadValue();
    TestScriptExceptionRethrownFromHostFn();
    TestHostMadeExceptionSurfaces();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
