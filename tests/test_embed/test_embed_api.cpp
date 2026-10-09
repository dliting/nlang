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

static void TestStringResultIsDecodeCopy() {
    TEST(TestStringResultIsDecodeCopy);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    Value g = itp.call("values_demo.greet", {Value("held")});
    //The returned Value owns its bytes (StrValCopy at decode): holding it
    //across further VM work cannot pull them out from under the host.
    for (int i = 0; i < 32; ++i) {
        (void)itp.call("values_demo.add",
            {Value(int32_t(1)), Value(int32_t(2))});
    }
    CHECK(g.kind() == Value::Kind::String && g.asString() == "hello, held",
        "string result reads unchanged after later calls");
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
    //Func 形参的 kind 裁决链回归钉（描述符 NonSerialized 回退帧布局后
    //打开的路径）：错配的参考值与 builder 都必须在编组边界被拒，而非
    //把错型值塞进帧里、留给下游 CallDelegate 的槽守卫报误导性 NPE。
    Value pair = itp.call("values_demo.makePair",
        {Value(int32_t(1)), Value(int32_t(2))});
    CHECK_THROWS(BadValue,
        (void)itp.call("values_demo.applyFunc",
            {pair, Value(int32_t(21))}),
        "struct value into a Func formal → BadValue (reference kind mismatch)");
    CHECK_THROWS(BadValue,
        (void)itp.call("values_demo.applyFunc",
            {itp.newList(), Value(int32_t(21))}),
        "list builder into a Func formal → BadValue (non-container formal)");
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

static void TestHostFunctionFuncParamKind() {
    TEST(TestHostFunctionFuncParamKind);
    Interpreter itp;
    itp.registerHostFunction("hostfns", "takeFunc",
        [](const std::vector<Value>& args) -> Value {
            //The Func argument must arrive as Kind::Func (reference arm),
            //not Kind::Int (scalar arm from the int32 placeholder when the
            //v1.12 descriptor degrades the Func signature).
            return Value(int32_t(
                args.at(0).kind() == Value::Kind::Func ? 1 : 0));
        });
    itp.load(EMBED_HOSTFNS_NCU);
    Value r = itp.call("hostfns.useTakeFunc", {});
    CHECK(r.asInt() == 1, "Func arg to native arrives as Kind::Func");
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

#ifndef EMBED_CONTAINERS_NCU
#error "EMBED_CONTAINERS_NCU must be defined by the build"
#endif
#ifndef EMBED_IO_NCU
#error "EMBED_IO_NCU must be defined by the build"
#endif
#ifndef EMBED_MATH_NCU
#error "EMBED_MATH_NCU must be defined by the build"
#endif
#ifndef NLANG_TEST_STDLIB_DIR
#error "NLANG_TEST_STDLIB_DIR must be defined by the build"
#endif

static void TestListProxyRoundTrip() {
    TEST(TestListProxyRoundTrip);
    Interpreter itp;
    itp.load(EMBED_CONTAINERS_NCU);
    Value v = itp.call("containers_demo.makeList", {});
    ListProxy list = v.asList();
    CHECK(list.size() == 3, "list size");
    CHECK(list.get(0).asInt() == 10, "list[0]");
    CHECK(list.get(2).asInt() == 30, "list[2]");
    list.set(1, Value(int32_t(99)));
    list.add(Value(int32_t(40)));
    Value sum = itp.call("containers_demo.sumList", {v});
    CHECK(sum.asInt() == 10 + 99 + 30 + 40, "host edits visible to NLang");
    PASS();
}

static void TestDictProxy() {
    TEST(TestDictProxy);
    Interpreter itp;
    itp.load(EMBED_CONTAINERS_NCU);
    Value v = itp.call("containers_demo.makeDict", {});
    DictProxy d = v.asDict();
    CHECK(d.containsKey("a") && d.get("a").asInt() == 1, "dict get");
    d.set("hostkey", Value(int32_t(77)));
    CHECK(d.containsKey("hostkey"), "dict set");
    Value r = itp.call("containers_demo.readAfterHostEdit", {v});
    CHECK(r.asInt() == 77, "dict write visible to NLang");
    std::vector<Value> keys = d.keys();
    CHECK(keys.size() == 3, "keys count");
    PASS();
}

static void TestArrayAndObjectProxies() {
    TEST(TestArrayAndObjectProxies);
    Interpreter itp;
    itp.load(EMBED_CONTAINERS_NCU);
    Value a = itp.call("containers_demo.makeArray", {});
    ArrayProxy arr = a.asArray();
    CHECK(arr.size() == 3, "array size");
    CHECK(arr.get(1).asInt() == 8, "array[1]");
    arr.set(1, Value(int32_t(88)));
    CHECK(arr.get(1).asInt() == 88, "array set/get");
    Value p = itp.call("containers_demo.makePoint", {});
    ObjectProxy obj = p.asObject();
    CHECK(obj.getField("x").asInt() == 3, "object field x");
    obj.setField("y", Value(int32_t(44)));
    CHECK(obj.getField("y").asInt() == 44, "object field set/get");
    PASS();
}

static void TestArrayProxyBoundsAndKindGuards() {
    TEST(TestArrayProxyBoundsAndKindGuards);
    Interpreter itp;
    itp.load(EMBED_CONTAINERS_NCU);
    Value a = itp.call("containers_demo.makeArray", {});
    ArrayProxy arr = a.asArray();
    CHECK_THROWS(BadValue, arr.get(3), "out-of-bounds get → BadValue");
    CHECK_THROWS(BadValue, arr.get(-1), "negative index → BadValue");
    CHECK_THROWS(BadValue, arr.set(0, Value(std::string("x"))),
        "kind-incompatible set → BadValue");
    CHECK_THROWS(BadValue, Value(int32_t(1)).asArray(),
        "asArray on Int → BadValue");
    PASS();
}

static void TestBuildersMaterializePerCrossing() {
    TEST(TestBuildersMaterializePerCrossing);
    Interpreter itp;
    itp.load(EMBED_CONTAINERS_NCU);
    Value b = itp.newList();
    ListProxy bl = b.asList();
    bl.add(Value(int32_t(1)));
    bl.add(Value(int32_t(2)));
    Value s1 = itp.call("containers_demo.sumList", {b});
    CHECK(s1.asInt() == 3, "builder crossing materializes");
    Value s2 = itp.call("containers_demo.sumList", {b});
    CHECK(s2.asInt() == 3, "second crossing works (fresh object)");
    //builder 与物化物独立：再 add 不影响已递交的
    bl.add(Value(int32_t(100)));
    Value s3 = itp.call("containers_demo.sumList", {b});
    CHECK(s3.asInt() == 103, "builder edits re-materialize on next crossing");
    //Dict builder 对称覆盖（newDict 与 newList 同一 builder 语义）
    Value db = itp.newDict();
    DictProxy bd = db.asDict();
    bd.set("hostkey", Value(int32_t(77)));
    Value dr = itp.call("containers_demo.readAfterHostEdit", {db});
    CHECK(dr.asInt() == 77, "dict builder crossing materializes");
    PASS();
}

//builder 字典键的标量/字符串约束（spec §6）：参考键无恒等外表示，堆内
//比较属 vm 桥 HostKeysEqual 的职责。set 入口前置拒绝——首个参考键即抛，
//而非旧行为的「首个可入、同种参考键再入才抛」的部分支持不一致。
static void TestBuilderDictRejectsReferenceKeys() {
    TEST(TestBuilderDictRejectsReferenceKeys);
    Interpreter itp;
    itp.load(EMBED_CONTAINERS_NCU);
    Value db = itp.newDict();
    DictProxy bd = db.asDict();
    Value obj = itp.call("containers_demo.makePoint", {});
    CHECK_THROWS(BadValue, bd.set(obj, Value(int32_t(1))),
        "builder dict reference key rejected at set entry");
    //标量键不受影响（前置拒绝仅约束参考键）
    bd.set("hostkey", Value(int32_t(77)));
    CHECK(bd.containsKey("hostkey"),
        "scalar key still accepted after reference-key rejection");
    PASS();
}

static void TestHeldProxySurvivesGc() {
    TEST(TestHeldProxySurvivesGc);
    Interpreter itp;
    itp.load(EMBED_CONTAINERS_NCU);
    Value v = itp.call("containers_demo.makeList", {});
    ListProxy list = v.asList();
    //Loop back-edges are GC safepoints and both default thresholds are
    //1024, so 20000 concat rounds ≈ 19x the string threshold → multiple
    //real collections at DEFAULT settings. Without extension point 3 the
    //list is swept by SweepPhase and the reads below hit a dead slot.
    (void)itp.call("containers_demo.churnStrings",
        {Value(int32_t(20000))});
    CHECK(list.size() == 3 && list.get(0).asInt() == 10,
        "host-held proxy survives real collections (GC root)");
    PASS();
}

static void TestProxyRemoveAndClear() {
    TEST(TestProxyRemoveAndClear);
    Interpreter itp;
    itp.load(EMBED_CONTAINERS_NCU);
    Value lv = itp.call("containers_demo.makeList", {});
    ListProxy list = lv.asList();
    list.removeAt(1);
    CHECK(list.size() == 2 && list.get(1).asInt() == 30,
        "list removeAt shifts elements");
    list.clear();
    CHECK(list.size() == 0, "list clear empties");
    Value dv = itp.call("containers_demo.makeDict", {});
    DictProxy d = dv.asDict();
    d.remove("a");
    CHECK(!d.containsKey("a") && d.get("b").asInt() == 2,
        "dict remove drops only the matched key");
    d.clear();
    CHECK(d.keys().empty(), "dict clear empties");
    PASS();
}

static void TestStructAndFuncKindsRoundTrip() {
    TEST(TestStructAndFuncKindsRoundTrip);
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    Value pair = itp.call("values_demo.makePair",
        {Value(int32_t(3)), Value(int32_t(4))});
    CHECK(pair.kind() == Value::Kind::Struct,
        "struct result decodes to Kind::Struct");
    Value total = itp.call("values_demo.sumPair", {pair});
    CHECK(total.asInt() == 7,
        "struct value passes back through and fields survive");
    Value fn = itp.call("values_demo.makeFunc", {});
    CHECK(fn.kind() == Value::Kind::Func,
        "function result decodes to Kind::Func");
    Value doubled = itp.call("values_demo.applyFunc",
        {fn, Value(int32_t(21))});
    CHECK(doubled.asInt() == 42,
        "function value passes back through and calls");
    PASS();
}

static void TestOutputHandlerSeparatesStreams() {
    TEST(TestOutputHandlerSeparatesStreams);
    Interpreter itp;
    std::string outText, errText;
    itp.setOutputHandler(
        [&](const char* t) { outText += t; },
        [&](const char* t) { errText += t; });
    itp.addImportDir(NLANG_TEST_STDLIB_DIR);
    itp.load(EMBED_IO_NCU);
    (void)itp.call("io_demo.emitBoth", {});
    CHECK(outText.find("to-out") != std::string::npos, "out routed");
    CHECK(errText.find("to-err") != std::string::npos, "err routed");
    CHECK(outText.find("to-err") == std::string::npos, "no leak into out");
    CHECK(errText.find("to-out") == std::string::npos, "no leak into err");
    //Both empty callbacks uninstall the handlers: a second emit must not
    //reach the captured buffers (the forwarder is detached from the VM).
    const size_t outLen = outText.size(), errLen = errText.size();
    itp.setOutputHandler(WriteFn(), WriteFn());
    (void)itp.call("io_demo.emitBoth", {});
    CHECK(outText.size() == outLen && errText.size() == errLen,
        "both-empty setOutputHandler uninstalls the forwarder");
    PASS();
}

static void TestInputDefaultsToNoChannel() {
    TEST(TestInputDefaultsToNoChannel);
    Interpreter itp;
    itp.setOutputHandler([](const char*) {}, [](const char*) {});
    itp.addImportDir(NLANG_TEST_STDLIB_DIR);
    itp.load(EMBED_IO_NCU);
    Value r = itp.call("io_demo.readWhenNoChannel", {});
    CHECK(r.asInt() == 1, "io.readLine raises catchable IOException");
    PASS();
}

//spec §5: a missing native binding (a declared native the loaded module
//does not export) maps to LoadError, not a raw runtime_error. The fixture's
//unit stem "math" makes its native "math.nonexistent"; nlang_math.dll loads
//(it is copied beside the test exe by nlang_stdlib_native_for_tests) but
//does not export that name, so the call reaches the executor's
//"native function not registered" throw — the message the environment-
//failure discriminator must recognize (a regression guard for the
//NLang-VM-prefixed message the prefix test used to miss).
static void TestUnregisteredNativeThrowsLoadError() {
    TEST(TestUnregisteredNativeThrowsLoadError);
    Interpreter itp;
    itp.addImportDir(NLANG_TEST_STDLIB_DIR);
    itp.load(EMBED_MATH_NCU);
    CHECK_THROWS(LoadError, itp.call("math.callNonexistent", {}),
        "missing native binding must throw LoadError");
    PASS();
}

//shutdown() 落位（公共面覆盖对账补的缺口）：必须放 main() 最后——
//它拆除运行时全局状态，之后的用例走再入循环钉「关停不毒化后续
//初始化」的生命周期契约。
static void TestShutdownReinitializes() {
    TEST(TestShutdownReinitializes);
    shutdown();
    initialize();
    Interpreter itp;
    itp.load(EMBED_VALUES_NCU);
    Value r = itp.call("values_demo.add",
        {Value(int32_t(20)), Value(int32_t(22))});
    CHECK(r.asInt() == 42, "interpreter works after shutdown + initialize");
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
    TestStringResultIsDecodeCopy();
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
    TestHostFunctionFuncParamKind();
    TestReentryThrowsBadValue();
    TestScriptExceptionRethrownFromHostFn();
    TestHostMadeExceptionSurfaces();
    TestListProxyRoundTrip();
    TestDictProxy();
    TestArrayAndObjectProxies();
    TestArrayProxyBoundsAndKindGuards();
    TestBuildersMaterializePerCrossing();
    TestBuilderDictRejectsReferenceKeys();
    TestHeldProxySurvivesGc();
    TestProxyRemoveAndClear();
    TestStructAndFuncKindsRoundTrip();
    TestOutputHandlerSeparatesStreams();
    TestInputDefaultsToNoChannel();
    TestUnregisteredNativeThrowsLoadError();
    TestShutdownReinitializes();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
