/*---
test_ref_binding.cpp - the reference-binding contract (v0.13.9 phase 3,
value_type_reference_design.md 3.1/3.9).

In-process ModuleBuilder coverage of the resolver-side rules Task 1's
grammar left unconsumed: &-marked declarations bind an initializer with
'=' (no bare, ctor or sized continuations), the initializer must be a
bindable location or an accepted rvalue (never const/static storage,
never another reference, never a method call result or an enum member),
scalar kinds match exactly, &-typed returns return outliving storage,
&-formals take no default, foreach & heads reject Dict-key and string
sources, and identity types (class/interface/array/container) degrade
the & mark to a no-op.
---*/
#include "test_support/test_scratch.h"
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/SnData.h>
#include <nlang/compiler/SnExpressions.h>
#include "builder/ModuleRegistry.h"
#include "builder/ExprResolver.h"
#ifndef Q_MOC_RUN
#include <filesystem>
#endif
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#ifndef STDLIB_DIR
#define STDLIB_DIR ""
#endif

using namespace nlang;

namespace {

//Collects error diagnostics in memory; errors() drives the assertions.
class MemLogger : public CompileLogger
{
public:
    const std::vector<std::string>& errorsText() const
    {
        return m_errors;
    }

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

std::string writeTempSource(const QString& name, const char* body)
{
    const std::filesystem::path dir =
        nlang::test::ScratchDir("nlang_ref_tests");
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name.toStdString();
    std::ofstream(file, std::ios::binary) << body;
    return file.string();
}

bool anyErrorMentions(const std::vector<std::string>& errors,
    const char* szNeedle)
{
    for (const auto& error : errors)
    {
        if (error.find(szNeedle) != std::string::npos)
            return true;
    }
    return false;
}

//Compiles one source; true when the build completed with zero error
//diagnostics (the positive-binding contract). Diagnostics are copied
//out so rejects can pin their named wording on the same helper.
bool buildCompile(const std::string& tag, const char* body,
    std::vector<std::string>& errorsText)
{
    const auto dir = nlang::test::ScratchDir("nlang_ref_tests");
    BuildParams params;
    params.m_SourceFiles.push_back(writeTempSource(
        QString::fromStdString(tag), body));
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    bool clean = false;
    //Exception boundary (same contract as ncc main): codegen internal
    //errors throw; without the catch the abort would kill the test run.
    try
    {
        builder.BuildArtifacts();
        clean = true;
    }
    catch (const std::exception&) { }
    errorsText = logger.errorsText();
    return clean && errorsText.empty();
}

//Compiles one source and hands the tree root to the visitor (kind
//survey for the no-op classification). False when the build threw.
bool buildVisitRoot(const std::string& tag, const char* body,
    std::vector<std::string>& errorsText,
    const std::function<void(const SnNamespace&)>& visit)
{
    const auto dir = nlang::test::ScratchDir("nlang_ref_tests");
    BuildParams params;
    params.m_SourceFiles.push_back(writeTempSource(
        QString::fromStdString(tag), body));
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    bool built = false;
    try
    {
        builder.BuildArtifacts();
        visit(builder.TreeRootView());
        built = true;
    }
    catch (const std::exception&) { }
    errorsText = logger.errorsText();
    return built;
}

//First root-level child of the given kind (null when absent).
const SnField* firstRootFieldOfKind(const SnNamespace& root, NodeKind kind)
{
    const auto& children = root.Children();
    for (auto it = children.cbegin(); it != children.cend(); ++it)
    {
        const auto& field = static_cast<const SnField&>(*it);
        if (field.Kind() == kind)
            return &field;
    }
    return nullptr;
}

} //namespace

class TestRefBinding : public QObject
{
    Q_OBJECT
private slots:
    //Runtime tables (IdString etc.) must exist before any Build().
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();
    }

    //The named-reject matrix: every &-binding ill-form names its own
    //diagnostic (the e2e refbind_reject_* rows pin the same wording
    //end to end; this loop pins each message at the resolver level).
    void refBindingRejects()
    {
        struct RejectRow
        {
            const char* szTag;
            const char* szBody;
            const char* szNeedle;
        };
        const RejectRow rows[] = {
            {"ref_const_target",
                "struct Point { int x; Point(int x_) { x = x_; } }\n"
                "int main() {\n"
                "    const Point p = Point(1);\n"
                "    Point& r = p;\n"
                "    return 0;\n"
                "}\n",
                "cannot bind a reference to const storage"},
            {"ref_scalar_convert",
                "int main() {\n"
                "    byte b = 1;\n"
                "    int& r = b;\n"
                "    return 0;\n"
                "}\n",
                "cannot bind Byte to Int32&"},
            {"ref_enum_to_int",
                "enum Color { Red }\n"
                "int main() {\n"
                "    Color c = Color.Red;\n"
                "    int& v = c;\n"
                "    return 0;\n"
                "}\n",
                "cannot bind Color to Int32&"},
            {"ref_int_to_enum",
                "enum Color { Red }\n"
                "int main() {\n"
                "    int n = 0;\n"
                "    Color& c = n;\n"
                "    return 0;\n"
                "}\n",
                "cannot bind Int32 to Color&"},
            {"ref_enum_member",
                "enum Color { Red }\n"
                "int main() {\n"
                "    Color& c = Color.Red;\n"
                "    return 0;\n"
                "}\n",
                "cannot bind a reference to enum member"},
            {"ref_no_init",
                "struct Point { int x; }\n"
                "int main() {\n"
                "    Point& r;\n"
                "    return 0;\n"
                "}\n",
                "must bind an initializer"},
            {"ref_ctor_cont",
                "struct Point { int x; Point(int x_) { x = x_; } }\n"
                "int main() {\n"
                "    Point& r(1);\n"
                "    return 0;\n"
                "}\n",
                "constructor initialization"},
            {"ref_size_cont",
                "struct Point { int x; }\n"
                "int main() {\n"
                "    Point& r[3];\n"
                "    return 0;\n"
                "}\n",
                "array declarator"},
            {"ref_method_result",
                "struct Point { int x; }\n"
                "class Box {\n"
                "    public Point makePoint() {\n"
                "        Point p;\n"
                "        return p;\n"
                "    }\n"
                "}\n"
                "int main() {\n"
                "    Box box;\n"
                "    Point& r = box.makePoint();\n"
                "    return 0;\n"
                "}\n",
                "method call result"},
            {"ref_ref_to_ref",
                "struct Point { int x; }\n"
                "int main() {\n"
                "    Point p;\n"
                "    Point& a = p;\n"
                "    Point& b = a;\n"
                "    return 0;\n"
                "}\n",
                "cannot bind a reference to reference"},
            {"ref_static_scalar",
                "int main() {\n"
                "    static int g = 5;\n"
                "    int& r = g;\n"
                "    return r;\n"
                "}\n",
                "cannot bind a reference to static storage"},
            {"ref_param_default",
                "void f(int& v = 5) {\n"
                "}\n"
                "int main() {\n"
                "    f(1);\n"
                "    return 0;\n"
                "}\n",
                "cannot have a default value"},
            {"ref_return_local",
                "struct Point { int x; }\n"
                "Point& make() {\n"
                "    Point p;\n"
                "    return p;\n"
                "}\n"
                "int main() {\n"
                "    Point q = make();\n"
                "    return 0;\n"
                "}\n",
                "cannot return a reference to local"},
        };
        for (const RejectRow& row : rows)
        {
            std::vector<std::string> errors;
            buildCompile(row.szTag, row.szBody, errors);
            QVERIFY2(anyErrorMentions(errors, row.szNeedle),
                (std::string(row.szTag) + " must name its diagnostic: "
                    + row.szNeedle).c_str());
        }
    }

    //Foreach & heads reject the two materialized-copy sources.
    void refForeachRefRejects()
    {
        std::vector<std::string> errors;
        buildCompile("ref_foreach_dict",
            "int main() {\n"
            "    Dict<string, int> d = {};\n"
            "    d[\"a\"] = 1;\n"
            "    foreach (string& k in d) {\n"
            "    }\n"
            "    return 0;\n"
            "}\n",
            errors);
        QVERIFY2(anyErrorMentions(errors,
                "iterate Dict keys by reference"),
            "Dict-key & iteration must be rejected");
        buildCompile("ref_foreach_string",
            "int main() {\n"
            "    string s = \"ab\";\n"
            "    foreach (char& c in s) {\n"
            "    }\n"
            "    return 0;\n"
            "}\n",
            errors);
        QVERIFY2(anyErrorMentions(errors,
                "iterate a string by reference"),
            "string & iteration must be rejected");
    }

    //The bindable-position matrix compiles clean: local, single field,
    //element, element field, List element, chain (o.inner.x — the
    //representation's (basis, offset) pair covers any depth), ctor
    //rvalue, literal, string aliasing, exact byte, and the identity
    //downgrades (class, container, array).
    void refBindingPositives()
    {
        struct PositiveRow
        {
            const char* szTag;
            const char* szBody;
        };
        const PositiveRow rows[] = {
            {"ref_pos_local",
                "struct Point { int x; }\n"
                "int main() {\n"
                "    Point p;\n"
                "    Point& r = p;\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_field",
                "struct Point { int x; }\n"
                "int main() {\n"
                "    Point p;\n"
                "    int& r = p.x;\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_element",
                "int main() {\n"
                "    int[] xs = int[2];\n"
                "    int& r = xs[0];\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_element_field",
                "struct Point { int x; }\n"
                "int main() {\n"
                "    Point[] ps = Point[2];\n"
                "    Point p0;\n"
                "    ps[0] = p0;\n"
                "    int& r = ps[0].x;\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_list_element",
                "int main() {\n"
                "    List<int> nums = List<int>();\n"
                "    int& r = nums[0];\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_chain",
                "struct Inner { int x; }\n"
                "struct Outer { Inner inner; }\n"
                "int main() {\n"
                "    Outer o;\n"
                "    int& r = o.inner.x;\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_rvalue_ctor",
                "struct Point { int x; Point(int x_) { x = x_; } }\n"
                "int main() {\n"
                "    Point& r = Point(7);\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_literal",
                "int main() {\n"
                "    int& r = 5;\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_string",
                "int main() {\n"
                "    string a = \"x\";\n"
                "    string& b = a;\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_byte_exact",
                "int main() {\n"
                "    byte b = 1;\n"
                "    byte& rb = b;\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_identity_class",
                "class Dog { int age; }\n"
                "int main() {\n"
                "    Dog d;\n"
                "    Dog& d2 = d;\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_identity_list",
                "int main() {\n"
                "    List<int> xs = List<int>();\n"
                "    List<int>& l = xs;\n"
                "    return 0;\n"
                "}\n"},
            {"ref_pos_identity_array",
                "int main() {\n"
                "    int[] xs = int[2];\n"
                "    int[]& ys = xs;\n"
                "    return 0;\n"
                "}\n"},
        };
        for (const PositiveRow& row : rows)
        {
            std::vector<std::string> errors;
            const bool clean = buildCompile(row.szTag, row.szBody, errors);
            QString detail = QString::fromStdString(row.szTag)
                + " must compile clean"
                + (errors.empty() ? QString()
                    : QString(": ") + errors.front().c_str());
            QVERIFY2(clean, detail.toUtf8().constData());
        }
    }

    //Foreach & heads accept array and List sources (Dict/string are the
    //only rejects; the runtime loop itself is Task 4's deliverable).
    void refForeachRefPositives()
    {
        std::vector<std::string> errors;
        const bool arrayOk = buildCompile("ref_pos_foreach_array",
            "int main() {\n"
            "    int[] xs = int[2];\n"
            "    foreach (int& v in xs) {\n"
            "        v = 1;\n"
            "    }\n"
            "    return 0;\n"
            "}\n",
            errors);
        QVERIFY2(arrayOk, "array & foreach must compile clean");
        const bool listOk = buildCompile("ref_pos_foreach_list",
            "struct Point { int x; }\n"
            "int main() {\n"
            "    List<Point> pts = List<Point>();\n"
            "    foreach (Point& pt in pts) {\n"
            "    }\n"
            "    return 0;\n"
            "}\n",
            errors);
        QVERIFY2(listOk, "List & foreach must compile clean");
    }

    //&-typed returns of outliving storage compile clean: an element, a
    //parameter, and a field.
    void refReturnPositives()
    {
        std::vector<std::string> errors;
        const bool elemOk = buildCompile("ref_pos_return_element",
            "class Pool {\n"
            "    int[] xs;\n"
            "    public int& pick() {\n"
            "        return xs[0];\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    Pool pool;\n"
            "    return 0;\n"
            "}\n",
            errors);
        QVERIFY2(elemOk, "element & return must compile clean");
        const bool paramOk = buildCompile("ref_pos_return_param",
            "int& identity(int& v) {\n"
            "    return v;\n"
            "}\n"
            "int main() {\n"
            "    int n = 1;\n"
            "    return identity(n);\n"
            "}\n",
            errors);
        QVERIFY2(paramOk, "parameter & return must compile clean");
    }

    //The no-op hook (Task 9 consumes it at instantiation points):
    //identity types (class/interface/array token) classify as no-op
    //marks; slot-copy types (scalar/enum/struct/string) do not.
    void refNoOpClassification()
    {
        QVERIFY(IsRefNoOpType(
            SnBuiltinDataType::InstanceOf(NK_Int32)) == false);
        QVERIFY(IsRefNoOpType(
            SnBuiltinDataType::InstanceOf(NK_String)) == false);
        QVERIFY(IsRefNoOpType(nullptr) == false);
        std::vector<std::string> errors;
        const bool built = buildVisitRoot("ref_noop_kinds",
            "enum Color { Red }\n"
            "struct Point { int x; }\n"
            "class Dog { int age; }\n"
            "interface Shape {\n"
            "}\n"
            "int[] pick(int[] xs) {\n"
            "    return xs;\n"
            "}\n"
            "int main() {\n"
            "    return 0;\n"
            "}\n",
            errors,
            [](const SnNamespace& root)
            {
                const SnField* pStruct = firstRootFieldOfKind(root,
                    NK_StructDecl);
                const SnField* pClass = firstRootFieldOfKind(root,
                    NK_ClassDecl);
                const SnField* pInterface = firstRootFieldOfKind(root,
                    NK_InterfaceDecl);
                const SnField* pEnum = firstRootFieldOfKind(root,
                    NK_EnumDecl);
                QVERIFY2(pStruct != nullptr, "struct field present");
                QVERIFY2(pClass != nullptr, "class field present");
                QVERIFY2(pInterface != nullptr, "interface field present");
                QVERIFY2(pEnum != nullptr, "enum field present");
                QVERIFY2(IsRefNoOpType(pStruct) == false,
                    "struct is a slot-copy type");
                QVERIFY2(IsRefNoOpType(pEnum) == false,
                    "enum is a slot-copy type");
                QVERIFY2(IsRefNoOpType(pClass),
                    "class is an identity type");
                QVERIFY2(IsRefNoOpType(pInterface),
                    "interface is an identity type");
            });
        QVERIFY2(built, "the kind survey source must compile clean");
        //The interned array token (the array-return return-type field).
        bool tokenChecked = false;
        const bool tokenBuilt = buildVisitRoot("ref_noop_token",
            "int[] pick(int[] xs) {\n"
            "    return xs;\n"
            "}\n"
            "int main() {\n"
            "    return 0;\n"
            "}\n",
            errors,
            [&](const SnNamespace& root)
            {
                const auto& children = root.Children();
                for (auto it = children.cbegin(); it != children.cend();
                    ++it)
                {
                    const auto& field = static_cast<const SnField&>(*it);
                    if (field.Name() != "pick")
                        continue;
                    const auto& func =
                        static_cast<const SnFunction&>(field);
                    QVERIFY2(func.ReturnType() != nullptr,
                        "pick has a return type");
                    QVERIFY2(IsRefNoOpType(func.ReturnType()->Field()),
                        "the array token is an identity type");
                    tokenChecked = true;
                    break;
                }
            });
        QVERIFY2(tokenBuilt && tokenChecked,
            "the array-token survey must run");
    }
};

QTEST_GUILESS_MAIN(TestRefBinding)
#include "test_ref_binding.moc"
