//Source-entry embedding tests (spec 2026-10-15-embedding-source-entry):
//runSource/runFile compile+execute ad-hoc scripts in-process, through
//the public header, with real compilation and real execution (no mocks).
#include "nlang/embed/NLang.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

#ifndef EMBED_DEPLOY_DIR
#error "EMBED_DEPLOY_DIR must be defined by the build"
#endif

//One captured output pair (out/err channels stay separate through
//setOutputHandler).
struct OutputSink {
    std::string out, err;
};
static void InstallSink(Interpreter& itp, OutputSink& sink) {
    itp.setOutputHandler(
        [&sink](const char* text) { sink.out += text; },
        [&sink](const char* text) { sink.err += text; });
}

int main() {
    initialize();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
