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

int main() {
    TestRunReturnsMainExitCode();
    TestRunTwiceThrowsBadValue();
    TestLoadTwiceThrowsBadValue();
    TestMissingArtifactThrowsLoadError();
    TestAddImportDirAfterLoadThrows();
    TestUncaughtScriptThrowSurfacesException();
    std::cerr << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
