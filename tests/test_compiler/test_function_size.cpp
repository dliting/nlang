/*---
test_function_size.cpp - per-function bytecode size limit unit tests.

The executor's jump operands carry an absolute in-function pc as uint16
(VmExecutor::OpJump reads uint16 and Seeks to it), so a function whose
compiled bytecode reaches 64 KiB gets branch targets that silently wrap
to low addresses: the function mis-executes with no diagnostic. The
backend must reject such a function with a named error instead of
emitting corrupt bytecode (the differential-fixture silent-exit family,
v0.13.7 Task 15). Console suite (same shape as test_array_flags).
---*/
#include "test_support/test_scratch.h"
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

//Collects error diagnostics in memory (same shape as test_array_property).
class MemLogger : public CompileLogger
{
public:
    void WriteLog(CompileLogLevel level, const ISourceLocation*,
        const char* szMessage) override
    {
        if (level == CLL_Error || level == CLL_Fatal)
            m_errors.emplace_back(szMessage);
    }
    const std::vector<std::string>& errors() const { return m_errors; }
private:
    std::vector<std::string> m_errors;
};

//ModuleBuilder/BuildEnvironment hold REFERENCES to their constructor
//inputs (ownership note per test_array_property), so the outcome struct
//owns all three for as long as the caller touches the tree. ModuleManager
//is process-global and refuses a second Create of the same module name,
//so the output module name is uniquified per call.
struct CompileOutcome
{
    std::unique_ptr<BuildParams> params;
    std::unique_ptr<MemLogger> logger;
    std::unique_ptr<ModuleBuilder> builder;
    bool ok = false;
};

static CompileOutcome compileOne(const std::string& sBody)
{
    static uint32_t runCount = 0;
    const auto dir = nlang::test::ScratchDir("nlang_function_size_tests");
    std::error_code fsError;
    std::filesystem::create_directories(dir, fsError);
    const auto file = dir / "main.n";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << sBody;
        if (!stream.good())
            return {};
    }
    CompileOutcome out;
    out.params = std::make_unique<BuildParams>();
    out.params->m_SourceFiles.push_back(file.string());
    out.params->m_sProjectDir = dir.string();
    out.params->m_sOutputModule =
        "function_size_" + std::to_string(++runCount);
    out.params->m_sOutputDir = dir.string();
    out.params->m_sTempDir = dir.string();
    out.logger = std::make_unique<MemLogger>();
    out.builder = std::make_unique<ModuleBuilder>(*out.params, *out.logger);
    try { out.ok = out.builder->BuildArtifacts(); }
    catch (const std::exception&) { out.ok = false; }
    return out;
}

//A function holding nIfs straight-line "if (x == k) { y = y + 1; }"
//statements: each compiles to a fixed few dozen bytes, so nIfs past
//~2500 crosses the 64 KiB jump-target ceiling. The threshold is pinned
//by the two tests well inside (accept at 300, reject at 8000): the
//exact boundary is codegen-layout sensitive, so it is not asserted on
//a fixed-size source.
static std::string sourceWithIfs(int nIfs)
{
    std::string body = "int huge() {\n    int x = 0;\n    int y = 0;\n";
    for (int i = 0; i < nIfs; ++i)
        body += "    if (x == " + std::to_string(i) + ") { y = y + 1; }\n";
    body += "    return y;\n}\nint main() { return huge(); }\n";
    return body;
}

static std::string joinErrors(const CompileOutcome& out)
{
    std::string all;
    for (const auto& e : out.logger->errors())
        all += e + "\n";
    return all;
}

//A function whose bytecode crosses the 16-bit jump range must fail the
//build with a diagnostic naming the function — not compile silently and
//mis-execute at runtime.
static void test_oversized_function_rejected()
{
    TEST(oversized_function_rejected);
    auto out = compileOne(sourceWithIfs(8000));
    CHECK(!out.ok, "8000-statement function must exceed the jump range "
        "and fail the build");
    bool namesFunction = false;
    for (const auto& e : out.logger->errors()) {
        if (e.find("function 'huge'") != std::string::npos)
            namesFunction = true;
    }
    CHECK(namesFunction, "a diagnostic must name the offending function, "
        "got: " + joinErrors(out));
    PASS();
}

//The guard must not fire on ordinary functions (tripwire honesty: the
//reject test alone would also pass if every build failed).
static void test_normal_function_accepted()
{
    TEST(normal_function_accepted);
    auto out = compileOne(sourceWithIfs(300));
    CHECK(out.ok, "300-statement function must stay inside the jump "
        "range, got: " + joinErrors(out));
    PASS();
}

int main()
{
    Runtime::StaticInit();
    TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)
    test_oversized_function_rejected();
    test_normal_function_accepted();
    std::cerr << "\nfunction_size: " << g_pass << " passed, "
        << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
