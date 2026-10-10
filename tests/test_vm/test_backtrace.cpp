// --- runtime error reporting tests: backtrace file paths, throw
// messages, the InvalidCastException family, --no-debug-info anchors.
// Same in-process harness as the debugger tests (buildSource/loadBuilt).
#include "test_debugger_common.h"

using namespace nlang;

//FormatBacktraceFrame 的纯函数形状钉（单一定义，两位点共享）。
static void TestFormatBacktraceFrameShapes() {
    TEST(TestFormatBacktraceFrameShapes);
    {
        CHECK(FormatBacktraceFrame("f", "C:/x/a.n", 3, "mod")
                  == "f (C:/x/a.n:3)",
              "real file + line");
        CHECK(FormatBacktraceFrame("f", "C:/x/a.n", 0, "mod")
                  == "f (C:/x/a.n:?)",
              "zero line prints ? (existing convention)");
        CHECK(FormatBacktraceFrame("f", "", 7, "mod") == "f (mod.n:7)",
              "empty file falls back to the legacy module spelling");
    }
    PASS();
}

//跨帧回溯带本翻译单元的真实编译路径，非编造的 <module>.n。
static void TestBacktraceCarriesRealSourceFile() {
    TEST(TestBacktraceCarriesRealSourceFile);
    {
        BuildOutcome bo = buildSource("bt_real_src",
            "int helper() {\n"       // 1
            "    int[] a = new int[1];\n"  // 2
            "    return a[5];\n"     // 3
            "}\n"                    // 4
            "int main() {\n"         // 5
            "    return helper();\n" // 6
            "}\n");                  // 7
        REQUIRE(bo.ok);
        CompiledModule mod = loadBuilt("bt_real_src");
        VmExecutor exec;
        std::string what;
        try {
            exec.Execute(mod);
        } catch (const std::exception& e) {
            what = e.what();
        }
        REQUIRE(!what.empty());   //the index-out-of-bounds escaped uncaught
        const std::string bt = exec.Backtrace();
        //module name == tag here, so "bt_real_src.n:3" alone cannot
        //discriminate; the scratch-dir prefix proves the path came from
        //CompiledFunction::sourceFile (absolute), not the module stem.
        CHECK(bt.find((scratchDir() / "bt_real_src.n").string())
                  != std::string::npos,
              "backtrace file is the absolute compiled path");
        CHECK(bt.find("bt_real_src.n:3") != std::string::npos,
              "helper frame carries the real file + line");
        CHECK(bt.find("bt_real_src.n:6") != std::string::npos,
              "main frame carries the real file + line");
    }
    PASS();
}

//未捕获用户 throw 的顶层消息＝异常自身 message（旧实现恒 "user throw"）。
static void TestThrowMessagePassthrough() {
    TEST(TestThrowMessagePassthrough);
    {
        BuildOutcome bo = buildSource("bt_throw_msg",
            "int main() {\n"
            "    throw new Exception(\"boom\");\n"
            "}\n");
        REQUIRE(bo.ok);
        CompiledModule mod = loadBuilt("bt_throw_msg");
        VmExecutor exec;
        std::string what;
        try {
            exec.Execute(mod);
        } catch (const std::exception& e) {
            what = e.what();
        }
        CHECK(what == "boom", "top-level message is the exception's own");
    }
    PASS();
}

//空消息兜底＝异常类叶名（类名恒可得），不再输出字面 "user throw"。
static void TestThrowEmptyMessageFallsBackToClassName() {
    TEST(TestThrowEmptyMessageFallsBackToClassName);
    {
        BuildOutcome bo = buildSource("bt_throw_empty",
            "int main() {\n"
            "    throw new Exception(\"\");\n"
            "}\n");
        REQUIRE(bo.ok);
        CompiledModule mod = loadBuilt("bt_throw_empty");
        VmExecutor exec;
        std::string what;
        try {
            exec.Execute(mod);
        } catch (const std::exception& e) {
            what = e.what();
        }
        CHECK(what == "Exception", "empty message falls back to the leaf class name");
    }
    PASS();
}

//catch 后裸 rethrow 保留原消息。
static void TestRethrowKeepsMessage() {
    TEST(TestRethrowKeepsMessage);
    {
        BuildOutcome bo = buildSource("bt_rethrow_msg",
            "int main() {\n"
            "    try {\n"
            "        throw new Exception(\"rethrown\");\n"
            "    } catch (Exception e) {\n"
            "        throw;\n"
            "    }\n"
            "}\n");
        REQUIRE(bo.ok);
        CompiledModule mod = loadBuilt("bt_rethrow_msg");
        VmExecutor exec;
        std::string what;
        try {
            exec.Execute(mod);
        } catch (const std::exception& e) {
            what = e.what();
        }
        CHECK(what == "rethrown", "rethrow keeps the original message");
    }
    PASS();
}

void run_backtrace_tests() {
    TestFormatBacktraceFrameShapes();
    TestBacktraceCarriesRealSourceFile();
    TestThrowMessagePassthrough();
    TestThrowEmptyMessageFallsBackToClassName();
    TestRethrowKeepsMessage();
}
