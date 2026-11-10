/*---
    test_statement_diag.cpp — 语句级诊断行为单元测试。
    钉「局部声明初始化错误只报一次」：local-decl 被分解为 AssignStmt 并在
    遍历中途插入段落子列表、立即解析（StatementResolverDecls.cpp），段落
    遍历随后重访该语句——Access(SnAssignStmt) 的每个出口都标记
    NF_Resolved，未标记的错误出口会让重访再次解析并双报诊断。
    每个用例断言诊断的**出现次数**（而非仅存在性）：e2e 只做子串存在性
    断言，抓不住双报回归。
---*/
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

//Collects error diagnostics in memory (same shape as test_array_property).
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
//inputs (ownership note per test_array_property), so the outcome struct
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
    const auto dir = std::filesystem::temp_directory_path()
        / "nlang_statement_diag_tests";
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
        "statement_diag_" + std::to_string(++runCount);
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

} //namespace

class TestStatementDiag : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();   //cast table (0.7.5: no longer inside Runtime::StaticInit)
    }

    //An undefined callee in a local-decl initializer: pre-fix the
    //decomposed AssignStmt was revisited unflagged and "boom" was
    //reported twice.
    void TestLocalInitDiagnosticReportedOnce() {
        auto out = compileOne(
            "int main() {\n"
            "    int x = boom();\n"
            "    return 0;\n"
            "}\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out, "\"boom\""), 1);
    }

    //func construction-by-name reject (the func batch's visible face of
    //the same double report): the reject fires in Access(SnNewExpr) on
    //the initializer's right side.
    void TestFuncCtorRejectReportedOnce() {
        auto out = compileOne(
            "int main() {\n"
            "    func<int, int> f = func<int, int>();\n"
            "    return f(1);\n"
            "}\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "func types cannot be constructed by name"), 1);
    }

    //Init-list element mismatch: FinishInitListAssign flags the
    //statement resolved on error exits too.
    void TestInitListDiagnosticReportedOnce() {
        auto out = compileOne(
            "int main() {\n"
            "    int[] a = [1, \"x\"];\n"
            "    return 0;\n"
            "}\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out, "Incompatible type"), 1);
    }

    //0.13.1a Task 3: the spelling probe must stay neutral on comparison
    //shapes — a mis-armed '<' would arrive as OT_TypeLt and die as a
    //syntax error. Each shape compiles clean only under the comparison
    //reading (invariant pins: green before and after the probe landed;
    //the spelling shapes' own failure was watched through the e2e).
    void TestReceiverSpellingParseShapes() {
        //Comparison chain: `<b>` ends before 'c', not '.' or '(' — the
        //comparison reading holds, so the chain parses and its semantic
        //(bool > int) is what gets rejected; a mis-armed probe would
        //surface a "syntax error" instead.
        auto chain = compileOne(
            "int main() {\n"
            "    int a = 1;\n"
            "    int b = 2;\n"
            "    int c = 3;\n"
            "    if (a < b > c) {\n"
            "        return 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n");
        QVERIFY(!chain.ok);
        QCOMPARE(countErrorsContaining(chain, "syntax error"), 0);
        QCOMPARE(countErrorsContaining(chain, "cannot be ordered"), 1);

        //Comparison inside call args: `<b, c>` ends before 'd' — both
        //comparisons reach the bool formals.
        auto callArg = compileOne(
            "bool f(bool p, bool q) {\n"
            "    return p || q;\n"
            "}\n"
            "int main() {\n"
            "    int a = 1;\n"
            "    int b = 2;\n"
            "    int c = 3;\n"
            "    int d = 4;\n"
            "    if (f(a < b, c > d)) {\n"
            "        return 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n");
        QVERIFY(callArg.ok);
        QVERIFY(callArg.logger->errorsText().empty());

        //Comparison in assignment: `<b>` ends before ';'.
        auto assign = compileOne(
            "int main() {\n"
            "    int a = 1;\n"
            "    int b = 2;\n"
            "    bool x = false;\n"
            "    x = a < b;\n"
            "    if (x) {\n"
            "        return 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n");
        QVERIFY(assign.ok);
        QVERIFY(assign.logger->errorsText().empty());

        //The spelling shape inside call args — the probe arms ('('
        //terminator) and the constructor arm parses it; the formal takes
        //the matching instantiation so the call type-checks.
        auto spellingArg = compileOne(
            "class Hold<U> {\n"
            "    public U held;\n"
            "    public Hold(U v) { held = v; }\n"
            "}\n"
            "int g(Hold<int> v) {\n"
            "    return v.held;\n"
            "}\n"
            "int main() {\n"
            "    return g(Hold<int>(5));\n"
            "}\n");
        QVERIFY(spellingArg.ok);
        QVERIFY(spellingArg.logger->errorsText().empty());

        //The spelling shape on a static member call — full dispatch to
        //the synthetic instantiation's method table.
        auto spellingMember = compileOne(
            "class Pair<K, V> {\n"
            "    public static string sig() {\n"
            "        return typeName(K) + \",\" + typeName(V);\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    if (Pair<int, string>.sig() != \"int,string\") {\n"
            "        return 3;\n"
            "    }\n"
            "    return 0;\n"
            "}\n");
        QVERIFY(spellingMember.ok);
        QVERIFY(spellingMember.logger->errorsText().empty());
    }
};

QTEST_GUILESS_MAIN(TestStatementDiag)
#include "test_statement_diag.moc"
