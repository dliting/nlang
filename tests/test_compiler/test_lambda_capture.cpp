/*---
    test_lambda_capture.cpp — display synthesis and capture promotion
    (lambda/closure plan Task 6, spec §4.1). Positive cases assert the
    synthesis artifacts on the tree: a capturing lambda gains a display
    class (the captured storage) and a synthetic method (the lowered
    body); lambdas capturing one variable share that variable's display;
    an & binding captures as a base+offset field pair; a captured
    receiver rides the $this field; a nested-scope display holds an
    $outer reference field; the recursive self-reference captures the
    function value itself. The static-context this reject keeps its
    resolution-phase diagnostic — the redirect must not swallow it
    (plan review M4). RED before Task 6: DisplayClass() is null on every
    lambda.
---*/
#include "test_support/test_scratch.h"
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnStatements.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace nlang;

namespace {

//Collects error diagnostics in memory (same shape as test_lambda_typing).
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

struct CompileOutcome
{
    std::unique_ptr<BuildParams> params;
    std::unique_ptr<MemLogger> logger;
    std::unique_ptr<ModuleBuilder> builder;
    bool ok = false;
};

//Compile one source to a fresh temp module (ModuleManager is
//process-global and refuses a second Create of the same name).
CompileOutcome compileOne(const char* szBody)
{
    static uint32_t runCount = 0;
    const auto dir = nlang::test::ScratchDir("nlang_lambda_capture_tests");
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
        "lambda_capture_" + std::to_string(++runCount);
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

//First NK_LambdaExpr in the subtree — null when none is there.
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

//The display class's data-field names in declaration order — the
//capture field set under test ($-prefixed synthesized slots included).
std::vector<std::string> displayFieldNames(const SnLambdaExpr& lambda)
{
    std::vector<std::string> names;
    auto* pClass = const_cast<SnLambdaExpr&>(lambda).DisplayClass();
    if (!pClass)
        return names;
    for (auto& member : pClass->Members())
        if (member.Kind() == NK_ClassField)
            names.push_back(member.Name());
    return names;
}

//Full-build silence: the capture family compiles end to end (Task 6
//delivers the emission, so no backend seam filtering here).
int allErrors(const CompileOutcome& out)
{
    return static_cast<int>(out.logger->errorsText().size());
}

} //namespace

class TestLambdaCapture : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)
    }

    //A lambda capturing one outer int: the display hosts the captured
    //storage under the variable's own name, and the lambda writes back
    //the display class and its synthetic method.
    void capturesOuterLocal()
    {
        auto out = compileOne(
            "int main() {\n"
            "    int n = 10;\n"
            "    func<int, int> f = (x) -> x + n;\n"
            "    return f(1);\n"
            "}\n");
        QVERIFY(out.ok);
        QCOMPARE(allErrors(out), 0);
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        QVERIFY(pLambda->IsResolved());
        QVERIFY(pLambda->DisplayClass() != nullptr);
        QVERIFY(pLambda->SyntheticMethod() != nullptr);
        const auto names = displayFieldNames(*pLambda);
        QCOMPARE(names.size(), size_t(1));
        if (names.size() == 1)
            QCOMPARE(names[0], std::string("n"));
    }

    //Two lambdas capturing the same variable — sibling blocks included
    //— attach to that variable's declaring scope: one display class,
    //shared storage.
    void siblingLambdasShareDisplay()
    {
        auto out = compileOne(
            "int main() {\n"
            "    int n = 0;\n"
            "    func<int, int> a = null;\n"
            "    func<int, int> b = null;\n"
            "    {\n"
            "        a = (x) -> x + n;\n"
            "    }\n"
            "    {\n"
            "        b = (x) -> x + n;\n"
            "    }\n"
            "    return a(1) + b(2);\n"
            "}\n");
        QVERIFY(out.ok);
        QCOMPARE(allErrors(out), 0);
        std::vector<const SnLambdaExpr*> lambdas;
        std::function<void(const Node&)> walk = [&](const Node& node)
        {
            if (node.Kind() == NK_LambdaExpr)
                lambdas.push_back(static_cast<const SnLambdaExpr*>(&node));
            const auto& children = node.Children();
            for (auto it = children.cbegin(); it != children.cend(); ++it)
                walk(*it);
        };
        walk(out.builder->TreeRootView());
        QCOMPARE(lambdas.size(), size_t(2));
        QVERIFY(lambdas[0]->DisplayClass() != nullptr);
        QVERIFY(lambdas[1]->DisplayClass() != nullptr);
        QCOMPARE(static_cast<const void*>(lambdas[0]->DisplayClass()),
                 static_cast<const void*>(lambdas[1]->DisplayClass()));
    }

    //Capturing an & binding stores the reference pair: ONE display
    //field (the 8-byte pair copy preserves the (base, cell) alias).
    void refCaptureBaseOffsetPair()
    {
        auto out = compileOne(
            "int main() {\n"
            "    int base = 1;\n"
            "    int& r = base;\n"
            "    func<int, int> f = (x) -> x + r;\n"
            "    return f(1);\n"
            "}\n");
        QVERIFY(out.ok);
        QCOMPARE(allErrors(out), 0);
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        const auto names = displayFieldNames(*pLambda);
        QCOMPARE(names.size(), size_t(1));
        if (names.size() == 1)
            QCOMPARE(names[0], std::string("r"));
    }

    //Instance-member access inside a lambda captures the receiver: the
    //display carries the $this field.
    void thisCaptureReceiverField()
    {
        auto out = compileOne(
            "class C {\n"
            "    int count;\n"
            "    public func<int, int> bound() {\n"
            "        count = 0;\n"
            "        return (x) -> x + count;\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    C c = C();\n"
            "    func<int, int> g = c.bound();\n"
            "    return g(1);\n"
            "}\n");
        QVERIFY(out.ok);
        QCOMPARE(allErrors(out), 0);
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        const auto names = displayFieldNames(*pLambda);
        bool bHasThis = false;
        for (const auto& name : names)
            if (name == "$this")
                bHasThis = true;
        QVERIFY(bHasThis);
    }

    //A lambda whose scope nests inside another captured scope: the
    //inner display holds the $outer reference field (the chase path).
    void nestedScopeOuterRefField()
    {
        auto out = compileOne(
            "int main() {\n"
            "    int n = 5;\n"
            "    func<int, int> f = null;\n"
            "    {\n"
            "        int m = 1;\n"
            "        f = (x) -> x + m + n;\n"
            "    }\n"
            "    return f(1);\n"
            "}\n");
        QVERIFY(out.ok);
        QCOMPARE(allErrors(out), 0);
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        const auto names = displayFieldNames(*pLambda);
        //m rides the inner display, n chases through $outer.
        bool bHasM = false;
        bool bHasOuter = false;
        for (const auto& name : names) {
            if (name == "m")
                bHasM = true;
            if (name == "$outer")
                bHasOuter = true;
        }
        QVERIFY(bHasM);
        QVERIFY(bHasOuter);
    }

    //The recursive self-reference: the function value being declared is
    //captured before the lambda is created (the display field is read
    //at call time). Block body — the language has no ternary, so the
    //base case needs a statement.
    void recursiveSelfCapture()
    {
        auto out = compileOne(
            "int main() {\n"
            "    func<int, int> f = (x) -> { if (x <= 1) { return 1; }"
            " return x * f(x - 1); };\n"
            "    return f(5);\n"
            "}\n");
        QVERIFY(out.ok);
        QCOMPARE(allErrors(out), 0);
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        const auto names = displayFieldNames(*pLambda);
        QCOMPARE(names.size(), size_t(1));
        if (names.size() == 1)
            QCOMPARE(names[0], std::string("f"));
    }

    //An early exit before the lambda's position must not fork the
    //display allocation: the synthesis is path-independent (the display
    //still exists with the captured field).
    void earlyExitScopeSynthesizesOnce()
    {
        auto out = compileOne(
            "int f2(int v) {\n"
            "    if (v > 0) { return v; }\n"
            "    return 0;\n"
            "}\n"
            "int main() {\n"
            "    int n = 5;\n"
            "    if (n > 3) { return f2(n); }\n"
            "    func<int, int> g = (x) -> x + n;\n"
            "    return g(1);\n"
            "}\n");
        QVERIFY(out.ok);
        QCOMPARE(allErrors(out), 0);
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        QVERIFY(pLambda->DisplayClass() != nullptr);
        const auto names = displayFieldNames(*pLambda);
        QCOMPARE(names.size(), size_t(1));
    }

    //Review M4: a lambda inside a static method referencing instance
    //state stays a resolution-phase compile error — the display
    //redirect must not swallow the existing diagnostic.
    void staticContextThisStillRejected()
    {
        auto out = compileOne(
            "class C {\n"
            "    int count;\n"
            "    public static func<int, int> make() {\n"
            "        return (x) -> x + count;\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    func<int, int> g = C.make();\n"
            "    return g(1);\n"
            "}\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out,
            "this is not available in a static method."), 1);
    }

    //A zero-capture lambda still takes the unified display path (D3):
    //a display class exists (empty field set, no $this) with its
    //synthetic method.
    void zeroCaptureTakesDisplayPath()
    {
        auto out = compileOne(
            "int main() {\n"
            "    func<int, int> f = (x) -> x + 1;\n"
            "    return f(1);\n"
            "}\n");
        QVERIFY(out.ok);
        QCOMPARE(allErrors(out), 0);
        auto* pLambda = findLambda(out.builder->TreeRootView());
        QVERIFY(pLambda != nullptr);
        QVERIFY(pLambda->DisplayClass() != nullptr);
        QVERIFY(pLambda->SyntheticMethod() != nullptr);
        const auto names = displayFieldNames(*pLambda);
        QCOMPARE(names.size(), size_t(0));
    }
};

#include "test_lambda_capture.moc"

QTEST_GUILESS_MAIN(TestLambdaCapture)
