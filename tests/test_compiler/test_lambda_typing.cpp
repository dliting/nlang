/*---
    test_lambda_typing.cpp — lambda typing through the expected-type
    channel (lambda/closure plan Task 4, spec §3.2/§3.5).
    Positive cases assert RESOLUTION ARTIFACTS on the tree (ModuleBuilder::
    TreeRootView): the lambda flags NF_Resolved, stamps the expected
    func<...> instantiation, and fills one param descriptor per parameter.
    The emission phase has no lambda lowering until Task 5, so the full
    build may fail past resolution — `resolutionErrors` filters the
    backend's "NLang backend" seam text and asserts the resolution phase
    itself stayed silent. Negative cases assert diagnostic COUNTS of the
    named needles (the decomposed local-decl revisit is the double-report
    trap — substring existence cannot catch it).
---*/
#include "test_support/test_scratch.h"
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnStatements.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace nlang;

namespace {

//Collects error diagnostics in memory (same shape as test_lambda_params).
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
//inputs (ownership note per test_lambda_params), so the outcome struct
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
    const auto dir = nlang::test::ScratchDir("nlang_lambda_typing_tests");
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
        "lambda_typing_" + std::to_string(++runCount);
    out.params->m_sOutputDir = dir.string();
    out.params->m_sTempDir = dir.string();
    out.logger = std::make_unique<MemLogger>();
    out.builder = std::make_unique<ModuleBuilder>(*out.params, *out.logger);
    try { out.ok = out.builder->BuildArtifacts(); }
    catch (const std::exception&) { out.ok = false; }
    return out;
}

int countErrorsContaining(const CompileOutcome& out, const char* szNeedle)
{
    int hits = 0;
    for (const auto& text : out.logger->errorsText())
        if (text.find(szNeedle) != std::string::npos)
            ++hits;
    return hits;
}

//Errors from the RESOLUTION phase — everything except the backend's
//"NLang backend" seam (Task 4 delivers resolution only; Task 5-7 light
//the emission up, so the seam failure is expected and filtered).
int countResolutionErrors(const CompileOutcome& out)
{
    int hits = 0;
    for (const auto& text : out.logger->errorsText())
        if (text.find("NLang backend") == std::string::npos)
            ++hits;
    return hits;
}

//First NK_LambdaExpr in the subtree — null when none resolved there.
//(Children() iterates Node&, so the walk is typed on Node, not SyntaxNode;
//the const list exposes only cbegin/cend, hence the explicit loop.)
const SnLambdaExpr* findLambda(const Node& node)
{
    if (node.Kind() == NK_LambdaExpr)
        return static_cast<const SnLambdaExpr*>(&node);
    const auto& children = node.Children();
    for (auto it = children.cbegin(); it != children.cend(); ++it)
        if (const auto* pHit = findLambda(*it))
            return pHit;
    return nullptr;
}

//First NK_IdentifierExpr spelled `name` in the subtree — null when none.
const SnIdentifierExpr* findIdentifier(const Node& node, const char* name)
{
    if (node.Kind() == NK_IdentifierExpr
        && static_cast<const SnIdentifierExpr&>(node).Name() == name)
        return static_cast<const SnIdentifierExpr*>(&node);
    const auto& children = node.Children();
    for (auto it = children.cbegin(); it != children.cend(); ++it)
        if (const auto* pHit = findIdentifier(*it, name))
            return pHit;
    return nullptr;
}

//The stamped func<...> instantiation of a resolved lambda — null when
//the node carries no class-decl type (unresolved or misstamped).
const SnClassDecl* stampedFuncDecl(const SnLambdaExpr& lambda)
{
    auto* pType = lambda.EvalDataType();
    if (!pType || pType->Kind() != NK_ClassDecl)
        return nullptr;
    return static_cast<const SnClassDecl*>(pType);
}

//The stingiest placement: the local-decl initializer decomposes into an
//AssignStmt that the paragraph walk revisits — every error exit must
//flag itself resolved or the diagnostics report twice.
const char* const kInitializerWrapper =
    "int main() {\n"
    "    func<int, int> f = %1;\n"
    "    return 0;\n"
    "}\n";

std::string wrapped(const char* szLambda)
{
    return QString(kInitializerWrapper).arg(szLambda).toStdString();
}

//The func<void> placement for the D6 discard rule (a value under a void
//expectation is the discard, not an arity mismatch).
const char* const kVoidWrapper =
    "int main() {\n"
    "    func<void> g = %1;\n"
    "    return 0;\n"
    "}\n";

std::string wrappedVoid(const char* szLambda)
{
    return QString(kVoidWrapper).arg(szLambda).toStdString();
}

//The &-formal and &-return placement (spec §3.3) — the ref-flag row of
//the expected type is what the &-marked parameter spells against.
const char* const kRefWrapper =
    "int main() {\n"
    "    func<int&, int&> f = %1;\n"
    "    return 0;\n"
    "}\n";

std::string wrappedRef(const char* szLambda)
{
    return QString(kRefWrapper).arg(szLambda).toStdString();
}

} //namespace

class TestLambdaTyping : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)
    }

    //Bare parameter filled from the expected type: `(x) -> x*2` under
    //`func<int, int>` types x as int and stamps the func instantiation.
    void TestTypedParamInferredFromExpected() {
        auto out = compileOne(wrapped("(x) -> x * 2").c_str());
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        QVERIFY(pLambda->IsResolved());
        QCOMPARE(pLambda->ParamLocals().size(), size_t(1));
        auto* pParam = pLambda->ParamLocals()[0].get();
        QVERIFY(pParam->EvalDataType() != nullptr);
        QCOMPARE(pParam->EvalDataType()->Kind(), NK_Int32);
        auto* pFunc = stampedFuncDecl(*pLambda);
        QVERIFY(pFunc != nullptr);
        QVERIFY(pFunc->IsGenericInstantiation());
        QCOMPARE(pFunc->GenericTypeArgs().size(), size_t(2));
        QCOMPARE(pFunc->GenericTypeArgs()[0]->Kind(), NK_Int32);
        QCOMPARE(pFunc->GenericTypeArgs()[1]->Kind(), NK_Int32);
        QCOMPARE(countResolutionErrors(out), 0);
        QCOMPARE(countErrorsContaining(out, "lambda"), 0);
    }

    //No expected type — a lambda as a binary operand never receives an
    //InferredTarget: the named no-target diagnostic fires.
    void TestNoExpectedTypeRejected() {
        auto out = compileOne(wrapped("1 + ((x) -> x)").c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "requires an expected function type"), 1);
    }

    //Explicit parameter types must match the expected signature
    //position-for-position, with no variance.
    void TestExplicitParamTypeMismatch() {
        auto out = compileOne(wrapped("(float x) -> x").c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out, "lambda parameter"), 1);
    }

    //D6: a func<void> expectation discards nothing silently — a
    //value-yielding expression body is a compile error.
    void TestVoidExprBodyDiscardRejected() {
        auto out = compileOne(wrappedVoid("() -> 1").c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out, "yields a value"), 1);
    }

    //The zero-parameter lambda against a one-parameter expected type:
    //the arity gate names both counts.
    void TestZeroParamArityMismatch() {
        auto out = compileOne(wrapped("() -> 1").c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out, "yields a value"), 0);
        QCOMPARE(countErrorsContaining(out, "takes 0 parameters"), 1);
    }

    //A non-void expression body whose type disagrees with the expected
    //return type rejects through the cast choke point — the same
    //return-position discipline as a function's `return` statement
    //(FinishReturnExpr), which keeps the generic wording.
    void TestBodyTypeMismatchRejected() {
        auto out = compileOne(wrapped("(x) -> \"s\"").c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "Incompatible type"), 1);
    }

    //「块体void成形」: a block body without returns types void under a
    //func<void> expectation; the body's locals stay scoped to the block.
    void TestBlockBodyVoidFormed() {
        auto out = compileOne(
            "int main() {\n"
            "    func<void> g = () -> { int q = 1; q += 2; };\n"
            "    return 0;\n"
            "}\n");
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        QVERIFY(pLambda->IsResolved());
        //Task 6's capture analysis moves the body into the synthetic
        //method — the block now lives there, the lambda keeps the
        //pointer pair.
        QVERIFY(pLambda->SyntheticMethod() != nullptr);
        QVERIFY(pLambda->SyntheticMethod()->Body() != nullptr);
        auto* pFunc = stampedFuncDecl(*pLambda);
        QVERIFY(pFunc != nullptr);
        QCOMPARE(pFunc->GenericTypeArgs().size(), size_t(1));
        QCOMPARE(pFunc->GenericTypeArgs()[0]->Kind(), NK_Void);
        QCOMPARE(countResolutionErrors(out), 0);
        QCOMPARE(countErrorsContaining(out, "lambda"), 0);
    }

    //A block body's return checks against the lambda's own return type,
    //and the parameter scopes into the block (the descriptor answers the
    //body's name lookup before the enclosing function's locals).
    void TestBlockReturnTyped() {
        auto out = compileOne(
            "int main() {\n"
            "    func<int, int> f = (x) -> { return x * 2; };\n"
            "    return 0;\n"
            "}\n");
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        QVERIFY(pLambda->IsResolved());
        QCOMPARE(pLambda->ParamLocals().size(), size_t(1));
        auto* pParam = pLambda->ParamLocals()[0].get();
        //Task 6 moved the body into the synthetic method. The
        //resolution-time bindings still answer the original parameter
        //descriptor (emission reads them there); the moved block's own
        //name lookup answers the rebuilt formal instead.
        QVERIFY(pLambda->SyntheticMethod() != nullptr);
        auto* pUse = findIdentifier(*pLambda->SyntheticMethod()->Body(), "x");
        QVERIFY(pUse != nullptr);
        QCOMPARE(pUse->Field(), static_cast<SnField*>(pParam));
        QCOMPARE(countResolutionErrors(out), 0);
        QCOMPARE(countErrorsContaining(out, "lambda"), 0);
    }

    //「块体无return即void」: a return-less block body against a non-void
    //expectation is a named error, not a silent garbage value.
    void TestBlockNeverReturnsRejected() {
        auto out = compileOne(
            "int main() {\n"
            "    func<int, int> f = (x) -> { int q = x; };\n"
            "    return 0;\n"
            "}\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out, "never returns"), 1);
    }

    //A bare return inside a lambda block rejects when the expected
    //return type carries a value.
    void TestBareReturnInBlockRejected() {
        auto out = compileOne(
            "int main() {\n"
            "    func<int, int> f = (x) -> { return; };\n"
            "    return 0;\n"
            "}\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "Missing return value"), 1);
    }

    //A value return inside a lambda block rejects under a void
    //expectation (no silent discard on the return path either).
    void TestValueReturnAgainstVoidRejected() {
        auto out = compileOne(
            "int main() {\n"
            "    func<void> g = () -> { return 1; };\n"
            "    return 0;\n"
            "}\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out, "returns void"), 1);
    }

    //&-formal and &-return signature: `int& x` parses as the reference
    //parameter spelling, the descriptor carries the mark, and the body
    //types against the &-returning expectation.
    void TestRefParamAndReturnTyped() {
        auto out = compileOne(wrappedRef("(int& x) -> x").c_str());
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        QVERIFY(pLambda->IsResolved());
        QCOMPARE(pLambda->ParamLocals().size(), size_t(1));
        auto* pParam = pLambda->ParamLocals()[0].get();
        QVERIFY(pParam->ContainFlags(NF_Ref));
        QVERIFY(pParam->EvalDataType() != nullptr);
        QCOMPARE(pParam->EvalDataType()->Kind(), NK_Int32);
        QVERIFY(stampedFuncDecl(*pLambda) != nullptr);
        QCOMPARE(countResolutionErrors(out), 0);
        QCOMPARE(countErrorsContaining(out, "lambda"), 0);
    }

    //The & mark joins the position-for-position agreement (no variance):
    //a reference-marked parameter under a plain-value expectation is the
    //named parameter mismatch.
    void TestRefParamMarkMismatchRejected() {
        auto out = compileOne(wrapped("(int& x) -> x").c_str());
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out, "lambda parameter"), 1);
    }

    //A lambda call argument defers like a brace argument: the chosen
    //overload's formal type becomes the target.
    void TestLambdaCallArgumentTyped() {
        auto out = compileOne(
            "void run(func<int, int> g) { }\n"
            "int main() {\n"
            "    run((x) -> x * 2);\n"
            "    return 0;\n"
            "}\n");
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        QVERIFY(pLambda->IsResolved());
        QCOMPARE(pLambda->ParamLocals().size(), size_t(1));
        QCOMPARE(pLambda->ParamLocals()[0]->EvalDataType()->Kind(),
            NK_Int32);
        QCOMPARE(countResolutionErrors(out), 0);
        QCOMPARE(countErrorsContaining(out, "lambda"), 0);
    }

    //A lambda entry of a container literal inherits the element type —
    //the same InferredTarget push the nested brace entries ride.
    void TestLambdaListEntryTyped() {
        auto out = compileOne(
            "int main() {\n"
            "    List<func<int, int>> l = [(x) -> x];\n"
            "    return 0;\n"
            "}\n");
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        QVERIFY(pLambda->IsResolved());
        QCOMPARE(pLambda->ParamLocals().size(), size_t(1));
        QCOMPARE(countResolutionErrors(out), 0);
        QCOMPARE(countErrorsContaining(out, "lambda"), 0);
    }

    //Field-initializer rejection (spec §3.5, citing the constant-
    //expression rules §4.8.3/§4.7.2): the existing constant-surface
    //gates reject the func-typed field initializer before any lambda
    //resolution — no lambda-named diagnostic fires.
    void TestInstanceFieldInitializerRejected() {
        auto out = compileOne(
            "class C {\n"
            "    func<int, int> m = (x) -> x;\n"
            "}\n"
            "int main() {\n"
            "    return 0;\n"
            "}\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "cannot have an initializer"), 1);
        QCOMPARE(countErrorsContaining(out, "lambda"), 0);
    }

    //A static field initializer is a runtime store (v0.12.6 $staticInit),
    //not a constant-expression position — the lambda types through the
    //ordinary assignment channel.
    void TestStaticFieldLambdaResolves() {
        auto out = compileOne(
            "class C {\n"
            "    static func<int, int> m = (x) -> x;\n"
            "}\n"
            "int main() {\n"
            "    return 0;\n"
            "}\n");
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        QVERIFY(pLambda->IsResolved());
        QCOMPARE(pLambda->ParamLocals().size(), size_t(1));
        QCOMPARE(countResolutionErrors(out), 0);
        QCOMPARE(countErrorsContaining(out, "lambda"), 0);
    }
};

QTEST_GUILESS_MAIN(TestLambdaTyping)
#include "test_lambda_typing.moc"
