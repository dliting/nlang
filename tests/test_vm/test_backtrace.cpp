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

void run_backtrace_tests() {
    TestFormatBacktraceFrameShapes();
    TestBacktraceCarriesRealSourceFile();
}
