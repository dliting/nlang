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
            "    int[] a = int[1];\n"  // 2
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
            "    throw Exception(\"boom\");\n"
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
            "    throw Exception(\"\");\n"
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
            "        throw Exception(\"rethrown\");\n"
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

//非法 as 强转抛 InvalidCastException——NLang catch 可捕获、message 可读。
static void TestInvalidCastIsCatchable() {
    TEST(TestInvalidCastIsCatchable);
    {
        BuildOutcome bo = buildSource("bt_cast_exc",
            "class Point {\n"
            "    public int x;\n"
            "}\n"
            "class Other {\n"
            "    public int z;\n"
            "}\n"
            "int main() {\n"
            "    Point p = Point();\n"
            "    Object o = p;\n"
            "    int code = 0;\n"
            "    try {\n"
            "        Other t = o as Other;\n"
            "        code = 1;\n"
            "    } catch (Exception e) {\n"
            "        if (e.message.contains(\"invalid cast\")) {\n"
            "            code = 42;\n"
            "        } else {\n"
            "            code = 2;\n"
            "        }\n"
            "    }\n"
            "    return code;\n"
            "}\n");
        REQUIRE(bo.ok);
        CompiledModule mod = loadBuilt("bt_cast_exc");
        VmExecutor exec;
        CHECK(exec.Execute(mod) == 42,
              "catch handles the failed cast and reads its message");
    }
    PASS();
}

//用户可直接构造 InvalidCastException 并 throw。
static void TestUserConstructedInvalidCast() {
    TEST(TestUserConstructedInvalidCast);
    {
        BuildOutcome bo = buildSource("bt_cast_user",
            "int main() {\n"
            "    InvalidCastException e = InvalidCastException(\"custom\");\n"
            "    throw e;\n"
            "}\n");
        REQUIRE(bo.ok);
        CompiledModule mod = loadBuilt("bt_cast_user");
        VmExecutor exec;
        std::string what;
        try {
            exec.Execute(mod);
        } catch (const std::exception& e) {
            what = e.what();
        }
        CHECK(what == "custom", "user-constructed InvalidCastException throws");
    }
    PASS();
}

//--no-debug-info：锚点计数 0；默认构建与现状一致（非空）。
static void TestStripDebugInfoRemovesAnchors() {
    TEST(TestStripDebugInfoRemovesAnchors);
    {
        const char* src =
            "int main() {\n"
            "    int[] a = int[1];\n"
            "    return a[5];\n"
            "}\n";
        const auto dir = scratchDir();
        const auto nPath = dir / "bt_strip.n";
        const auto modPath = dir / "bt_strip.ncu";
        std::filesystem::remove(modPath);
        {
            std::ofstream out(nPath, std::ios::binary);
            out << src;
        }
        //Mirror buildSource (test_debugger_common.cpp) with the flag
        //set — same param shape, logger, and BuildArtifacts path.
        BuildParams params;
        params.m_SourceFiles.push_back(nPath.string());
        params.m_sOutputModule = "bt_strip";
        params.m_sOutputDir = dir.string();
        params.m_sTempDir = dir.string();
        params.m_sStdLibDir = STDLIB_DIR;
        params.m_bStripDebugInfo = true;   //the flag under test
        ListCompileLogger logger;
        ModuleBuilder builder(params, logger);
        REQUIRE(builder.BuildArtifacts()
            && std::filesystem::exists(modPath));
        CompiledModule mod = ModuleLoader::Load(modPath.string());
        bool anyAnchor = false;
        for (const auto& fn : mod.functions)
            if (!BuildLinePcMap(fn).empty()) { anyAnchor = true; break; }
        CHECK(!anyAnchor, "stripped artifact carries no statement anchors");

        BuildOutcome plain = buildSource("bt_plain", src);
        REQUIRE(plain.ok);
        CompiledModule modPlain = loadBuilt("bt_plain");
        bool plainAnchor = false;
        for (const auto& fn : modPlain.functions)
            if (!BuildLinePcMap(fn).empty()) { plainAnchor = true; break; }
        CHECK(plainAnchor, "default build keeps its statement anchors");
    }
    PASS();
}

// --- 帧机等价钉死（K1 Slice 2）：回溯仅有的两种可观测形态各一条字节级断言。
// 等价性论证：m_unwindFrames 在 catch 命中时清空 → 文本回溯只在未捕获路径
// 可观测；字段回溯（RaiseNlangException 在抛出点从调用栈构造）是第二种。
// 两者逐字节钉死后，帧机与递归引擎的可观测行为收口于此。

//形态一：未捕获用户 throw 的整段文本回溯 —— 全等断言（非子串）钉死
//「最内帧在前、每帧一行、两空格 at 前缀、无多余/重复帧」。
static void TestUncaughtBacktraceTextByteExact() {
    TEST(TestUncaughtBacktraceTextByteExact);
    {
        BuildOutcome bo = buildSource("bt_pin_uncaught",
            "int inner() {\n"                    // 1
            "    throw Exception(\"deep\");\n"   // 2
            "}\n"                                // 3
            "int mid() {\n"                      // 4
            "    return inner();\n"              // 5
            "}\n"                                // 6
            "int main() {\n"                     // 7
            "    return mid();\n"                // 8
            "}\n");                              // 9
        REQUIRE(bo.ok);
        CompiledModule mod = loadBuilt("bt_pin_uncaught");
        VmExecutor exec;
        std::string what;
        try {
            exec.Execute(mod);
        } catch (const std::exception& e) {
            what = e.what();
        }
        REQUIRE(what == "deep");
        const std::string file = (scratchDir() / "bt_pin_uncaught.n").string();
        const std::string want =
            "  at bt_pin_uncaught.inner (" + file + ":2)\n"
            "  at bt_pin_uncaught.mid ("   + file + ":5)\n"
            "  at bt_pin_uncaught.main ("  + file + ":8)\n";
        CHECK(exec.Backtrace() == want,
              "uncaught backtrace text is byte-exact: innermost-first, "
              "one line per frame, no extra frames");
    }
    PASS();
}

//形态二：VM 抛出异常的 e.backtrace 字段 —— 源码内拼接本机真实源路径，
//在 NLang 侧做全等比较，钉死「帧数=活跃调用栈、逐字节条目、顺序」。
//（用户 throw 的字段恒空是既有设计，exception_backtrace_access.n 已覆盖。）
static void TestExceptionBacktraceFieldByteExact() {
    TEST(TestExceptionBacktraceFieldByteExact);
    {
        //NLang 字符串字面量里反斜杠要成对转义。
        const std::string file = (scratchDir() / "bt_pin_field.n").string();
        std::string escaped;
        for (char c : file) {
            if (c == '\\') escaped += "\\\\";
            else escaped += c;
        }
        const std::string src =
            "int helper() {\n"                                  // 1
            "    int x = 0;\n"                                  // 2
            "    return 1 / x;\n"                               // 3
            "}\n"                                               // 4
            "int main() {\n"                                    // 5
            "    try {\n"                                       // 6
            "        return helper();\n"                        // 7
            "    } catch (DivByZeroException e) {\n"            // 8
            "        if (e.backtrace.length() != 2) return 100;\n" // 9
            "        string f0 = e.backtrace.get(0);\n"         // 10
            "        string f1 = e.backtrace.get(1);\n"         // 11
            "        if (f0 == \"bt_pin_field.helper (\" + \"" + escaped + "\" + \":3)\") {\n" // 12
            "            if (f1 == \"bt_pin_field.main (\" + \"" + escaped + "\" + \":7)\") {\n" // 13
            "                return 7;\n"                       // 14
            "            }\n"                                   // 15
            "            return 102;\n"                         // 16
            "        }\n"                                       // 17
            "        return 101;\n"                             // 18
            "    }\n"                                           // 19
            "}\n";                                              // 20
        BuildOutcome bo = buildSource("bt_pin_field", src);
        REQUIRE(bo.ok);
        CompiledModule mod = loadBuilt("bt_pin_field");
        VmExecutor exec;
        CHECK(exec.Execute(mod) == 7,
              "backtrace field is byte-exact: exactly the two active "
              "frames, innermost-first, full path + line per entry");
    }
    PASS();
}

void run_backtrace_tests() {
    TestFormatBacktraceFrameShapes();
    TestBacktraceCarriesRealSourceFile();
    TestThrowMessagePassthrough();
    TestThrowEmptyMessageFallsBackToClassName();
    TestRethrowKeepsMessage();
    TestInvalidCastIsCatchable();
    TestUserConstructedInvalidCast();
    TestStripDebugInfoRemovesAnchors();
    TestUncaughtBacktraceTextByteExact();
    TestExceptionBacktraceFieldByteExact();
}
