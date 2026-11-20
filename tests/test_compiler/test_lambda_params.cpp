/*---
    test_lambda_params.cpp — lambda parameter-shape gate unit tests
    (lambda/closure plan Task 3, spec §3.5).
    The cover grammar delivers every list item that is not a parameter
    shape as a raw expression; the resolver's parameter gate rejects
    those with a verbatim diagnostic. Each case asserts the DIAGNOSTIC
    COUNT of the needle (the decomposed local-decl revisit is the
    double-report trap — substring existence cannot catch it).
    The default-value and generic-list spellings are pinned at the
    parse layer: NLang has no assignment-as-expression and no `<`-led
    expression, so those shapes are syntax errors before any resolver
    run (ruled unreachable for the named diagnostics).
---*/
#include "test_support/test_scratch.h"
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace nlang;

namespace {

//Collects error diagnostics in memory (same shape as test_statement_diag).
class MemLogger : public CompileLogger
{
public:
    const std::vector<std::string>& errorsText() const { return m_errors; }
protected:
    void WriteLog(CompileLogLevel level, const ISourceLocation*,
        const char* szMessage) override
    {
        if (level == CLL_Error || level == CLL_Fatal)
            m_errors.emplace_back(szMessage);
    }
private:
    std::vector<std::string> m_errors;
};

//ModuleBuilder/BuildEnvironment hold REFERENCES to their constructor
//inputs (ownership note per test_statement_diag), so the outcome struct
//owns all three for as long as the caller touches the tree.
struct CompileOutcome
{
    std::unique_ptr<BuildParams> params;
    std::unique_ptr<MemLogger> logger;
    std::unique_ptr<ModuleBuilder> builder;
    bool ok = false;
};

//Compile one source to a fresh temp module (the module name is
//uniquified per call: ModuleManager is process-global and refuses a
//second Create of the same name).
CompileOutcome compileOne(const char* szBody)
{
    static uint32_t runCount = 0;
    const auto dir = nlang::test::ScratchDir("nlang_lambda_param_tests");
    std::error_code fsError;
    std::filesystem::create_directories(dir, fsError);
    const auto file = dir / "main.n";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << szBody;
        if (!stream.good())
            return {};
    }
    CompileOutcome out;
    out.params = std::make_unique<BuildParams>();
    out.params->m_SourceFiles.push_back(file.string());
    out.params->m_sProjectDir = dir.string();
    out.params->m_sOutputModule =
        "lambda_param_" + std::to_string(++runCount);
    out.params->m_sOutputDir = dir.string();
    out.params->m_sTempDir = dir.string();
    out.logger = std::make_unique<MemLogger>();
    out.builder = std::make_unique<ModuleBuilder>(*out.params, *out.logger);
    try { out.ok = out.builder->BuildArtifacts(); }
    catch (const std::exception&) { out.ok = false; }
    return out;
}

//Errors whose text contains szNeedle — the count IS the assertion.
int countErrorsContaining(const CompileOutcome& out, const char* szNeedle)
{
    int hits = 0;
    for (const auto& text : out.logger->errorsText())
        if (text.find(szNeedle) != std::string::npos)
            ++hits;
    return hits;
}

//The local-decl initializer is the stingiest placement: it decomposes
//into an AssignStmt that the paragraph walk revisits, so a gate that
//does not flag its error exit reports twice.
const char* const kInitializerWrapper =
    "int main() {\n"
    "    func<int, int> f = %1;\n"
    "    return 0;\n"
    "}\n";

} //namespace

class TestLambdaParams : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)
    }

    //Raw literal item: `(1) => 1`.
    void TestRawLiteralParamRejected() {
        const auto body = QString(kInitializerWrapper)
            .arg("(1) => 1").toStdString();
        auto out = compileOne(body.c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "invalid lambda parameter list"), 1);
    }

    //Raw member access: `(x.y) => 1` — the member expression rides the
    //cover list as an expression (the ParenHeadType twin has no '.' arm).
    void TestMemberAccessParamRejected() {
        const auto body = QString(kInitializerWrapper)
            .arg("(x.y) => 1").toStdString();
        auto out = compileOne(body.c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "invalid lambda parameter list"), 1);
    }

    //Raw invoke: `(f()) => 1`.
    void TestInvokeParamRejected() {
        const auto body = QString(kInitializerWrapper)
            .arg("(f()) => 1").toStdString();
        auto out = compileOne(body.c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "invalid lambda parameter list"), 1);
    }

    //The generic-parameter-list spelling `(T<int> x) => 1` (spec §3.5
    //"lambda带泛型参数表"): the grammar has no `<T>` slot; the plain-'<'
    //comparison reading owns the tokens, the chain arrives raw and the
    //gate rejects it.
    void TestGenericListSpellingRejected() {
        const auto body = QString(kInitializerWrapper)
            .arg("(T<int> x) => 1").toStdString();
        auto out = compileOne(body.c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "invalid lambda parameter list"), 1);
    }

    //A deeper bit-and chain stays raw: only the two-bare-operand shape
    //is reinterpreted as a reference parameter (Task 2 ruling), so
    //`(int & x & y) => 1` reaches the gate.
    void TestDeepBitAndChainRejected() {
        const auto body = QString(kInitializerWrapper)
            .arg("(int & x & y) => 1").toStdString();
        auto out = compileOne(body.c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "invalid lambda parameter list"), 1);
    }

    //The default-value spelling `(x = 3) => x` never reaches the gate:
    //NLang has no assignment-as-expression (the `=` lives only in the
    //call-site named-argument production), so the cover list rejects it
    //at parse time. Rejected is what spec §3.5 wants; the named
    //diagnostic would be dead code.
    void TestDefaultValueSpellingIsParseError() {
        const auto body = QString(kInitializerWrapper)
            .arg("(x = 3) => x").toStdString();
        auto out = compileOne(body.c_str());
        QVERIFY(!out.ok);
        QVERIFY(countErrorsContaining(out, "syntax error") > 0);
        QCOMPARE(countErrorsContaining(
            out, "invalid lambda parameter list"), 0);
    }

    //Same for the `<`-led generic list `(<T> x) => 1`: no expression
    //production starts with '<'.
    void TestAngleLedListIsParseError() {
        const auto body = QString(kInitializerWrapper)
            .arg("(<T> x) => 1").toStdString();
        auto out = compileOne(body.c_str());
        QVERIFY(!out.ok);
        QVERIFY(countErrorsContaining(out, "syntax error") > 0);
        QCOMPARE(countErrorsContaining(
            out, "invalid lambda parameter list"), 0);
    }
};

QTEST_GUILESS_MAIN(TestLambdaParams)
#include "test_lambda_params.moc"
