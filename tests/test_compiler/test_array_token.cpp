/*---
test_array_token.cpp - interned array type token unit tests (0.7.3 B).

In-process coverage of the SnArrayTypeToken infrastructure: hash-consing
identity through BuildEnvironment::InternArrayTypeToken, the derived
IsArrayType hook, display form, the full GetCastInfo array matrix
(same-token Same / cross-element None / array->string Auto / int null
bridge Auto), and the argument-binding accept/reject matrix through real
in-process compiles. Console-style suite (same shape as
test_array_flags).

In-process host: Runtime::StaticInit() must run before any Build(),
otherwise Build() segfaults unrecoverably.
---*/
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnTypes.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/Logger.h>
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

//Collects error diagnostics in memory (same shape as test_array_flags).
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
//inputs (same ownership note as test_array_flags), so the outcome
//struct owns all three for as long as the caller touches the tree.
//ModuleManager is process-global and refuses a second Create of the
//same module name, so the output module name is uniquified per call.
struct CompileOutcome
{
    std::unique_ptr<BuildParams> params;
    std::unique_ptr<MemLogger> logger;
    std::unique_ptr<ModuleBuilder> builder;
    bool ok = false;
};

static CompileOutcome compileOne(const char* szBody)
{
    static uint32_t runCount = 0;
    const auto dir = std::filesystem::temp_directory_path()
        / "nlang_array_token_tests";
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
        "array_token_" + std::to_string(++runCount);
    out.params->m_sOutputDir = dir.string();
    out.params->m_sTempDir = dir.string();
    out.logger = std::make_unique<MemLogger>();
    out.builder = std::make_unique<ModuleBuilder>(*out.params, *out.logger);
    try { out.ok = out.builder->Build(); }
    catch (const std::exception&) { out.ok = false; }
    return out;
}

//One real build stays alive for the whole run: Build() mints the
//primitive type field singletons (SnInt32::Instance() and siblings,
//bound to every `int`/`float`/`string` type expression in the tree)
//and the builder owns their AST. Same ownership discipline as
//test_array_flags — the singletons die with the builder.
static const CompileOutcome& primitiveHost()
{
    static CompileOutcome host = compileOne(
        "int main() {\n"
        "    int i = 0;\n"
        "    float f = 0.0;\n"
        "    string s = \"\";\n"
        "    return i;\n"
        "}\n");
    return host;
}

//The primitive type fields as the resolver presents them to CastInfo.
static SnField* IntField()   { return SnInt32::Instance(); }
static SnField* FloatField() { return SnFloat::Instance(); }
static SnField* StrField()   { return SnString::Instance(); }

//A standalone environment just for interning (no build attached).
static BuildEnvironment& tokenEnv()
{
    static BuildParams params;
    static MemLogger logger;
    static BuildEnvironment env(params, logger);
    return env;
}

//Hash-consing identity: one token per element type per environment,
//pointer equality is type equality, display form renders "T[]".
static void test_intern_identity()
{
    TEST(intern_identity);
    auto* pEnv = &tokenEnv();
    auto* t1 = pEnv->InternArrayTypeToken(IntField());
    auto* t2 = pEnv->InternArrayTypeToken(IntField());
    CHECK(t1 != nullptr, "intern returned null");
    CHECK(t1 == t2, "same element must yield the same token");
    CHECK(t1->Kind() == NK_ArrayTypeToken, "kind is ArrayTypeToken");
    CHECK(t1->IsArrayType(), "derived IsArrayType hook");
    CHECK(t1->ElemTypeOf() == IntField(), "element is the int field");
    CHECK(t1->IsTypeField(), "carries the type-field flag family");
    //Display composes the element's own display (Int32 for the int
    //builtin, same as the resolved SnArrayTypeExpr path) + "[]".
    CHECK(t1->ToString() == IntField()->ToString() + "[]",
        "display form is element+[], got: " + t1->ToString());

    auto* ts = pEnv->InternArrayTypeToken(StrField());
    auto* tf = pEnv->InternArrayTypeToken(FloatField());
    CHECK(ts != t1 && tf != t1 && ts != tf,
        "different elements get distinct tokens");

    //A nested token (int[][]) is itself an element: distinct token,
    //own display form, and the element chain is visible.
    auto* iia = pEnv->InternArrayTypeToken(t1);
    auto* iia2 = pEnv->InternArrayTypeToken(t1);
    CHECK(iia == iia2 && iia != t1, "int[][] gets its own token");
    CHECK(iia->ElemTypeOf() == t1, "int[][] element is the int[] token");
    CHECK(iia->ToString() == t1->ToString() + "[]",
        "nested display form composes, got: " + iia->ToString());

    //Interning is TU-scoped: a fresh environment mints its own token
    //for the same element (lifetime = the owning environment).
    static BuildParams otherParams;
    static MemLogger otherLogger;
    static BuildEnvironment otherEnv(otherParams, otherLogger);
    auto* tOther = otherEnv.InternArrayTypeToken(IntField());
    CHECK(tOther != nullptr && tOther != t1,
        "another environment mints its own token");
    CHECK(tOther == otherEnv.InternArrayTypeToken(IntField()),
        "the second environment is self-consistent too");
    PASS();
}

//The GetCastInfo array matrix (D2/D5 + null bridge).
static void test_cast_matrix()
{
    TEST(cast_matrix);
    auto* pEnv = &tokenEnv();
    auto* ia = pEnv->InternArrayTypeToken(IntField());
    auto* fa = pEnv->InternArrayTypeToken(FloatField());
    auto* iia = pEnv->InternArrayTypeToken(ia);

    //D2: same interned token = Same; anything else = None.
    CHECK(GetCastInfo(ia, ia).Kind() == TCK_Same,
        "same interned token = Same");
    CHECK(GetCastInfo(ia, fa).Kind() == TCK_None,
        "int[] vs float[] = None");
    CHECK(GetCastInfo(iia, ia).Kind() == TCK_None,
        "int[][] vs int[] = None");

    //D5: array -> string = Auto (all-position toString coercion).
    CHECK(GetCastInfo(ia, StrField()).Kind() == TCK_Auto,
        "array -> string = Auto");
    CHECK(GetCastInfo(StrField(), ia).Kind() == TCK_None,
        "string -> array = None");

    //Array tokens do not cast to/from primitives (no implicit decay).
    CHECK(GetCastInfo(ia, IntField()).Kind() == TCK_None,
        "array -> int = None");
    CHECK(GetCastInfo(ia, FloatField()).Kind() == TCK_None,
        "array -> float = None");
    CHECK(GetCastInfo(FloatField(), ia).Kind() == TCK_None,
        "float -> array = None");

    //Null bridge: int source (the null literal's type) x array target
    //= Auto no-op; FixupExprType's null-only gate rejects the non-null
    //int upstream. Float has no null form, so it stays None.
    CHECK(GetCastInfo(IntField(), ia).Kind() == TCK_Auto,
        "int (null literal) -> array = Auto");
    PASS();
}

//The argument-binding accept/reject matrix (0.7.3 B / spec §7): real
//in-process compiles of one-line call sites, asserting the verdicts the
//distance/cast layers give array-token arguments.
static void test_param_binding_matrix()
{
    TEST(param_binding_matrix);
    struct Shape
    {
        const char* name;
        const char* formal;   //whole callee declaration
        const char* call;
        bool accept;
    };
    const Shape shapes[] = {
        {"same-elem array formal",
         "int f(int[] a) { return 1; }",
         "int[] a = new int[2]; return f(a);", true},
        {"cross-element array formal",
         "int f(float[] a) { return 1; }",
         "int[] a = new int[2]; return f(a);", false},
        {"string formal (D5 coercion)",
         "int f(string s) { return 1; }",
         "int[] a = new int[2]; return f(a);", true},
        {"int formal",
         "int f(int i) { return 1; }",
         "int[] a = new int[2]; return f(a);", false},
        {"float formal",
         "int f(float g) { return 1; }",
         "int[] a = new int[2]; return f(a);", false},
        {"null into array formal (bridge)",
         "int f(int[] a) { return 1; }",
         "return f(null);", true},
        {"non-null int into array formal",
         "int f(int[] a) { return 1; }",
         "return f(1);", false},
    };
    for (const auto& sh : shapes)
    {
        std::string src = std::string(sh.formal)
            + "\nint main() {\n    " + sh.call + "\n}\n";
        auto out = compileOne(src.c_str());
        if (sh.accept)
            CHECK(out.ok, std::string("must accept: ") + sh.name);
        else
            CHECK(!out.ok, std::string("must reject: ") + sh.name);
    }
    PASS();
}

int main()
{
    Runtime::StaticInit();
    if (!primitiveHost().ok)
    {
        std::cerr << "FAIL: primitive host build did not succeed\n";
        return 1;
    }
    test_intern_identity();
    test_cast_matrix();
    test_param_binding_matrix();
    std::cerr << "\narray_token: " << g_pass << " passed, "
        << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
