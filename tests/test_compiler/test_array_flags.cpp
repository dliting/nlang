/*---
test_array_flags.cpp - generic instance key array-flag unit tests.

In-process ModuleBuilder coverage of the C-period key split: generic
instantiations that differ only in the array-ness of a type argument
(List<int> vs List<int[]>) must mint distinct synthetic SnClassDecl
instances, and the mirror GenericArrayFlags() must carry the per-arg
array flag. Console-style suite (same shape as test_stdlib); the
e2e suite pins the end-to-end compile/run behavior.
---*/
#include <nlang/runtime/Runtime.h>
#include <nlang/compiler/ModuleBuilder.h>
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SyntaxNode.h>
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

//C-period hole 1: instantiations differing only in the array-ness of a
//type argument must NOT share the cached synthetic class (the erased
//key collapsed them), and the mirror must expose the flag.
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
    CHECK(pArrayOf->GenericArrayFlags().size() == 1
        && pArrayOf->GenericArrayFlags()[0] == 1,
        "mirror carries the array flag");
    CHECK(pPlain->GenericArrayFlags().size() == 1
        && pPlain->GenericArrayFlags()[0] == 0,
        "plain instantiation flag is 0");
    //Display name should render the array-ness (diagnostic readability).
    CHECK(pArrayOf->Name().find("[]") != std::string::npos,
        "display name renders array-ness, got: " + pArrayOf->Name());
    PASS();
}

//Dict.keys() re-cast (C-period consumption 6): the inferred List<K>
//must inherit the Dict's key-array flag, or the split key turns the
//today-working explicit `List<int[]> ks = d.keys()` into a reject.
static void test_dict_keys_recast_carries_array_flag()
{
    TEST(dict_keys_recast_carries_array_flag);
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
    CHECK(pKeys->GenericArrayFlags().size() == 1
        && pKeys->GenericArrayFlags()[0] == 1,
        "re-cast List carries the key array flag");
    PASS();
}

int main()
{
    Runtime::StaticInit();
    test_generic_array_flags_split_keys();
    test_dict_keys_recast_carries_array_flag();
    std::cerr << "\narray_flags: " << g_pass << " passed, "
        << g_fail << " failed\n";
    return g_fail == 0 ? 0 : 1;
}
