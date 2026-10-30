/*---
test_generic_decl.cpp - generic class declaration tests.

In-process ModuleBuilder coverage of the `class Name<T, ...>` declaration
form under the v0.12.3 dictionary model: mixed members (bodies, fields,
native) in one template, instantiation signature substitution, and the
declaration-level contract (bare type-parameter initializers/defaults,
type-parameter arrays, member-name collisions).
---*/
#include <QtTest/QtTest>
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/CastInfo.h>
#include "builder/ModuleRegistry.h"
#include <nlang/compiler/SyntheticBuiltinRegistry.h>
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/ScriptLocation.h>
#include <memory>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
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
        std::filesystem::temp_directory_path() / "nlang_generic_decl_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path file = dir / name.toStdString();
    std::ofstream(file, std::ios::binary) << body;
    return file.string();
}

//Compile one source file; returns (build ok, error texts).
std::pair<bool, std::vector<std::string>> compileSource(
    const std::string& tag, const std::string& path)
{
    const auto dir = std::filesystem::temp_directory_path()
        / "nlang_generic_decl_tests";
    BuildParams params;
    params.m_SourceFiles.push_back(path);
    params.m_sOutputModule = tag;
    params.m_sOutputDir = dir.string();
    params.m_sTempDir = dir.string();
    params.m_sStdLibDir = STDLIB_DIR;
    MemLogger logger;
    ModuleBuilder builder(params, logger);
    bool ok = false;
    //Exception boundary (same contract as ncc main): codegen internal
    //errors throw; without the catch the abort would kill the test run.
    try { ok = builder.BuildArtifacts(); }
    catch (const std::exception&) { ok = false; }
    return {ok, logger.errorsText()};
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

} //namespace

class TestGenericDecl : public QObject
{
    Q_OBJECT
private slots:
    //Runtime tables (IdString etc.) must exist before any Build().
    void initTestCase() {
        Runtime::StaticInit();
        TypeCastInfo::StaticInit();
    }

    //A generic declaration with all-native members parses (grammar-level
    //acceptance — resolving T inside member signatures is Task 2 of the
    //plan, so build-clean is not asserted here).
    void declGenericAcceptsNativeMembers()
    {
        const auto path = writeTempSource("generic_ok.n",
            "class Box<T> {\n"
            "    public native T get();\n"
            "    public native void set(T v);\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_decl_ok", path);
        QVERIFY2(!anyErrorMentions(outcome.second, "syntax error"),
            "class Box<T> must parse");
    }

    //v0.12.3 dictionary model: members mix freely — a method body, a
    //T field and a native member coexist in one template (one row,
    //body compiled once against placeholder types).
    void declGenericAcceptsMixedMembers()
    {
        const auto path = writeTempSource("generic_mixed.n",
            "class Box<T> {\n"
            "    public T item;\n"
            "    public Box() { }\n"
            "    public T get() { return item; }\n"
            "    public native int marker();\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_mixed", path);
        QVERIFY2(outcome.first,
            "mixed-member template must build clean");
        QVERIFY2(!anyErrorMentions(outcome.second, "native"),
            "mixed-member template must not demand all-native members");
    }

    //v0.12.3 rule alignment: a method body compiles inside a declared
    //generic (the body is compiled once against placeholder types; the
    //"all members native" restriction is retired with the dictionary
    //model).
    void declGenericAcceptsMethodBodies()
    {
        const auto path = writeTempSource("generic_body.n",
            "class Box<T> {\n"
            "    public int get() {\n"
            "        return 0;\n"
            "    }\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_decl_body", path);
        QVERIFY2(outcome.first,
            "a method body in a generic declaration must build clean");
        QVERIFY2(!anyErrorMentions(outcome.second, "native"),
            "a method body in a generic declaration must not demand native");
    }

    //A declared generic instantiates: the member signatures substitute
    //the type arguments (Box<int>.get() reads as int), and the class
    //builds clean end to end.
    void declGenericInstantiatesMembers()
    {
        const auto path = writeTempSource("generic_inst.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native T get();\n"
            "    public native void set(T v);\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    int x = b.get();\n"
            "    b.set(x);\n"
            "    return 0;\n"
            "}\n");
        const auto outcome = compileSource("generic_inst", path);
        QVERIFY2(outcome.first,
            "declared generic instantiation must build clean");
        QVERIFY2(outcome.second.empty(),
            "no diagnostics expected for the instantiated generic");
    }

    //Two or more type parameters: the declaration list and the use-site
    //argument list both carry commas (regression for the 0.11.0
    //single-parameter-only TypeParams shape).
    void declGenericMultiParamDeclares()
    {
        const auto path = writeTempSource("generic_multi.n",
            "class Pair<K, V> {\n"
            "    public native Pair();\n"
            "    public native K first();\n"
            "    public native void second(V v);\n"
            "}\n"
            "int main() {\n"
            "    Pair<int, string> p = Pair();\n"
            "    p.second(\"x\");\n"
            "    return 0;\n"
            "}\n");
        const auto outcome = compileSource("generic_multi", path);
        QVERIFY2(outcome.first,
            "multi-parameter generic declaration must build clean");
        QVERIFY2(outcome.second.empty(),
            "no diagnostics expected for the multi-parameter generic");
    }

    //The type-argument count is checked against the declaration's
    //parameter list.
    void declGenericRejectsWrongArity()
    {
        const auto path = writeTempSource("generic_arity.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native T get();\n"
            "}\n"
            "int main() {\n"
            "    Box<int, string> b = Box();\n"
            "    return 0;\n"
            "}\n");
        const auto outcome = compileSource("generic_arity", path);
        QVERIFY2(!outcome.first,
            "wrong type-argument count must fail the build");
        QVERIFY2(anyErrorMentions(outcome.second, "type argument"),
            "diagnostic must name the type-argument count mismatch");
    }

    //A generic declaration used bare (no type arguments) is not a type:
    //the diagnostic names the instantiation form.
    void declGenericRejectsBareType()
    {
        const auto path = writeTempSource("generic_bare.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native T get();\n"
            "}\n"
            "int main() {\n"
            "    Box b = Box();\n"
            "    return 0;\n"
            "}\n");
        const auto outcome = compileSource("generic_bare", path);
        QVERIFY2(!outcome.first,
            "a bare generic declaration must not be usable as a type");
        QVERIFY2(anyErrorMentions(outcome.second, "type argument"),
            "diagnostic must point at the missing type arguments");
    }

    //A class FIELD of declared-generic type mints the instantiation in
    //the declaration pass — before the template's own signatures resolve.
    //The mint must resolve the clone's signature surface: an unresolved
    //return type read as void here, and the cache served the broken
    //instance to every later use of the same instantiation.
    void declGenericFieldMintResolvesMembers()
    {
        const auto path = writeTempSource("generic_field_mint.n",
            "class C {\n"
            "    public Promise<int> p;\n"
            "}\n"
            "int main() {\n"
            "    Promise<int> p = Promise();\n"
            "    Future<int> f = p.future();\n"
            "    return 0;\n"
            "}\n");
        const auto outcome = compileSource("generic_field_mint", path);
        QString diag;
        for (const auto& e : outcome.second) diag += QString::fromStdString(e) + " | ";
        QVERIFY2(outcome.first,
            qPrintable("a declared-generic field must mint a complete "
                       "instance — errors: " + diag));
        QVERIFY2(!anyErrorMentions(outcome.second, "void function"),
            "future() must keep its declared return type");
    }
    //A member named like a type parameter is rejected: within the
    //declaration's scope the parameter name is a type, so a member with
    //that name would shadow it (spec §8: type-parameter/member name
    //collision is a declaration-level error).
    void declGenericRejectsTypeParamMemberNameCollision()
    {
        const auto path = writeTempSource("generic_name_clash.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native int T();\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_name_clash", path);
        QVERIFY2(!outcome.first,
            "a member named like a type parameter must be rejected");
        QVERIFY2(anyErrorMentions(outcome.second, "T"),
            "diagnostic must name the colliding member");
    }

    //An empty generic body parses (the marker-class shape, consistent
    //with empty non-generic class bodies).
    void declGenericAcceptsEmptyBody()
    {
        const auto path = writeTempSource("generic_empty.n",
            "class Empty<T> { }\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_empty", path);
        QVERIFY2(!anyErrorMentions(outcome.second, "syntax error"),
            "an empty generic body must parse");
    }

    //A generic declaration composes with inheritance and interface
    //implementation (the ClassDecl productions keep their optional
    //clauses under a type-parameter list).
    void declGenericAcceptsInheritanceCombos()
    {
        const auto path = writeTempSource("generic_inherit.n",
            "class Base {\n"
            "    public int b;\n"
            "}\n"
            "interface IF {\n"
            "    public int ifc();\n"
            "}\n"
            "class WithBase<T> : Base {\n"
            "    public native int size();\n"
            "}\n"
            "class WithImpl<T> implements IF {\n"
            "    public native int ifc();\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_inherit", path);
        QVERIFY2(!anyErrorMentions(outcome.second, "syntax error"),
            "inheritance/implementation must compose with a type-parameter list");
        QVERIFY2(outcome.first,
            "generic inheritance/implementation combos must build clean");
    }

    //v0.12.3 rule alignment: a concrete-typed field initializer works
    //on generics exactly as on ordinary classes.
    void declGenericAcceptsConcreteFieldInitializer()
    {
        const auto path = writeTempSource("generic_field_init.n",
            "class Box<T> {\n"
            "    public int count = 5;\n"
            "    public T item;\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_field_init", path);
        QVERIFY2(outcome.first,
            "a concrete-typed field initializer must build clean");
        QVERIFY2(!anyErrorMentions(outcome.second, "initializer"),
            "a concrete-typed field initializer must be allowed");
    }

    //A T-typed field initializer is rejected: T has no concrete kind,
    //so no meaningful initial value exists (constructors may
    //initialize; the null right-hand side also pins the null-vs-T
    //rule).
    void declGenericRejectsTypeParamFieldInitializer()
    {
        const auto path = writeTempSource("generic_t_init.n",
            "class Box<T> {\n"
            "    public T item = null;\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_t_init", path);
        QVERIFY2(!outcome.first,
            "a T-typed field initializer must be rejected");
        QVERIFY2(anyErrorMentions(outcome.second, "type parameter"),
            "diagnostic must name the type parameter rule");
    }

    //v0.12.3 rule alignment: a concrete-typed parameter default value
    //works on generics exactly as on ordinary classes.
    void declGenericAcceptsConcreteParamDefault()
    {
        const auto path = writeTempSource("generic_default.n",
            "class Box<T> {\n"
            "    public native int f(int n = 3);\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_default", path);
        QVERIFY2(outcome.first,
            "a concrete-typed parameter default must build clean");
        QVERIFY2(!anyErrorMentions(outcome.second, "default value"),
            "a concrete-typed parameter default must be allowed");
    }

    //A T-typed parameter default is rejected: with no constraint on T
    //there is no meaningful default value (same rule as T-typed field
    //initializers).
    void declGenericRejectsTypeParamDefault()
    {
        const auto path = writeTempSource("generic_t_default.n",
            "class Box<T> {\n"
            "    public native void f(T v = null);\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_t_default", path);
        QVERIFY2(!outcome.first,
            "a T-typed parameter default must be rejected");
        QVERIFY2(anyErrorMentions(outcome.second, "type parameter"),
            "diagnostic must name the type parameter rule");
    }

    //An array of a type parameter has no representable element kind in
    //the shared template row — rejected at the type position with a
    //rule diagnostic, not a backend internal error (the array token
    //would intern over the placeholder and the registration walk fails
    //to find a class named like the parameter).
    void declGenericRejectsTypeParamArrayField()
    {
        const auto path = writeTempSource("generic_t_array.n",
            "class Box<T> {\n"
            "    public T[] items;\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_t_array", path);
        QVERIFY2(!outcome.first,
            "a T[] field must be rejected");
        QVERIFY2(anyErrorMentions(outcome.second, "type parameter"),
            "diagnostic must name the type-parameter array rule");
    }

    //Until instance execution lands, a bodied member is not on an
    //instantiation's call surface: the clone carries the signature
    //only, and a call bound to it would reach the backend
    //unregistered. The call must fail resolution with the ordinary
    //unknown-member diagnostic, not an internal error.
    void declGenericRejectsBodyMemberCall()
    {
        const auto path = writeTempSource("generic_body_call.n",
            "class Box<T> {\n"
            "    public int get() { return 1; }\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    int x = b.get();\n"
            "    return x;\n"
            "}\n");
        const auto outcome = compileSource("generic_body_call", path);
        QVERIFY2(!outcome.first,
            "a bodied-member call on an instantiation must be rejected");
        QVERIFY2(anyErrorMentions(outcome.second,
                "does not exist or is not accessible"),
            "diagnostic must be the unknown-member text, not an "
            "internal error");
    }

    //Concrete parameter defaults survive instantiation: the member
    //clone deep-copies its default (literal and constant-operator
    //forms), so a call omitting the argument matches the clone's arity
    //and emits the default — no "not compatible" arity lie.
    void declGenericInstantiatesConcreteParamDefault()
    {
        const auto path = writeTempSource("generic_clone_default.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native int f(int n = 3);\n"
            "    public native int g(int n = 1 + 2);\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    int r = b.f() + b.g();\n"
            "    return r;\n"
            "}\n");
        const auto outcome = compileSource("generic_clone_default", path);
        QString diag;
        for (const auto& e : outcome.second)
            diag += QString::fromStdString(e) + " | ";
        QVERIFY2(outcome.first,
            qPrintable("calls omitting defaulted args must match the "
                       "clone — errors: " + diag));
    }

    //A sized array of a type parameter in a method body — the
    //C-style trailing declarator `T a[3];` — interns its token over
    //the placeholder on the declaration channel (not through the
    //type-expression guard). The rule diagnostic must fire there too,
    //not the backend internal error of the unguarded intern.
    void declGenericRejectsSizedTypeParamArray()
    {
        const auto path = writeTempSource("generic_t_sized.n",
            "class Box<T> {\n"
            "    public int get() {\n"
            "        T a[3];\n"
            "        return 1;\n"
            "    }\n"
            "}\n"
            "int main() { return 0; }\n");
        const auto outcome = compileSource("generic_t_sized", path);
        QVERIFY2(!outcome.first,
            "a sized T array declarator must be rejected");
        QVERIFY2(anyErrorMentions(outcome.second, "type parameter"),
            "diagnostic must name the type-parameter array rule, not an "
            "internal error");
    }

    //The value channel of the signature-only member gate: a bound
    //method reference (func<...> f = b.get) resolves through the
    //identifier binding, not the invoke collector — the clone must
    //stay off that surface too, with the ordinary unknown-member
    //diagnostic instead of a backend internal error.
    void declGenericRejectsMethodValueBinding()
    {
        const auto path = writeTempSource("generic_method_ref.n",
            "class Box<T> {\n"
            "    public int get() { return 1; }\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    func<int> f = b.get;\n"
            "    return f();\n"
            "}\n");
        const auto outcome = compileSource("generic_method_ref", path);
        QVERIFY2(!outcome.first,
            "a method reference to a bodied member must be rejected");
        QVERIFY2(anyErrorMentions(outcome.second,
                "Cannot resolve the field"),
            "diagnostic must be the unknown-member text, not an "
            "internal error");
    }

    //Constant REFERENCE defaults survive instantiation: an enum-member
    //default is legal on ordinary classes, so the member clone must
    //carry it as a bound copy — an omitted-argument call matches the
    //clone's arity instead of failing with the arity lie.
    void declGenericInstantiatesEnumMemberDefault()
    {
        const auto path = writeTempSource("generic_enum_default.n",
            "enum Color { Red, Green }\n"
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native int f(Color c = Color.Red);\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    return b.f();\n"
            "}\n");
        const auto outcome = compileSource("generic_enum_default", path);
        QString diag;
        for (const auto& e : outcome.second)
            diag += QString::fromStdString(e) + " | ";
        QVERIFY2(outcome.first,
            qPrintable("an enum-member default must survive "
                       "instantiation — errors: " + diag));
    }

    //A string default is the ownership-critical clone arm (the payload
    //re-allocates); pinned so the arm cannot silently regress to the
    //arity lie. Coverage pin for already-correct behavior.
    void declGenericInstantiatesStringParamDefault()
    {
        const auto path = writeTempSource("generic_str_default.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native int f(string s = \"none\");\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    return b.f();\n"
            "}\n");
        const auto outcome = compileSource("generic_str_default", path);
        QString diag;
        for (const auto& e : outcome.second)
            diag += QString::fromStdString(e) + " | ";
        QVERIFY2(outcome.first,
            qPrintable("a string default must survive instantiation — "
                       "errors: " + diag));
    }

    //The cloned operator default must keep its resolved type channel:
    //EmitDefaultBinding keys the scalar cast off the default
    //expression's EvalDataType, so a clone of `double d = 1 + 2` that
    //loses it emits int bits into the wider formal slot (bytecode-
    //proven regression). Asserts at the mint seam through the
    //build's declared-generic registry.
    void declGenericCloneDefaultKeepsOperatorType()
    {
        const auto path = writeTempSource("generic_op_default.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native int f(double d = 1 + 2);\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    return b.f();\n"
            "}\n");
        const auto dir = std::filesystem::temp_directory_path()
            / "nlang_generic_decl_tests";
        BuildParams params;
        params.m_SourceFiles.push_back(path);
        params.m_sOutputModule = "generic_op_default";
        params.m_sOutputDir = dir.string();
        params.m_sTempDir = dir.string();
        params.m_sStdLibDir = STDLIB_DIR;
        MemLogger logger;
        ModuleBuilder builder(params, logger);
        bool built = false;
        try { built = builder.BuildArtifacts(); }
        catch (const std::exception&) { built = false; }
        QVERIFY2(built, "a mixed-width operator default must build clean");

        const GenericInstanceRegistry& generics =
            builder.Registry().DeclaredGenerics();
        SnExpression* pCloneDefault = nullptr;
        auto collect = [&generics, &pCloneDefault](uint32_t ownerUnit) {
            generics.ForEachInstance(ownerUnit,
                [&pCloneDefault](SnClassDecl& inst) {
                    if (pCloneDefault || inst.Name() != "Box")
                        return;
                    for (auto& member : inst.Members())
                        if (member.Kind() == NK_Function
                                && member.Name() == "f")
                            for (auto& param : static_cast<SnFunction&>(
                                    member).Params())
                                if (!pCloneDefault)
                                    pCloneDefault = param.Value();
                });
        };
        //The owning unit index is not exposed through the registry
        //surface this test can reach, so the sweep scans the unit
        //range small builds occupy (0..16) plus the detached NO_OWNER
        //pool — cheap and complete for single-TU builds.
        for (uint32_t unit = 0; unit <= 16; ++unit)
            collect(unit);
        collect(ModuleRegistry::NO_OWNER);
        QVERIFY2(pCloneDefault != nullptr,
            "the Box instantiation must carry the f clone");
        QVERIFY2(pCloneDefault->Kind() == NK_BinaryExpr,
            "the clone's default is the operator tree");
        QVERIFY2(pCloneDefault->EvalDataType() != nullptr,
            "the cloned operator default must keep its resolved type "
            "channel (the emit-time scalar cast keys off it)");
    }

    //A default referencing the method's own instance field through the
    //implicit-this form must survive instantiation as a TRANSFERRED
    //binding — the clone emits it exactly like the template row (the
    //receiver field read at the caller). An unbound copy reaches
    //codegen unresolved and dies with an internal error.
    void declGenericImplicitFieldDefaultBinds()
    {
        const auto path = writeTempSource("generic_impl_default.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public int count;\n"
            "    public native int f(int n = count);\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    return b.f();\n"
            "}\n");
        const auto outcome = compileSource("generic_impl_default", path);
        QVERIFY2(outcome.first,
            "an implicit-this field default must build on the clone "
            "with its binding transferred");
    }

    //A member-expr default (`n = p.x`) binds a formal receiver and a
    //cross-class field — both declaration nodes are stable across the
    //build, so the clone carries transferred bindings and compiles in
    //every arity form (the runtime value pin lives in
    //test_native_host's TestGenericMemberDefault).
    void declGenericFormalMemberDefaultBinds()
    {
        const auto path = writeTempSource("generic_member_default.n",
            "class Payload {\n"
            "    public int x;\n"
            "    public Payload() { x = 11; }\n"
            "}\n"
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native int f(Payload p, int n = p.x);\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    Payload p = Payload();\n"
            "    return b.f(p) + b.f(p, 5);\n"
            "}\n");
        const auto outcome = compileSource("generic_member_default", path);
        QVERIFY2(outcome.first,
            "a formal-receiver member default must build on the clone "
            "(reliance and all-args forms alike)");
    }

    //Invoke-shaped defaults (`n = C.g()`) are outside the clone's
    //constant-default surface — the degraded no-default form reports
    //the arity mismatch at the reliance site (the same surface as any
    //unsupported default shape).
    void declGenericInvokeDefaultDegradesToArity()
    {
        const auto path = writeTempSource("generic_invoke_default.n",
            "class C {\n"
            "    public static int g() { return 3; }\n"
            "}\n"
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public native int f(int n = C.g());\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    return b.f();\n"
            "}\n");
        const auto outcome = compileSource("generic_invoke_default", path);
        QVERIFY2(!outcome.first,
            "an invoke-shaped default must degrade on the clone");
        QVERIFY2(anyErrorMentions(outcome.second,
                "not compatible with the declaration"),
            "the degrade must surface as the arity diagnostic, not an "
            "internal error");
    }

    //An explicit this-receiver default (`n = this.count`) must survive
    //instantiation like the implicit-this form: the clone rebuilds the
    //this expression WITH its type channel (the member ladder's
    //typed-receiver dispatch reads it — see the structural pin below)
    //and transfers the field binding, so every arity form compiles.
    void declGenericThisFieldDefaultBinds()
    {
        const auto path = writeTempSource("generic_this_default.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public int count;\n"
            "    public native int f(int n = this.count);\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    return b.f();\n"
            "}\n");
        const auto outcome = compileSource("generic_this_default", path);
        QVERIFY2(outcome.first,
            "an explicit this-receiver default must build on the clone");
    }

    //The same this-receiver inside an operator default (`a + this.count`)
    //exercises the operator arm's operand recursion — an unhandled
    //this-expr operand degrades the WHOLE tree to no default.
    void declGenericBinaryThisFieldDefaultBinds()
    {
        const auto path = writeTempSource("generic_this_binary_default.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public int count;\n"
            "    public native int f(int a, int n = a + this.count);\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    return b.f(1);\n"
            "}\n");
        const auto outcome = compileSource("generic_this_binary_default",
            path);
        QVERIFY2(outcome.first,
            "an operator default with a this-receiver operand must build "
            "on the clone");
    }

    //The this-receiver clone must keep the TYPE CHANNEL, not just the
    //name shape: resolution stamps EvalDataType(classDecl) on a this
    //expression and the member ladder's typed-receiver dispatch reads
    //member.Outer()->EvalDataType() — a null channel makes every arm
    //decline and the field read silently emits nothing (the staged
    //default is then an unwritten frame cell; runtime values cannot
    //discriminate this window because instance fields stay zero until
    //Task 5). Asserts at the mint seam like the operator-type pin.
    void declGenericCloneDefaultKeepsThisType()
    {
        const auto path = writeTempSource("generic_this_type.n",
            "class Box<T> {\n"
            "    public native Box();\n"
            "    public int count;\n"
            "    public native int f(int n = this.count);\n"
            "}\n"
            "int main() {\n"
            "    Box<int> b = Box();\n"
            "    return b.f();\n"
            "}\n");
        const auto dir = std::filesystem::temp_directory_path()
            / "nlang_generic_decl_tests";
        BuildParams params;
        params.m_SourceFiles.push_back(path);
        params.m_sOutputModule = "generic_this_type";
        params.m_sOutputDir = dir.string();
        params.m_sTempDir = dir.string();
        params.m_sStdLibDir = STDLIB_DIR;
        MemLogger logger;
        ModuleBuilder builder(params, logger);
        bool built = false;
        try { built = builder.BuildArtifacts(); }
        catch (const std::exception&) { built = false; }
        QVERIFY2(built, "a this-receiver default must build clean");

        const GenericInstanceRegistry& generics =
            builder.Registry().DeclaredGenerics();
        SnExpression* pCloneDefault = nullptr;
        auto collect = [&generics, &pCloneDefault](uint32_t ownerUnit) {
            generics.ForEachInstance(ownerUnit,
                [&pCloneDefault](SnClassDecl& inst) {
                    if (pCloneDefault || inst.Name() != "Box")
                        return;
                    for (auto& member : inst.Members())
                        if (member.Kind() == NK_Function
                                && member.Name() == "f")
                            for (auto& param : static_cast<SnFunction&>(
                                    member).Params())
                                if (!pCloneDefault)
                                    pCloneDefault = param.Value();
                });
        };
        //The owning unit index is not exposed through the registry
        //surface this test can reach, so the sweep scans the unit
        //range small builds occupy (0..16) plus the detached NO_OWNER
        //pool — cheap and complete for single-TU builds.
        for (uint32_t unit = 0; unit <= 16; ++unit)
            collect(unit);
        collect(ModuleRegistry::NO_OWNER);
        QVERIFY2(pCloneDefault != nullptr,
            "the Box instantiation must carry the f clone");
        QVERIFY2(pCloneDefault->Kind() == NK_MemberExpr,
            "the clone's default is the this-receiver member tree");
        auto* pOuter = static_cast<SnMemberExpr&>(
            *pCloneDefault).Outer();
        QVERIFY2(pOuter && pOuter->Kind() == NK_ThisExpr,
            "the member tree's outer is the cloned this expression");
        QVERIFY2(pOuter->EvalDataType() != nullptr,
            "the cloned this expression must keep its resolved type "
            "channel (a null channel makes the member ladder decline "
            "and the field read emits nothing)");
    }

    //Two in-process builds with different synthetic-generic shapes. The
    //synthetic caches (List/Dict instantiations, ByteStream singleton,
    //static-face qualifier) were process-global maps keyed by raw tree
    //pointers — the same corruption class the declared-generic registry
    //fix scoped per build: once the allocator reuses a freed address
    //from the first build, a stale entry can serve the second. The
    //shape difference is the detector — a stale int-shaped List/Dict
    //serving B's string uses is a compile error, so each cycle asserts
    //every shape still builds clean after the others.
    void syntheticsSecondBuildIsolated()
    {
        const auto pathA = writeTempSource("syn_two_a.n",
            "int main() {\n"
            "    List<int> l = [10, 20, 30];\n"
            "    int x = l.get(1);\n"
            "    Dict<string, int> d = {};\n"
            "    d.set(\"one\", 1);\n"
            "    ByteStream bs = ByteStream();\n"
            "    bs.writeInt(x);\n"
            "    return 0;\n"
            "}\n");
        const auto pathB = writeTempSource("syn_two_b.n",
            "int main() {\n"
            "    List<string> l = [\"a\", \"b\"];\n"
            "    string s = l.get(0);\n"
            "    Dict<int, string> d = {};\n"
            "    d.set(1, \"one\");\n"
            "    ByteStream bs = ByteStream();\n"
            "    bs.writeString(s);\n"
            "    return 0;\n"
            "}\n");
        const auto pathC = writeTempSource("syn_two_c.n",
            "int main() {\n"
            "    Promise<int> pa = Promise();\n"
            "    Future<int> a = pa.future();\n"
            "    Future<int>[] arr = [a];\n"
            "    Future.selectAny(arr);\n"
            "    return 0;\n"
            "}\n");
        for (int cycle = 0; cycle != 3; ++cycle)
        {
            const auto a = compileSource("syn_two_a", pathA);
            QVERIFY2(a.first && a.second.empty(),
                "int-shaped synthetics must build clean");
            const auto b = compileSource("syn_two_b", pathB);
            QVERIFY2(b.first && b.second.empty(),
                "string-shaped synthetics must build clean after the "
                "int-shaped build (a stale int instance would reject "
                "string s = l.get(0))");
            const auto c = compileSource("syn_two_c", pathC);
            QVERIFY2(c.first && c.second.empty(),
                "Promise/Future/selectAny synthetics must build clean "
                "after the container builds");
        }
    }

    //The per-build boundary (final-review finding): RegisterUnits calls
    //ModuleRegistry::Reset() to drop the previous run's module tables
    //("owner tags point into its destroyed AST") — the per-build
    //registries must drop with them. A surviving registry keeps entries
    //keyed by pointers into that same destroyed tree, which is the
    //same-address corruption class both registries exist to remove.
    //Pinned at the registry seam (the double-BuildArtifacts flow is not
    //a supported path — TU re-registration rejects the duplicate
    //package before Reset's downstream): adopt into a standalone
    //ModuleRegistry, Reset, and both registries must come back empty
    //(the adopted nodes are freed with their registry).
    void registryResetDropsPerBuildRegistries()
    {
        ModuleRegistry reg;
        auto makeDecl = [] {
            return new SnClassDecl(new std::string("Minted"),
                nullptr, nullptr, ScriptLocation());
        };
        SyntheticBuiltinRegistry::GenericInstKey key;
        key.baseName = "List";
        SnClassDecl* pGeneric = makeDecl();
        SnClassDecl* pSingleton = makeDecl();
        reg.SyntheticBuiltins().AdoptGeneric(key, pGeneric);
        reg.SyntheticBuiltins().AdoptBuiltinClass("ByteStream", pSingleton);
        //Templates are raw references (the registry owns instances and
        //placeholders only), so this one stays test-owned.
        std::unique_ptr<SnClassDecl> upTemplate(makeDecl());
        SnClassDecl* pInstance = makeDecl();
        reg.DeclaredGenerics().AdoptInstance(pInstance, upTemplate.get(),
            {}, 0);
        QCOMPARE(reg.SyntheticBuiltins().MintedCount(), size_t(2));
        QVERIFY(reg.DeclaredGenerics().InstanceOf(pInstance)
            == upTemplate.get());

        reg.Reset();

        QCOMPARE(reg.SyntheticBuiltins().MintedCount(), size_t(0));
        //pInstance was freed with the reset registry; only its pointer
        //VALUE is compared here (map lookup, no dereference).
        QVERIFY(reg.DeclaredGenerics().InstanceOf(pInstance) == nullptr);
    }
};

QTEST_GUILESS_MAIN(TestGenericDecl)
#include "test_generic_decl.moc"
