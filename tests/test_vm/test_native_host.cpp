// NativeHost callback integration tests. Real .n sources are compiled and
// run; the natives they call are registered in-process and exercise every
// NativeHost callback (string mint/read, List<string> creation, output,
// input, exceptions and the PRNG). No VM internals are mocked.

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "nlang/langservice/SymbolIndex.h"
#include "VmExecutor.h"
#include "IHostIo.h"
#include "ModuleLoader.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

namespace fs = std::filesystem;

fs::path scratchDir() {
    static const auto dir = fs::temp_directory_path() / "nlang_test_native_host";
    fs::create_directories(dir);
    return dir;
}

//--- Test natives (registered in-process) ----------------------------------

void HsMint(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    native::ReturnInt(ret, host->newString(host, "minted"));
}
void HsEcho(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    native::ReturnString(host, ret, native::ArgString(host, args, 0));
}
void HsList(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    const char* items[] = {"x", "y"};
    native::ReturnInt(ret, host->newListString(host, items, 2));
}
void HsRaiseIo(NativeHost* host, uint8_t*, const uint8_t*, int) {
    host->raiseException(host, NEXC_IOException, "io boom");
}
void HsRaiseBase(NativeHost* host, uint8_t*, const uint8_t*, int) {
    host->raiseException(host, NEXC_Base, "base boom");
}
void HsRawRandom(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    native::ReturnInt(ret, static_cast<int32_t>(host->nextRandom(host)));
}
void HsSeededRandom(NativeHost* host, uint8_t* ret, const uint8_t* args, int) {
    host->seedRandom(host, native::ArgInt(args, 0));
    native::ReturnInt(ret, static_cast<int32_t>(host->nextRandom(host)));
}
void HsReadLine(NativeHost* host, uint8_t* ret, const uint8_t*, int) {
    const char* line = host->readLine(host);
    native::ReturnInt(ret, host->newString(host, line));
}

void RegisterHs(VmExecutor& e, const std::string& pkg) {
    //The package prefix is the TU the `native` declarations were compiled
    //from (runSource writes <tag>.n), because a free function's VM key is
    //now "<package>.<name>" (phase 5). Methods keep bare names - none here.
    e.RegisterNative(pkg + ".hsMint", &HsMint);
    e.RegisterNative(pkg + ".hsEcho", &HsEcho);
    e.RegisterNative(pkg + ".hsList", &HsList);
    e.RegisterNative(pkg + ".hsRaiseIo", &HsRaiseIo);
    e.RegisterNative(pkg + ".hsRaiseBase", &HsRaiseBase);
    e.RegisterNative(pkg + ".hsRawRandom", &HsRawRandom);
    e.RegisterNative(pkg + ".hsSeededRandom", &HsSeededRandom);
    e.RegisterNative(pkg + ".hsReadLine", &HsReadLine);
}

// Compile + run; `configure` registers natives / installs host IO.
int runSource(const std::string& tag, const std::string& source,
              std::function<void(VmExecutor&)> configure) {
    const fs::path dir = scratchDir();
    {
        std::ofstream out(dir / (tag + ".n"), std::ios::binary);
        out << "import io;\nimport math;\nimport fs;\n" << source;
    }
    BuildParams params;
    params.m_SourceFiles.push_back((dir / (tag + ".n")).string());
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    ListCompileLogger logger;
    ModuleBuilder builder(params, logger);
    bool built = false;
    try { built = builder.Build(); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "internal error: %s\n", e.what());
        return -1;
    }
    if (!built) {
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            std::fprintf(stderr, "diag: %s\n", (*it)->Message().c_str());
        return -1;
    }
    CompiledModule mod = ModuleLoader::Load(
        (dir / (tag + ".nmod")).string());
    VmExecutor exec;
    configure(exec);
    return exec.Execute(mod);
}

const std::string kDecls =
    "native string hsMint();\n"
    "native string hsEcho(string s);\n"
    "native List<string> hsList();\n"
    "native void hsRaiseIo();\n"
    "native void hsRaiseBase();\n"
    "native int hsRawRandom();\n"
    "native int hsSeededRandom(int seed);\n"
    "native string hsReadLine();\n";

void TestMintEchoListRandomRaise() {
    const std::string source = kDecls +
        "int main() {\n"
        "  if (hsMint() != \"minted\") return 1;\n"
        "  if (hsEcho(\"echo-me\") != \"echo-me\") return 2;\n"
        "  List<string> l = hsList();\n"
        "  if (l.length() != 2) return 3;\n"
        "  if (l.get(0) != \"x\" || l.get(1) != \"y\") return 4;\n"
        "  if (hsRawRandom() == hsRawRandom()) return 5;\n"
        "  if (hsSeededRandom(42) != hsSeededRandom(42)) return 6;\n"
        "  int caughtIo = 0;\n"
        "  try {\n"
        "    hsRaiseIo();\n"
        "  } catch (IOException e) {\n"
        "    caughtIo = 1;\n"
        "  }\n"
        "  if (!caughtIo) return 7;\n"
        "  int caughtBase = 0;\n"
        "  try {\n"
        "    hsRaiseBase();\n"
        "  } catch (Exception e) {\n"
        "    caughtBase = 1;\n"
        "  }\n"
        "  if (!caughtBase) return 8;\n"
        "  return 0;\n"
        "}\n";
    int rc = runSource("host_core", source,
                       [](VmExecutor& e) { RegisterHs(e, "host_core"); });
    CHECK(rc == 0, "mint/echo/list/random/raise callbacks (rc)");
}

void TestReadLine() {
    const fs::path dir = scratchDir();
    const fs::path inFile = dir / "stdin.txt";
    {
        std::ofstream out(inFile, std::ios::binary);
        out << "line-from-stdin\n";
    }
    // Redirect the C stdin (and, via default sync, std::cin) to the file.
    if (!std::freopen(inFile.string().c_str(), "r", stdin)) {
        ++g_fail;
        std::fprintf(stderr, "FAIL: could not redirect stdin\n");
        return;
    }
    const std::string source = kDecls +
        "int main() { if (hsReadLine() != \"line-from-stdin\") return 1; return 0; }\n";
    int rc = runSource("host_readline", source,
                       [](VmExecutor& e) { RegisterHs(e, "host_readline"); });
    CHECK(rc == 0, "readLine callback (rc)");
}

struct CapturingIo : IHostIo {
    std::string text;
    void OnOutput(std::string_view v) override { text += v; }
};

void TestWriteOutput() {
    CapturingIo io;
    const std::string source =
        "int main() { io.print(\"write-check\"); return 0; }\n";
    int rc = runSource("host_write", source, [&io](VmExecutor& e) {
        e.SetHostIo(&io);
    });
    CHECK(rc == 0, "writeOutput program runs");
    CHECK(io.text == "write-check\n", "writeOutput routes through host IO");
}

} // namespace

int main() {
    Runtime::StaticInit();
    std::fprintf(stderr, "=== NativeHost Callback Integration Tests ===\n");
    TestMintEchoListRandomRaise();
    TestReadLine();
    TestWriteOutput();
    std::fprintf(stderr, "=== Results: %d passed, %d failed ===\n",
                 g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
