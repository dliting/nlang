// test_module_state.cpp — per-executor module state for the nlang_net
// module (net library design §6, rulings D8/R8): the socket slot table
// lives in the executor's moduleState slot, not in process-global
// storage. Two live executors in one process mint independent handles
// (each first bind returns handle 1), and an executor's teardown closes
// its still-open fds so the next executor can rebind the port (the D12
// backstop). Real compiler, real bytecode, real sockets — no mocks.
#include "test_support/test_scratch.h"

#include "nlang/compiler/ModuleBuilder.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/compiler/Logger.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/vm/CompiledModule.h"
#include "VmExecutor.h"
#include "NcuLoader.h"
#include "NcuLinker.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if (cond) ++g_pass; else { ++g_fail; std::fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

namespace {

namespace fs = std::filesystem;

// The probe program: bind an ephemeral listener through the module
// native and KEEP it open (no close) — the exit code carries
// port*8 + handle so the test can read both back. A shared (static)
// slot table would hand the second executor handle 2; independent
// per-executor tables hand both executors handle 1.
const char* kProbeSource =
    "import net;\n"
    "int main() {\n"
    "  int h = net._tcpBind(\"127.0.0.1\", 0, 512);\n"
    "  if (h < 1) return -1;\n"
    "  int p = net._tcpLocalPort(h);\n"
    "  if (p < 1) return -2;\n"
    "  return p * 8 + h;\n"
    "}\n";

// The rebind program: take over the port the probe executors left
// (their teardown must have closed the fds), through the script class
// face, and close cleanly.
std::string fixedBindSource(int port) {
    const std::string p = std::to_string(port);
    return std::string("import net;\n"
        "int main() {\n"
        "  TcpListener l = TcpListener.bind(\"127.0.0.1\", ")
        + p + ");\n"
        "  int got = l.localPort();\n"
        "  l.close();\n"
        "  if (got == " + p + ") { return 0; }\n"
        "  return 1;\n"
        "}\n";
}

// Compile main.n (the given source) in a fresh scenario dir against
// the built stdlib package; returns the linked image. Throws on a
// build or load/link failure so the caller reports a failed CHECK
// instead of crashing the binary.
CompiledModule buildModule(const fs::path& dir, const std::string& source) {
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "main.n", std::ios::binary);
        out << source;
    }
    BuildParams params;
    params.m_SourceFiles.push_back((dir / "main.n").string());
    params.m_sProjectDir = dir.string();
    params.m_sOutputModule = dir.filename().string();
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    params.m_ImportDirs = { dir.string(), STDLIB_DIR };
    ListCompileLogger logger;
    if (!ModuleBuilder(params, logger).BuildArtifacts()) {
        std::string log;
        for (auto it = logger.cbegin(); it != logger.cend(); ++it)
            log += (*it)->Message() + "\n";
        throw std::runtime_error("build failed:\n" + log);
    }
    NcuLoader::Options loaderOpts;
    loaderOpts.searchDirs = { dir.string(), STDLIB_DIR };
    NcuLoader::Result loaded = NcuLoader::LoadClosure(
        (dir / (dir.filename().string() + ".ncu")).string(), loaderOpts);
    return NcuLinker::Link(std::move(loaded.units), loaded.entryKey);
}

}  //namespace

int main() {
    //The compiler pipeline's static-init contract (IdString intern
    //tables + cast table) — required before any ModuleBuilder use.
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();

    const fs::path root =
        nlang::test::ScratchDir("nlang_test_module_state");

    int codeA = -1, codeB = -1;
    try {
        CompiledModule probeMod =
            buildModule(root / "probe", kProbeSource);
        //A and B coexist: each executor owns its own nlang_net state.
        VmExecutor execA;
        codeA = execA.Execute(probeMod);
        {
            VmExecutor execB;
            codeB = execB.Execute(probeMod);
        }   //B's teardown closes its still-open listener
        //A is destroyed at the end of this scope — its teardown must
        //close the fd it never closed in script.
    } catch (const std::exception& e) {
        std::fprintf(stderr, "probe scenario: %s\n", e.what());
    }

    CHECK(codeA > 0 && codeA % 8 == 1,
          "executor A: first bind mints handle 1");
    CHECK(codeB > 0 && codeB % 8 == 1,
          "executor B: first bind mints handle 1 (independent tables)");
    const int portA = codeA > 0 ? codeA / 8 : 0;
    const int portB = codeB > 0 ? codeB / 8 : 0;
    CHECK(portA > 0 && portB > 0 && portA != portB,
          "both executors hold live distinct ephemeral listeners");

    if (portA > 0) {
        //Reached only after execA's destructor ran.
        try {
            CompiledModule fixedMod =
                buildModule(root / "fixed", fixedBindSource(portA));
            VmExecutor execC;
            const int rcC = execC.Execute(fixedMod);
            CHECK(rcC == 0,
                  "A's teardown closed its fd: the port rebinds (D12)");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "rebind scenario: %s\n", e.what());
            CHECK(false, "rebind scenario built and ran");
        }
    }

    std::printf("module_state: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
