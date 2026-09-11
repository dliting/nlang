/*---
test_array_property.cpp - array-valued type property unit tests.

In-process ModuleBuilder coverage of the array redesign B property:
IsArrayValued() is stamped at resolver binding tails from the declared
or bound type, so each expression SHAPE below (identifier, member
field, new-array, array-returning call, container element read,
container subscript, plain array subscript) carries the property that
the old shape-inference predicate computed post hoc.
---*/
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SyntaxNode.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace nlang;

namespace {

//Collects error diagnostics in memory (same shape as test_module_import).
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
//inputs (same ownership note as test_module_import), so the outcome
//struct owns all three for as long as the caller touches the tree.
struct CompileOutcome
{
    std::unique_ptr<BuildParams> params;
    std::unique_ptr<MemLogger> logger;
    std::unique_ptr<ModuleBuilder> builder;
    bool ok = false;
};

//Compile one source; ok=false means infrastructure or resolution
//failure (callers QVERIFY the outcome before touching the tree).
//ModuleManager is process-global and refuses a second Create of the
//same module name (see test_module_import), so the output module name
//is uniquified per call.
CompileOutcome compileOne(const char* szBody)
{
    static uint32_t runCount = 0;
    const auto dir = std::filesystem::temp_directory_path()
        / "nlang_array_property_tests";
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
        "array_property_" + std::to_string(++runCount);
    out.params->m_sOutputDir = dir.string();
    out.params->m_sTempDir = dir.string();
    out.logger = std::make_unique<MemLogger>();
    out.builder = std::make_unique<ModuleBuilder>(*out.params, *out.logger);
    try { out.ok = out.builder->Build(); }
    catch (const std::exception&) { out.ok = false; }
    return out;
}

//Collect resolved expressions of a node kind, optionally filtered by
//ToString() (szName == nullptr matches every node of the kind). Whole-
//tree walk via Children()'s const iterators (containment invariant:
//every child is reachable through Children()); pre-order, so document
//order of the source is preserved in the result.
std::vector<SnExpression*> collectByKind(const SnNamespace& root,
    NodeKind kind, const char* szName)
{
    std::vector<SnExpression*> found;
    std::function<void(const SyntaxNode&)> walk =
        [&](const SyntaxNode& node) {
            if (node.Kind() == kind
                && (szName == nullptr
                    || node.ToString() == szName))
                found.push_back(const_cast<SnExpression*>(
                    static_cast<const SnExpression*>(&node)));
            const auto& children = node.Children();
            for (auto it = children.cbegin(); it != children.cend(); ++it)
            {
                //Children() yields const Node&; every node below the
                //tree root is a SyntaxNode in practice (same assumption
                //as ModuleBuilder's tree sweep).
                walk(static_cast<const SyntaxNode&>(*it));
            }
        };
    walk(root);
    return found;
}

} //namespace

class TestArrayProperty : public QObject
{
    Q_OBJECT
private slots:
    //Runtime tables (IdString etc.) must exist before any Build().
    void initTestCase() { Runtime::StaticInit(); }

    void identifierAndMemberShapes();
    void valueShapes();
};

void TestArrayProperty::identifierAndMemberShapes()
{
    //No member `.length` here: its receiver widening is Task 3 scope —
    //`s.f.length` does not compile until then.
    auto out = compileOne(
        "struct S { int[] f; }\n"
        "int main() {\n"
        "    int[] a = new int[2];\n"
        "    S s;\n"
        "    int[] t = s.f;\n"
        "    int n = a.length;\n"
        "    return n;\n"
        "}\n");
    QVERIFY(out.ok);
    const auto& root = out.builder->TreeRootView();
    //lvalue shapes: array-valued
    auto ids = collectByKind(root, NK_IdentifierExpr, "a");
    QVERIFY(!ids.empty());
    QVERIFY(ids[0]->IsArrayValued());
    //non-array identifier: property false
    auto ns = collectByKind(root, NK_IdentifierExpr, "n");
    QVERIFY(!ns.empty());
    QVERIFY(!ns[0]->IsArrayValued());
    //member field lvalue: array-valued (s.f)
    auto members = collectByKind(root, NK_MemberExpr, "s.f");
    QVERIFY(!members.empty());
    QVERIFY(members[0]->IsArrayValued());
}

void TestArrayProperty::valueShapes()
{
    auto out = compileOne(
        "int[] mk() { return new int[3]; }\n"
        "int main() {\n"
        "    int[] a = new int[3];\n"
        "    int x = a[0] + mk()[0];\n"
        "    List<int[]> li = new List<int[]>();\n"
        "    li.add(new int[1]);\n"
        "    int[] e = li.get(0);\n"
        "    int[] e2 = li[0];\n"
        "    return e.length + e2.length + x;\n"
        "}\n");
    QVERIFY(out.ok);
    const auto& root = out.builder->TreeRootView();
    //new-array expressions are always array-valued. ToString() is the
    //constant "new_array", so the nodes are located by kind here (the
    //source has three: mk's body, main's decl, li.add's argument).
    auto news = collectByKind(root, NK_NewArrayExpr, nullptr);
    QVERIFY(!news.empty());
    for (auto* pNew : news)
        QVERIFY(pNew->IsArrayValued());
    //subscript of a plain array: NOT array-valued (element type).
    //a[0] comes first in pre-order (the x assignment precedes li[0]).
    auto subs = collectByKind(root, NK_SubscriptExpr, nullptr);
    QVERIFY(subs.size() >= 3);   //a[0], mk()[0], li[0]
    //plain-array subscript yields int: false
    QVERIFY(!subs[0]->IsArrayValued());
    //container sugar li[0] is array-valued: pre-order puts it third
    //(ToString() is the constant "subscript", so position locates it —
    //the source has exactly these three subscripts).
    QVERIFY(subs[2]->IsArrayValued());
    //array-returning call mk() is array-valued (the source's only bare
    //array-valued invoke; li.add rides the member shape, not this arm).
    auto mks = collectByKind(root, NK_InvokeExpr, "mk()");
    QVERIFY(!mks.empty());
    QVERIFY(mks[0]->IsArrayValued());
    //container element read l.get(0) on List<int[]>: array-valued
    auto gets = collectByKind(root, NK_MemberExpr, "li.get(0)");
    QVERIFY(!gets.empty());
    QVERIFY(gets[0]->IsArrayValued());
}

QTEST_GUILESS_MAIN(TestArrayProperty)
#include "test_array_property.moc"
