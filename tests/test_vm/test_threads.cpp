/*---
    test_threads.cpp - 并发 K1（帧栈机 + 虚拟线程/协程）单元测试。

    Harness 形态与 test_primops Layer 3 相同：真实 .n 源码经
    ModuleBuilder 编译、NcuLoader/NcuLinker 装链接、VmExecutor 真实执行，
    断言 main() 返回值。本文件随切片增长：
      Slice 0  冒烟（本文件）；
      后续切片 加入调度/挂起/根集等行为测试与测试专用旋钮。
    确定性规约（计划钉死）：断言只用顺序/终值/happens-before，不做墙钟
    断言；交替顺序仅经测试专用配额旋钮强制。
---*/
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

#ifdef _WIN32
#include <windows.h>
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
        / "nlang_test_threads";
    std::filesystem::create_directories(dir);
    return dir;
}

//Compile + execute; returns main()'s value, or -1 when the build failed
//(same harness shape as test_primops — real sources, real execution).
static int runSource(const std::string& tag, const std::string& source)
{
    const auto dir = scratchDir();
    const auto nPath = dir / (tag + ".n");
    const auto modPath = dir / (tag + ".ncu");
    std::filesystem::remove(modPath);

    {
        std::ofstream out(nPath, std::ios::binary);
        out << source;
    }

    BuildParams params;
    params.m_SourceFiles.push_back(nPath.string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;

    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    bool built = false;
    try {
        built = builder.BuildArtifacts();
    } catch (const std::exception& e) {
        std::cerr << "build failed: " << e.what() << "\n";
        return -1;
    }
    if (!built || !std::filesystem::exists(modPath)) {
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            std::cerr << (*it)->Message() << "\n";
        return -1;
    }
    try {
        NcuLoader::Options loaderOpts;
        loaderOpts.searchDirs = {dir.string(), STDLIB_DIR};
        NcuLoader::Result loaded =
            NcuLoader::LoadClosure(modPath.string(), loaderOpts);
        CompiledModule mod = NcuLinker::Link(std::move(loaded.units),
                                             loaded.entryKey);
        VmExecutor exec;
        return exec.Execute(mod);
    } catch (const std::exception& e) {
        std::cerr << "load/link failed: " << e.what() << "\n";
        return -1;
    }
}

void test_harness_smoke()
{
    TEST(harness_smoke);
    const int rc = runSource("smoke",
        "int main() {\n"
        "    return 7;\n"
        "}\n");
    CHECK(rc == 7, "compile + execute round trip returns main value");
    PASS();
}

void test_harness_smoke_exception_path()
{
    TEST(harness_smoke_exception_path);
    const int rc = runSource("smoke_exc",
        "int main() {\n"
        "    try {\n"
        "        throw Exception(\"boom\");\n"
        "    } catch (Exception e) {\n"
        "        if (e.message == \"boom\") return 0;\n"
        "        return 2;\n"
        "    }\n"
        "}\n");
    CHECK(rc == 0, "NLangThrow catch path works through the harness");
    PASS();
}

int main()
{
#ifdef _WIN32
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS
                             | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    //Hidden host contract (same as ncc/nvm main): the compiler needs the
    //IdString tables and the cast table (module boundary: runtime stays
    //compiler-free).
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    std::cerr << "=== NLang Threads Unit Tests ===\n\n";

    try {
        test_harness_smoke();
        test_harness_smoke_exception_path();
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
