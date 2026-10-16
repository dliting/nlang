/*---
    test_method_contracts.cpp — 覆写签名/接口契约检查单元测试。
    载体形态同 test_statement_diag：MemLogger 收集诊断文本，
    断言「出现次数」（e2e 只做子串存在性断言，抓不住双报回归）。
    Task 1 首用例是别名指针同一性事实探针（spec §3.5）：别名形参与
    底层类型形参的 Type()->Field() 必须同一节点，否则签名恒等谓词
    需要别名展开（事实带回计划修订，实现暂停）。
---*/
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnMisc.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace nlang;

namespace {

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

CompileOutcome compileOne(const char* szBody)
{
    static uint32_t runCount = 0;
    const auto dir = std::filesystem::temp_directory_path()
        / "nlang_method_contracts_tests";
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
        "method_contracts_" + std::to_string(++runCount);
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
    if (!out.logger)
        return 0;
    int hits = 0;
    for (const auto& text : out.logger->errorsText())
        if (text.find(szNeedle) != std::string::npos)
            ++hits;
    return hits;
}

//Depth-first search for the first class member function with the given
//name (root namespace + function-parent nesting, mirroring the check's
//CollectClasses domain). Const chain throughout: ModuleBuilder exposes
//the tree read-only via TreeRootView() (TreeRoot() is private).
const SnFunction* findMethod(const CompileOutcome& out,
    const std::string& cls, const std::string& method)
{
    std::function<const SnFunction*(const SnFunctionParentField&)> scan =
        [&](const SnFunctionParentField& parent) -> const SnFunction* {
        for (auto& member : parent.Members())
        {
            if (member.Kind() == NK_ClassDecl)
            {
                if (member.Name() == cls)
                {
                    for (auto& m : static_cast<const SnClassDecl&>(member).Members())
                        if (m.Kind() == NK_Function && m.Name() == method)
                            return static_cast<const SnFunction*>(&m);
                }
            }
            else if (CanBeFuncParentEx(member.Kind()))
            {
                if (auto* hit = scan(static_cast<const SnFunctionParentField&>(member)))
                    return hit;
            }
        }
        return nullptr;
    };
    return scan(out.builder->TreeRootView());
}

} //namespace

class TestMethodContracts : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();
    }

    //Spec §3.5 first pin: an alias-written formal and the underlying
    //type formal must resolve to the SAME type node, or the signature
    //identity predicate needs alias expansion.
    void TestAliasFormalSameTypeNode() {
        auto out = compileOne(
            "using MyInt = int;\n"
            "class A {\n"
            "    virtual int f(MyInt x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    int f(int x) { return x; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(out.ok);
        auto* pAlias = findMethod(out, "A", "f");
        auto* pPlain = findMethod(out, "B", "f");
        QVERIFY(pAlias && pPlain);
        QVERIFY(pAlias->Params().size() == 1);
        QVERIFY(pPlain->Params().size() == 1);
        const SnFieldExpr* pAliasType = nullptr;
        const SnFieldExpr* pPlainType = nullptr;
        for (auto& param : pAlias->Params()) pAliasType = param.Type();
        for (auto& param : pPlain->Params()) pPlainType = param.Type();
        QVERIFY(pAliasType && pPlainType);
        QCOMPARE(pAliasType->Field(), pPlainType->Field());
    }

    //r9: ResolveClassBases runs twice per class (pre-pass + document
    //order); AddImplements must not append the same interface twice or
    //every interface diagnostic is emitted in duplicate.
    void TestInterfaceDiagReportedOnce() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class F implements IF {\n"
            "    int other() { return 1; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "does not implement the method"), 1);
    }
};

QTEST_GUILESS_MAIN(TestMethodContracts)
#include "test_method_contracts.moc"
