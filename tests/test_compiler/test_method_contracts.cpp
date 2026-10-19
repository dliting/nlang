/*---
    test_method_contracts.cpp — 覆写签名/接口契约检查单元测试。
    载体形态同 test_statement_diag：MemLogger 收集诊断文本，
    断言「出现次数」（e2e 只做子串存在性断言，抓不住双报回归）。
    Task 1 首用例是别名指针同一性事实探针（spec §3.5）：别名形参与
    底层类型形参的 Type()->Field() 必须同一节点，否则签名恒等谓词
    需要别名展开（探针已确立指针同一性，签名恒等谓词无需别名展开）。
---*/
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/SnData.h>
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

//Struct-side parse probes (0.9.11 T2.5): the first struct declaration
//with the given name, and its single constructor.
const SnStructDecl* findStruct(const CompileOutcome& out,
    const std::string& name)
{
    std::function<const SnStructDecl*(const SnFunctionParentField&)> scan =
        [&](const SnFunctionParentField& parent) -> const SnStructDecl* {
        for (auto& member : parent.Members())
        {
            if (member.Kind() == NK_StructDecl && member.Name() == name)
                return static_cast<const SnStructDecl*>(&member);
            if (CanBeFuncParentEx(member.Kind()))
            {
                if (auto* hit = scan(static_cast<const SnFunctionParentField&>(member)))
                    return hit;
            }
        }
        return nullptr;
    };
    return scan(out.builder->TreeRootView());
}

const SnFunction* onlyCtor(const SnStructDecl* pStruct)
{
    if (!pStruct || pStruct->Ctors().size() != 1)
        return nullptr;
    for (auto& ctor : pStruct->Ctors())
        return &ctor;
    return nullptr;
}

//0.9.11 T3: statement count of a paragraph's child view (the view's
//wrapper iterator predates the iterator traits std::distance needs).
int stmtCount(SnStatementList& stmts)
{
    int count = 0;
    for (auto& stmt : stmts)
    {
        (void)stmt;
        ++count;
    }
    return count;
}

//0.9.11 T3: shape probe for an injected `this.<field> = <default>;`
//statement (an NK_AssignStmt marked NF_Generated whose left side is a
//member access on `this`). Returns the target field name, or an empty
//string when the statement does not match the injected shape.
std::string injectedAssignName(const SnStatement& stmt)
{
    if (stmt.Kind() != NK_AssignStmt || !stmt.ContainFlags(NF_Generated))
        return {};
    auto* pLeft = static_cast<const SnAssignStmt&>(stmt).Left();
    if (!pLeft || pLeft->Kind() != NK_MemberExpr)
        return {};
    auto& member = static_cast<const SnMemberExpr&>(*pLeft);
    if (!member.Outer() || member.Outer()->Kind() != NK_ThisExpr)
        return {};
    auto* pInner = member.Inner();
    if (!pInner || pInner->Kind() != NK_IdentifierExpr)
        return {};
    return static_cast<const SnIdentifierExpr*>(pInner)->Name();
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

    //=== class-side bijection (spec §2.1) ===

    //p5: return-type mismatch vs ancestor virtual (the original probe
    //shape: silent compile + bit-wrong runtime read).
    void TestOverrideReturnMismatchRejected() {
        auto out = compileOne(
            "class A {\n"
            "    virtual float f(int x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    int f(int x) { return 1; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "claims the virtual dispatch slot"), 1);
    }

    //p6: param-type mismatch.
    void TestOverrideParamTypeMismatchRejected() {
        auto out = compileOne(
            "class A {\n"
            "    virtual int f(int x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    int f(string s) { return 2; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "claims the virtual dispatch slot"), 1);
    }

    //p7: param-count mismatch.
    void TestOverrideParamCountMismatchRejected() {
        auto out = compileOne(
            "class A {\n"
            "    virtual int f(int x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    int f(int x, int y) { return 3; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "claims the virtual dispatch slot"), 1);
    }

    //m2: mid-chain mismatch errors at the mid class exactly once; the
    //grandchild's correct override is NOT flagged (error recovery:
    //per-name skip below a rejected name).
    void TestMidChainMismatchNoCascade() {
        auto out = compileOne(
            "class A {\n"
            "    virtual int f(int x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    int f(string s) { return 2; }\n"
            "}\n"
            "class C : B {\n"
            "    int f(int x) { return x + 10; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "claims the virtual dispatch slot"), 1);
    }

    //ov1: exact override PLUS an extension overload — the overload
    //hijacks name dispatch (order-dependent), the whole shape rejects.
    void TestOverridePlusOverloadRejected() {
        auto out = compileOne(
            "class A {\n"
            "    virtual int f(int x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    int f(int x) { return x; }\n"
            "    int f(string s) { return 5; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "claims the virtual dispatch slot"), 1);
    }

    //ov3: the same shape with the extension overload declared FIRST —
    //order must not change the verdict.
    void TestOverrideOverloadReversedOrderRejected() {
        auto out = compileOne(
            "class A {\n"
            "    virtual int f(int x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    int f(string s) { return 5; }\n"
            "    int f(int x) { return x; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "claims the virtual dispatch slot"), 1);
    }

    //po1: parent's same-name set holds virtual + non-virtual; only the
    //VIRTUAL signature is contract; the child's extra overload rejects.
    void TestParentMixedSameNameSet() {
        auto out = compileOne(
            "class A {\n"
            "    virtual int f(int x) { return x; }\n"
            "    int f(string s) { return 1; }\n"
            "}\n"
            "class B : A {\n"
            "    int f(int x) { return x; }\n"
            "    int f(string s, int n) { return 2; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "claims the virtual dispatch slot"), 1);
    }

    //s1c: a STATIC same-name method cannot match an instance contract.
    void TestStaticShadowRejected() {
        auto out = compileOne(
            "class A {\n"
            "    virtual int f(int x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    static int f(int x) { return x; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "claims the virtual dispatch slot"), 1);
    }

    //0.9.10 D2.4 refresh: the same-name static/virtual pair is legal
    //when the parameter lists differ (separate namespaces).
    void TestStaticDifferentParamsLegal() {
        auto out = compileOne(
            "class A {\n"
            "    virtual int f(int x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    static int f(string s) { return s.length(); }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(out.ok);
    }

    //0.9.10 D2.4: a static with the Object protocol's parameter list
    //claims the protocol dispatch slot (same rule as an inherited
    //virtual); a different parameter list stays contract-orthogonal.
    void TestStaticObjectProtocolRejected() {
        auto out = compileOne(
            "class A {\n"
            "    public static int equals(Object o) { return 1; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "claims the virtual dispatch slot"), 1);
    }

    //Legal: exact override. (Members called from main are declared
    //public: class members default to private, and external calls are
    //the point of the dispatch-shape check.)
    void TestExactOverrideLegal() {
        auto out = compileOne(
            "class A {\n"
            "    public virtual int f(int x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    public int f(int x) { return x + 1; }\n"
            "}\n"
            "int main() { A a = B(); return a.f(1); }\n");
        QVERIFY(out.ok);
    }

    //Legal (p8): same-class overload with NO contract on the name.
    void TestPlainOverloadNoContractLegal() {
        auto out = compileOne(
            "class C {\n"
            "    public int g(int x) { return x; }\n"
            "    public int g(string s) { return 7; }\n"
            "}\n"
            "int main() { C c; return c.g(\"x\"); }\n");
        QVERIFY(out.ok);
    }

    //Legal: hiding a NON-virtual ancestor method (index dispatch).
    void TestNonVirtualHideLegal() {
        auto out = compileOne(
            "class A {\n"
            "    public int h(int x) { return x; }\n"
            "}\n"
            "class B : A {\n"
            "    public int h(string s) { return 9; }\n"
            "}\n"
            "int main() { B b; return b.h(\"x\"); }\n");
        QVERIFY(out.ok);
    }

    //Legal (r3): parent declared AFTER the child.
    void TestParentDeclaredLaterLegal() {
        auto out = compileOne(
            "class B : A {\n"
            "    public int f(int x) { return x + 1; }\n"
            "}\n"
            "class A {\n"
            "    public virtual int f(int x) { return x; }\n"
            "}\n"
            "int main() { A a = B(); return a.f(1); }\n");
        QVERIFY(out.ok);
    }

    //=== interface-side completeness (spec §3.1 step 2) ===

    //r7: param mismatch, no call site — now a declaration-site error.
    //The class-side bijection fires first and its "claims the virtual
    //dispatch slot ... with a different signature" message carries the
    //asserted substring; the interface-side check is skipped for the
    //name via the rejected-names recovery.
    void TestIfaceParamMismatchRejected() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class D implements IF {\n"
            "    public int val(string s) { return 1; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "with a different signature"), 1);
    }

    //r8: return mismatch, no call site (message source: class side,
    //same as r7).
    void TestIfaceReturnMismatchRejected() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class D implements IF {\n"
            "    public float val(int x) { return 1.5; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "with a different signature"), 1);
    }

    //u13: missing entirely — evolved existing message, once (r9 fixed).
    void TestIfaceMissingReportedOnceWithSignature() {
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
            out, "does not implement the method \"val(Int32): Int32\""), 1);
    }

    //iv1: exact match exists but a first-declared sibling overload
    //hijacks name dispatch — rejected at the sibling.
    void TestIfaceSiblingOverloadHijackRejected() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class D implements IF {\n"
            "    public int val(string s) { return 1; }\n"
            "    public int val(int x) { return x; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out,
            "claims the virtual dispatch slot"), 1);
    }

    //u6/u8: same shape both declaration orders — order-independent
    //rejection (the extra overload is the error either way).
    void TestIfaceOverloadOrderIndependentA() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class D implements IF {\n"
            "    public int val() { return 7; }\n"
            "    public int val(int x) { return x; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out,
            "claims the virtual dispatch slot"), 1);
    }
    void TestIfaceOverloadOrderIndependentB() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class D implements IF {\n"
            "    public int val(int x) { return x; }\n"
            "    public int val() { return 7; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out,
            "claims the virtual dispatch slot"), 1);
    }

    //u10: two interfaces, same name, different signatures — one
    //dispatch winner cannot satisfy both.
    void TestTwoIfaceSameNameRejected() {
        auto out = compileOne(
            "interface IF1 {\n"
            "    public int val(int x);\n"
            "}\n"
            "interface IF2 {\n"
            "    public int val(string s);\n"
            "}\n"
            "class D implements IF1, IF2 {\n"
            "    public int val(int x) { return x; }\n"
            "    public int val(string s) { return 2; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "with a different signature"), 1);
    }

    //u7: inherited name found in Base (non-virtual) + own shadow —
    //the class-side bijection rejects D.val(string) against the
    //interface contract (the "with a different signature" tail of the
    //claims message is what the count asserts; the interface-side
    //variant is suppressed by the rejected-names recovery).
    void TestIfaceInheritedImplShadowRejected() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class Base {\n"
            "    public int val(int x) { return x; }\n"
            "}\n"
            "class D : Base implements IF {\n"
            "    public int val(string s) { return 1; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(
            out, "with a different signature"), 1);
    }

    //u1: NON-virtual implementing method is a valid implementation.
    void TestIfaceNonVirtualImplLegal() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class D implements IF {\n"
            "    public int val(int x) { return x; }\n"
            "}\n"
            "int main() { IF d = D(); return d.val(42); }\n");
        QVERIFY(out.ok);
    }

    //u2: parent-provided implementation satisfies the subclass.
    void TestIfaceParentImplLegal() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class Base implements IF {\n"
            "    public int val(int x) { return x; }\n"
            "}\n"
            "class D : Base {\n"
            "    int other() { return 1; }\n"
            "}\n"
            "int main() { IF d = D(); return d.val(42); }\n");
        QVERIFY(out.ok);
    }

    //u4: grandparent-provided implementation (B/D carry a dummy
    //member each — an empty class body is a parse error, p9d).
    void TestIfaceGrandparentImplLegal() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class A implements IF {\n"
            "    public int val(int x) { return x + 2; }\n"
            "}\n"
            "class B : A {\n"
            "    int dummyB() { return 0; }\n"
            "}\n"
            "class D : B {\n"
            "    int dummyD() { return 0; }\n"
            "}\n"
            "int main() { IF d = D(); return d.val(42); }\n");
        QVERIFY(out.ok);
    }

    //u3/iv3: private implementing method is valid (access checked
    //against the interface's public declaration, not the impl).
    void TestIfacePrivateImplLegal() {
        auto out = compileOne(
            "interface IF {\n"
            "    public int val(int x);\n"
            "}\n"
            "class D implements IF {\n"
            "    private int val(int x) { return x + 1; }\n"
            "}\n"
            "int main() { IF d = D(); return d.val(42); }\n");
        QVERIFY(out.ok);
    }

    //=== Object protocol table (spec §2.3) ===

    //eq2: equals with a wrong param type — today feeds a Point into a
    //string param at runtime.
    void TestEqualsWrongParamRejected() {
        auto out = compileOne(
            "class P {\n"
            "    int x;\n"
            "    int equals(string s) { return 1; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out,
            "claims the virtual dispatch slot of \"Object.equals"), 1);
    }

    void TestToStringOverloadRejected() {
        auto out = compileOne(
            "class P {\n"
            "    string toString(int base) { return \"x\"; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out,
            "claims the virtual dispatch slot of \"Object.toString"), 1);
    }

    //Review-driven: an out param is a signature mismatch for the
    //protocol's in-param slot (same identity predicate as the rest
    //of the check family).
    void TestEqualsOutParamRejected() {
        auto out = compileOne(
            "class P {\n"
            "    int x;\n"
            "    int equals(out Object o) { o = null; return 1; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out,
            "claims the virtual dispatch slot of \"Object.equals"), 1);
    }

    void TestGetHashCodeWrongReturnRejected() {
        auto out = compileOne(
            "class P {\n"
            "    string getHashCode() { return \"x\"; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out,
            "claims the virtual dispatch slot of \"Object.getHashCode"), 1);
    }

    //eq1: correct getHashCode override stays legal.
    void TestGetHashCodeOverrideLegal() {
        auto out = compileOne(
            "class P {\n"
            "    int x;\n"
            "    int getHashCode() { return 69; }\n"
            "}\n"
            "int main() { P p; return p.getHashCode() - 69; }\n");
        QVERIFY(out.ok);
    }

    //eq3: equals(Object) is spellable and dispatches.
    void TestEqualsObjectLegal() {
        auto out = compileOne(
            "class P {\n"
            "    int x;\n"
            "    int equals(Object other) { return 7; }\n"
            "}\n"
            "int main() { P p; return p.equals(p) - 7; }\n");
        QVERIFY(out.ok);
    }

    //toString() override — the shape the whole stdlib leans on.
    void TestToStringOverrideLegal() {
        auto out = compileOne(
            "class P {\n"
            "    string toString() { return \"P\"; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(out.ok);
    }

    //=== struct constructor spelling (0.9.11 T2.5) ===

    //sc1: default-access constructor parses; the AST member is an
    //NK_Function with no return type. The declaration contract and the
    //binding land in T2.6/T2.9 — at this stage the member is inert.
    //The bare spelling carries FA_Default into the node and
    //SnField::OnAddedToParent resolves it to the struct's private member
    //default — the same reading a bare class constructor gets.
    void TestStructCtorParsesDefaultAccess() {
        auto out = compileOne(
            "struct Point {\n"
            "    Point(int x) { }\n"
            "    int x;\n"
            "}\n"
            "int main() { return 0; }\n");
        QCOMPARE(countErrorsContaining(out, "syntax error"), 0);
        auto* pStruct = findStruct(out, "Point");
        QVERIFY(pStruct);
        QVERIFY(pStruct->Ctors().size() == 1);
        const SnFunction* pCtor = nullptr;
        for (auto& ctor : pStruct->Ctors())
            if (ctor.Name() == "Point")
                pCtor = &ctor;
        QVERIFY(pCtor);
        QVERIFY(pCtor->ReturnType() == nullptr);
        QVERIFY(pCtor->Params().size() == 1);
        QCOMPARE(int(pCtor->AccessType()), int(FA_Private));
    }

    //sc2: the access/modifier spellings parse (public, private, static,
    //public static). The contract that rejects struct constructors
    //carrying static/virtual arrives with T2.6.
    void TestStructCtorParsesAccessAndFlags() {
        auto out = compileOne(
            "struct A {\n"
            "    public A() { }\n"
            "}\n"
            "struct B {\n"
            "    private B() { }\n"
            "}\n"
            "struct C {\n"
            "    static C() { }\n"
            "}\n"
            "struct D {\n"
            "    public static D(int x) { }\n"
            "}\n"
            "int main() { return 0; }\n");
        QCOMPARE(countErrorsContaining(out, "syntax error"), 0);
        auto* pA = onlyCtor(findStruct(out, "A"));
        auto* pB = onlyCtor(findStruct(out, "B"));
        auto* pC = onlyCtor(findStruct(out, "C"));
        auto* pD = onlyCtor(findStruct(out, "D"));
        QVERIFY(pA && pB && pC && pD);
        QCOMPARE(int(pA->AccessType()), int(FA_Public));
        QCOMPARE(int(pB->AccessType()), int(FA_Private));
        QCOMPARE(int(pC->AccessType()), int(FA_Private));
        QVERIFY(pC->IsStatic());
        QCOMPARE(int(pD->AccessType()), int(FA_Public));
        QVERIFY(pD->IsStatic());
    }

    //sc3: a member name that is also a visible type name stays one-token
    //discriminated: `Point p;` is a field, `Box()` a constructor.
    void TestStructFieldAndCtorCoexist() {
        auto out = compileOne(
            "struct Point {\n"
            "    Point(int x) { }\n"
            "    int x;\n"
            "}\n"
            "struct Box {\n"
            "    Point p;\n"
            "    Box() { }\n"
            "}\n"
            "int main() { return 0; }\n");
        QCOMPARE(countErrorsContaining(out, "syntax error"), 0);
        auto* pBox = findStruct(out, "Box");
        QVERIFY(pBox);
        QVERIFY(pBox->FieldCount() == 1);
        QVERIFY(pBox->Ctors().size() == 1);
        QVERIFY(pBox->FindField("p") != nullptr);
    }

    //=== struct member contract (0.9.11 T2.6) ===

    //sc4: a function member whose name differs from the struct is not a
    //constructor — structs declare fields and constructors only.
    void TestStructRejectsPlainFunction() {
        auto out = compileOne(
            "struct S {\n"
            "    int x;\n"
            "    foo() { }\n"
            "}\n"
            "int main() { return 0; }\n");
        QCOMPARE(countErrorsContaining(out,
            "may only declare fields and constructors"), 1);
    }

    //sc5: a name-matching constructor must not carry static/virtual/const
    //(the same contract text as the class constructor flags).
    void TestStructCtorRejectsModifiers() {
        const char *spellings[] = {
            "    static S() { }\n",
            "    virtual S() { }\n",
            "    const S() { }\n",
        };
        for (auto spelling : spellings) {
            std::string src = std::string("struct S {\n") + spelling +
                "    int x;\n"
                "}\n"
                "int main() { return 0; }\n";
            auto out = compileOne(src.c_str());
            QCOMPARE(countErrorsContaining(out,
                "cannot be declared static, virtual, or const"), 1);
        }
    }

    //sc6: the legal spelling resolves end to end; the constructor is
    //inert until the binding lands (T2.9), so the program still compiles.
    void TestStructCtorLegalSpellingCompiles() {
        auto out = compileOne(
            "struct S {\n"
            "    public S(int v) { }\n"
            "    int x;\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(out.ok);
    }

    //sc7: `this` inside a struct constructor binds to the struct type
    //and the member assignment resolves (0.9.11 T2.7).
    void TestStructCtorThisBinds() {
        auto out = compileOne(
            "struct S {\n"
            "    public S(int v) { this.x = v; }\n"
            "    int x;\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(out.ok);
    }

    //sc8: the this-position diagnostic now names struct alongside class
    //and enum (0.9.11 T2.7).
    void TestThisOutsideMethodText() {
        auto out = compileOne(
            "int main() {\n"
            "    this.x = 1;\n"
            "    return 0;\n"
            "}\n");
        QCOMPARE(countErrorsContaining(out,
            "'this' can only be used inside a class, struct, or enum "
            "method."), 1);
    }

    //sc9: a value return in a struct constructor gets the constructor
    //wording, not the plain no-return-function wording (T2.8).
    void TestStructCtorRejectsReturnValue() {
        auto out = compileOne(
            "struct S {\n"
            "    public S() { return 5; }\n"
            "}\n"
            "int main() { return 0; }\n");
        QCOMPARE(countErrorsContaining(out,
            "constructor \"S\" cannot return a value"), 1);
    }

    //sc10/sc11: a struct constructor default that references this —
    //explicitly or through a bare field — is rejected at the declaration
    //like the class form (T2.8).
    void TestStructCtorDefaultRejectsThis() {
        auto out = compileOne(
            "struct S {\n"
            "    public S(int v = this.x) { }\n"
            "    int x;\n"
            "}\n"
            "int main() { return 0; }\n");
        QCOMPARE(countErrorsContaining(out,
            "cannot reference 'this'"), 1);
        auto out2 = compileOne(
            "struct S {\n"
            "    public S(int v = x) { }\n"
            "    int x;\n"
            "}\n"
            "int main() { return 0; }\n");
        QCOMPARE(countErrorsContaining(out2,
            "cannot reference 'this'"), 1);
    }

    //sc12: super(...) inside a struct constructor — structs have no
    //inheritance; the diagnostic names the class-only rule (T2.8).
    void TestStructCtorRejectsSuper() {
        auto out = compileOne(
            "struct S {\n"
            "    public S() { super(); }\n"
            "}\n"
            "int main() { return 0; }\n");
        QCOMPARE(countErrorsContaining(out,
            "'super' is only valid inside a class constructor"), 1);
    }

    //=== class field initializer injection (0.9.11 T3, D4/D9) ===

    //A class with field initializers and no user constructor gets a
    //synthesized public zero-arg constructor whose body is exactly the
    //injected `this.<field> = <default>;` statements, in declaration
    //order, each marked NF_Generated.
    void TestClassFieldInitSynthesizesCtor() {
        auto out = compileOne(
            "class Cfg {\n"
            "    public int x = 5;\n"
            "    public string s = \"hi\";\n"
            "    public bool b = true;\n"
            "    public float f = -1.5;\n"
            "}\n"
            "int main() { Cfg c = Cfg(); return c.x; }\n");
        QVERIFY(out.ok);
        auto* pCtor = findMethod(out, "Cfg", "Cfg");
        QVERIFY(pCtor);
        QVERIFY(pCtor->Params().size() == 0);
        QCOMPARE(pCtor->AccessType(), FA_Public);
        QVERIFY(!pCtor->HasReturn());
        QVERIFY(pCtor->ContainFlags(NF_Generated));
        QVERIFY(pCtor->Body() != nullptr);
        auto& stmts = pCtor->Body()->Statements();
        QVERIFY(stmtCount(stmts) == 4);
        const char* expectedNames[] = {"x", "s", "b", "f"};
        int index = 0;
        for (auto& stmt : stmts)
            QCOMPARE(injectedAssignName(stmt).c_str(),
                expectedNames[index++]);
    }

    //A user constructor receives the injected defaults at body start
    //(no super here); its own statements keep their position and the
    //constructor itself is not marked generated.
    void TestClassFieldInitInjectsUserCtor() {
        auto out = compileOne(
            "class Cfg {\n"
            "    public int x = 3;\n"
            "    Cfg(int y) { this.x = this.x + y; }\n"
            "}\n"
            "int main() { Cfg c = Cfg(4); return c.x; }\n");
        QVERIFY(out.ok);
        auto* pCtor = findMethod(out, "Cfg", "Cfg");
        QVERIFY(pCtor);
        QVERIFY(pCtor->Params().size() == 1);
        QVERIFY(!pCtor->ContainFlags(NF_Generated));
        auto& stmts = pCtor->Body()->Statements();
        QVERIFY(stmtCount(stmts) == 2);
        auto it = stmts.begin();
        QCOMPARE(injectedAssignName(*it).c_str(), "x");
        ++it;
        QVERIFY(it->Kind() == NK_AssignStmt);
        QVERIFY(!it->ContainFlags(NF_Generated));
    }

    //Base-first (D9): with super() first the injection lands right after
    //the super call; the base's own synthesized constructor applies the
    //base default during super().
    void TestClassFieldInitInjectsAfterSuper() {
        auto out = compileOne(
            "class Base {\n"
            "    public int a = 1;\n"
            "}\n"
            "class Derived : Base {\n"
            "    public int b = 2;\n"
            "    Derived() { super(); }\n"
            "}\n"
            "int main() { Derived d = Derived(); return d.b; }\n");
        QVERIFY(out.ok);
        auto* pBaseCtor = findMethod(out, "Base", "Base");
        QVERIFY(pBaseCtor);
        QVERIFY(pBaseCtor->ContainFlags(NF_Generated));
        auto& baseStmts = pBaseCtor->Body()->Statements();
        QVERIFY(stmtCount(baseStmts) == 1);
        QCOMPARE(injectedAssignName(*baseStmts.begin()).c_str(), "a");
        auto* pCtor = findMethod(out, "Derived", "Derived");
        QVERIFY(pCtor);
        auto& stmts = pCtor->Body()->Statements();
        QVERIFY(stmtCount(stmts) == 2);
        auto it = stmts.begin();
        QVERIFY(it->Kind() == NK_SuperCallStmt);
        ++it;
        QCOMPARE(injectedAssignName(*it).c_str(), "b");
    }

    //super() not first + initializers: the injection point is undefined,
    //so exactly one diagnostic (the e2e manifest only pins substring
    //presence; the count is this file's job) and no half-injected AST.
    void TestClassFieldInitSuperMidSingleError() {
        auto out = compileOne(
            "class Base {\n"
            "    public int a;\n"
            "    Base() { this.a = 1; }\n"
            "}\n"
            "class Derived : Base {\n"
            "    public int b = 2;\n"
            "    Derived() {\n"
            "        int t = 1;\n"
            "        super();\n"
            "    }\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out,
            "field initializers require super() to be the first"), 1);
    }

    //A native constructor has no body to inject into: exactly one
    //diagnostic naming the native/bodyless rule.
    void TestClassFieldInitNativeCtorSingleError() {
        auto out = compileOne(
            "class Cfg {\n"
            "    public int x = 5;\n"
            "    public native Cfg();\n"
            "}\n"
            "int main() { return 0; }\n");
        QVERIFY(!out.ok);
        QCOMPARE(countErrorsContaining(out,
            "cannot be applied to native or bodyless constructors"), 1);
    }
};

QTEST_GUILESS_MAIN(TestMethodContracts)
#include "test_method_contracts.moc"
