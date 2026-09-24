/*---
test_array_flags.cpp - generic instance array-type-arg unit tests.

In-process ModuleBuilder coverage of the instantiation key split:
generic instantiations that differ only in the array-ness of a type
argument (List<int> vs List<int[]>) must mint distinct synthetic
SnClassDecl instances. Since 0.7.3 B the split is carried by the type
argument itself — an array-typed argument IS the interned
SnArrayTypeToken (pointer identity), so GenericTypeArgs()[0] exposes
the array-ness directly. Console-style suite (same shape as
test_stdlib); the e2e suite pins the end-to-end compile/run behavior.
---*/
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SyntaxNode.h>
#include "VmExecutor.h"
#include "ModuleLoader.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
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

//Collects error diagnostics in memory (same shape as test_array_property).
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
//inputs (same ownership note as test_array_property), so the outcome
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
        / "nlang_array_flags_tests";
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
        "array_flags_" + std::to_string(++runCount);
    out.params->m_sOutputDir = dir.string();
    out.params->m_sTempDir = dir.string();
    out.logger = std::make_unique<MemLogger>();
    out.builder = std::make_unique<ModuleBuilder>(*out.params, *out.logger);
    try { out.ok = out.builder->Build(); }
    catch (const std::exception&) { out.ok = false; }
    return out;
}

//Whole-tree walk collecting resolved generic type expressions (same
//containment-invariant walk as test_array_property).
static std::vector<SnGenericTypeExpr*> collectGenericTypeExprs(
    const SnNamespace& root)
{
    std::vector<SnGenericTypeExpr*> found;
    std::function<void(const SyntaxNode&)> walk =
        [&](const SyntaxNode& node) {
            if (node.Kind() == NK_GenericTypeExpr)
                found.push_back(const_cast<SnGenericTypeExpr*>(
                    static_cast<const SnGenericTypeExpr*>(&node)));
            const auto& children = node.Children();
            for (auto it = children.cbegin(); it != children.cend(); ++it)
                walk(static_cast<const SyntaxNode&>(*it));
        };
    walk(root);
    return found;
}

//The synthetic instantiation behind a resolved generic type expression.
static SnClassDecl* instantiationOf(const SnGenericTypeExpr& expr)
{
    return static_cast<SnClassDecl*>(expr.EvalDataType());
}

//helper used by the CHECK messages above.
static std::string joinErrors(const MemLogger& logger)
{
    std::string all;
    for (const auto& e : logger.errors())
        all += e + "\n";
    return all;
}

//Compile + execute (test_stdlib's runSource shape): loads the .nmod the
//build wrote and runs main, returning its value. A VM runtime error
//propagates out — the red signal of the GC-arm tests below.
static int runOne(const char* szBody)
{
    auto out = compileOne(szBody);
    if (!out.ok)
        return -1;
    const auto modPath = std::filesystem::temp_directory_path()
        / "nlang_array_flags_tests"
        / (out.params->m_sOutputModule + ".nmod");
    CompiledModule mod = ModuleLoader::Load(modPath.string());
    VmExecutor exec;
    return exec.Execute(mod);
}

//C-period hole 4 (List arm): an array reachable ONLY through a List
//element slot must survive GC pressure. Today the boxed int handle
//hides the record from the tracer, the sweep clears its kind, and the
//hole-3 opcode kind check turns the dangling read into a named throw.
static void test_gc_list_arm_traces_array()
{
    TEST(gc_list_arm_traces_array);
    int rc = -1;
    try {
        rc = runOne(
            "int[] mkArr(int v) {\n"
            "    int[] a = new int[2];\n"
            "    a[0] = v;\n"
            "    a[1] = v + 1;\n"
            "    return a;\n"
            "}\n"
            "int churn() {\n"
            "    List<int[]> keep = new List<int[]>();\n"
            "    keep.add(mkArr(40));\n"
            "    for (int i = 0; i < 3000; i = i + 1) {\n"
            "        List<int[]> tmp = new List<int[]>();\n"
            "        tmp.add(mkArr(i));\n"
            "    }\n"
            "    return keep.get(0)[0] + keep.get(0)[1];\n"
            "}\n"
            "int main() { return churn(); }\n");
    } catch (const std::runtime_error& e) {
        CHECK(false, std::string("array must stay traceable through ")
            + "the List slot, got: " + e.what());
    }
    CHECK(rc == 81, "elements survive with values, main returned "
        + std::to_string(rc));
    PASS();
}

//C-period hole 4 (Dict arm): same liveness discipline through the
//Dict VALUE slot.
static void test_gc_dict_arm_traces_array()
{
    TEST(gc_dict_arm_traces_array);
    int rc = -1;
    try {
        rc = runOne(
            "int[] mkArr(int v) {\n"
            "    int[] a = new int[2];\n"
            "    a[0] = v;\n"
            "    a[1] = v + 1;\n"
            "    return a;\n"
            "}\n"
            "int churn() {\n"
            "    Dict<string,int[]> keep = new Dict<string,int[]>();\n"
            "    keep.set(\"k\", mkArr(5));\n"
            "    for (int i = 0; i < 3000; i = i + 1) {\n"
            "        Dict<string,int[]> tmp = new Dict<string,int[]>();\n"
            "        tmp.set(\"x\", mkArr(i));\n"
            "    }\n"
            "    return keep.get(\"k\")[0] + keep.get(\"k\")[1];\n"
            "}\n"
            "int main() { return churn(); }\n");
    } catch (const std::runtime_error& e) {
        CHECK(false, std::string("array must stay traceable through ")
            + "the Dict value slot, got: " + e.what());
    }
    CHECK(rc == 11, "elements survive with values, main returned "
        + std::to_string(rc));
    PASS();
}

//Instantiations differing only in the array-ness of a type argument
//must NOT share the cached synthetic class: the interned token in the
//type-arg slot keeps the keys distinct (0.7.3 B; the pre-token erased
//key collapsed them).
static void test_generic_array_flags_split_keys()
{
    TEST(generic_array_flags_split_keys);
    auto out = compileOne(
        "int main() {\n"
        "    List<int> p = new List<int>();\n"
        "    List<int[]> q = new List<int[]>();\n"
        "    q.add(new int[2]);\n"
        "    return 0;\n"
        "}\n");
    CHECK(out.ok, "build failed: "
        + (out.logger ? joinErrors(*out.logger) : std::string("?")));
    const auto& root = out.builder->TreeRootView();
    auto exprs = collectGenericTypeExprs(root);
    CHECK(exprs.size() == 4, "expected 4 generic type exprs (2 decl + "
        "2 new), got " + std::to_string(exprs.size()));

    //Group by instantiation pointer: List<int> appears twice (decl+new)
    //and List<int[]> twice — identity is the existing cache invariant.
    std::set<SnClassDecl*> distinct;
    for (auto* pExpr : exprs)
        distinct.insert(instantiationOf(*pExpr));
    CHECK(distinct.size() == 2,
        "array-ness must split the instance key, got "
        + std::to_string(distinct.size()) + " distinct class(es)");

    SnClassDecl* pPlain = nullptr;
    SnClassDecl* pArrayOf = nullptr;
    for (auto* pExpr : exprs) {
        bool isArrayArg = !pExpr->TypeArgs().empty()
            && pExpr->TypeArgs()[0]
            && pExpr->TypeArgs()[0]->Kind() == NK_ArrayTypeExpr;
        if (isArrayArg)
            pArrayOf = instantiationOf(*pExpr);
        else
            pPlain = instantiationOf(*pExpr);
    }
    CHECK(pPlain && pArrayOf, "both variants instantiated");
    CHECK(pPlain != pArrayOf, "plain and array-of must not share a class");
    //The type-arg slot carries the identity: the interned token for the
    //array variant, the plain primitive field for the scalar variant.
    CHECK(pArrayOf->GenericTypeArgs().size() == 1
        && pArrayOf->GenericTypeArgs()[0]->Kind() == NK_ArrayTypeToken,
        "array variant's type arg is the interned token");
    CHECK(pPlain->GenericTypeArgs().size() == 1
        && pPlain->GenericTypeArgs()[0]->Kind() == NK_Int32,
        "plain variant's type arg is the int field");
    //Display name renders the token ("Int32[]") — diagnostic readability.
    CHECK(pArrayOf->Name().find("Int32[]") != std::string::npos,
        "display name renders the array token, got: " + pArrayOf->Name());
    PASS();
}

//Dict.keys() re-cast: the inferred List<K> must carry the SAME interned
//token as the Dict's key slot — the per-TU intern table guarantees one
//token per element type, so a distinct instance would mean the re-cast
//minted a fresh type identity and the instantiation keys would diverge.
static void test_dict_keys_recast_carries_token()
{
    TEST(dict_keys_recast_carries_token);
    auto out = compileOne(
        "int main() {\n"
        "    Dict<int[],int> d = new Dict<int[],int>();\n"
        "    List<int[]> ks = d.keys();\n"
        "    return 0;\n"
        "}\n");
    CHECK(out.ok, "build failed: "
        + (out.logger ? joinErrors(*out.logger) : std::string("?")));
    const auto& root = out.builder->TreeRootView();
    auto exprs = collectGenericTypeExprs(root);
    CHECK(exprs.size() == 3, "expected 3 generic type exprs (Dict decl "
        "+ Dict new + List decl), got " + std::to_string(exprs.size()));
    //The keys() member call's EvalDataType is the re-cast List<K>.
    SnClassDecl* pKeys = nullptr;
    std::function<void(const SyntaxNode&)> findKeys =
        [&](const SyntaxNode& node) {
            if (node.Kind() == NK_MemberExpr
                && node.ToString().find("keys") != std::string::npos) {
                auto* pField = static_cast<const SnExpression&>(node)
                    .EvalDataType();
                if (pField && pField->Kind() == NK_ClassDecl)
                    pKeys = static_cast<SnClassDecl*>(pField);
            }
            const auto& children = node.Children();
            for (auto it = children.cbegin(); it != children.cend(); ++it)
                findKeys(static_cast<const SyntaxNode&>(*it));
        };
    findKeys(root);
    CHECK(pKeys, "keys() call resolved to a synthetic List class");
    //The Dict instantiation shares one cache entry between decl and new;
    //either Dict expr yields the same synthetic class.
    SnClassDecl* pDict = nullptr;
    for (auto* pExpr : exprs) {
        if (!pExpr->TypeArgs().empty()
            && pExpr->TypeArgs()[0]
            && pExpr->TypeArgs()[0]->Kind() == NK_ArrayTypeExpr)
            pDict = instantiationOf(*pExpr);
    }
    CHECK(pDict, "Dict<int[],int> instantiation found");
    CHECK(pKeys->GenericTypeArgs().size() == 1
        && pKeys->GenericTypeArgs()[0]->Kind() == NK_ArrayTypeToken,
        "re-cast List's key slot carries the interned token");
    CHECK(pKeys->GenericTypeArgs()[0] == pDict->GenericTypeArgs()[0],
        "re-cast List shares the Dict's interned key token (pointer "
        "identity)");
    PASS();
}

int main()
{
    Runtime::StaticInit();
    test_generic_array_flags_split_keys();
    test_dict_keys_recast_carries_token();
    test_gc_list_arm_traces_array();
    test_gc_dict_arm_traces_array();
    std::cerr << "\narray_flags: " << g_pass << " passed, "
        << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
