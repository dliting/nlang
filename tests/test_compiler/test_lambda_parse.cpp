// --- Lambda cover-grammar parse tests (lambda/closure plan Task 2) ---
// Pins the unified-prefix grammar split: `(expr)` stays a parenthesized
// expression, `... ) =>` routes to SnLambdaExpr, and the paren-group
// shapes that used to be syntax errors (`()`, `(a, b)`, `(int x)` without
// an arrow) are now rejected by the ParenPrefix semantic post-check
// instead of the grammar. Shape cases go through ParseFragment (parse
// only — the resolver has no SnLambdaExpr arm until Task 4); the reverse
// cases go through a full-unit scratch parse so the diagnostics fire in
// an initializer position.
#include "nlang/compiler/SnExpressions.h"
#include "nlang/compiler/SnStatements.h"
#include "nlang/compiler/ScriptParser.h"
#include "nlang/compiler/ScriptLocation.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
#include "nlang/compiler/TranslationUnit.h"
#include "nlang/compiler/SyntaxTree.h"
#include "nlang/runtime/Runtime.h"
#include "nlang/compiler/CastInfo.h"
#include "nlang/runtime/RnData.h"
#include "nlang/runtime/Module.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        ++g_failures; \
    } \
} while (0)

static nlang::ScriptLocation Loc()
{
    nlang::ScriptLocation loc;
    loc.m_nStartLine = 1;
    loc.m_nEndLine = 1;
    loc.m_nStartCol = 1;
    loc.m_nEndCol = 1;
    return loc;
}

static nlang::SnIdentifierExpr *MakeIdent(const char *zName)
{
    return new nlang::SnIdentifierExpr(new std::string(zName), Loc());
}

//Parses one expression span through the IDE-rescue fragment entry and
//hands back its single argument (the span is wrapped as one call
//argument, so exactly one arg = the span parsed as one expression).
static nlang::SnExpression *ParseOneSpan(const char *zSpan)
{
    std::vector<nlang::SnExpression *> args;
    if (!nlang::ScriptParser::ParseFragment(zSpan, Loc(), args))
    {
        std::printf("  (span did not parse: %s)\n", zSpan);
        for (auto *pArg : args)
            delete pArg;
        return nullptr;
    }
    if (args.size() != 1)
    {
        std::printf("  (span produced %zu args: %s)\n", args.size(), zSpan);
        for (auto *pArg : args)
            delete pArg;
        return nullptr;
    }
    return args[0];
}

//One full-unit parse against a scratch environment; errors() carries the
//collected diagnostics for the rejection assertions.
struct FullParse
{
    bool ok = false;
    std::vector<std::string> errors;
};

static FullParse ParseFullUnit(const char *zSource)
{
    FullParse outcome;
    nlang::BuildParams params;
    nlang::ListCompileLogger logger;
    nlang::BuildEnvironment env(params, logger);
    std::unique_ptr<nlang::Module> upModule(
        nlang::ModuleManager::CreateUnregistered("__lambda_parse_test"));
    if (!upModule)
    {
        outcome.errors.push_back("CreateUnregistered failed");
        return outcome;
    }
    //Capture the raw pointer before the move (ScriptParser.cpp's scratch
    //pattern): the moved-from unique_ptr must not be dereferenced.
    nlang::Module *pScratchModule = upModule.get();
    env.OwnModule(std::move(upModule));
    env.CurrModule(*pScratchModule);
    //ParseUnit asserts a current module; the primed global AST supplies
    //the builtin type nodes the parser resolves names against.
    nlang::ScriptParser parser(env);
    nlang::TranslationUnit unit("__lambda_parse_test.n");
    outcome.ok = parser.ParseUnit(unit, false, zSource) && !env.HasError();
    for (auto it = logger.cbegin(); it != logger.cend(); ++it)
        outcome.errors.push_back((*it)->FullMessage());
    return outcome;
}

static bool AnyErrorMentions(const FullParse &outcome, const char *zNeedle)
{
    for (const auto &msg : outcome.errors)
    {
        if (msg.find(zNeedle) != std::string::npos)
            return true;
    }
    return false;
}

//The lambda node: kind, param shapes, and body shape for the spec's
//surface forms (spec §3.1/D9).
static void TestLambdaShapes()
{
    //(x) => x*2 — one bare param folded from the identifier, expression
    //body. The bare param arrives as a raw identifier expression and the
    //arrow-confirmed reorganization folds it into the name slot.
    {
        nlang::SnExpression *pExpr = ParseOneSpan("(x) => x*2");
        CHECK(pExpr != nullptr);
        if (pExpr)
        {
            CHECK(pExpr->Kind() == nlang::NK_LambdaExpr);
            auto *pLambda = static_cast<nlang::SnLambdaExpr *>(pExpr);
            CHECK(pLambda->Params().size() == 1);
            if (pLambda->Params().size() == 1)
            {
                CHECK(pLambda->Params()[0].name == "x");
                CHECK(pLambda->Params()[0].pType == nullptr);
                CHECK(pLambda->Params()[0].pValueExpr == nullptr);
            }
            CHECK(pLambda->ExprBody() != nullptr);
            CHECK(pLambda->BlockBody() == nullptr);
            delete pExpr;
        }
    }
    //(x, y) => x+y — two bare params, order preserved.
    {
        nlang::SnExpression *pExpr = ParseOneSpan("(x, y) => x+y");
        CHECK(pExpr != nullptr);
        if (pExpr)
        {
            CHECK(pExpr->Kind() == nlang::NK_LambdaExpr);
            auto *pLambda = static_cast<nlang::SnLambdaExpr *>(pExpr);
            CHECK(pLambda->Params().size() == 2);
            if (pLambda->Params().size() == 2)
            {
                CHECK(pLambda->Params()[0].name == "x");
                CHECK(pLambda->Params()[1].name == "y");
            }
            delete pExpr;
        }
    }
    //(int x) => x — an explicit typed param.
    {
        nlang::SnExpression *pExpr = ParseOneSpan("(int x) => x");
        CHECK(pExpr != nullptr);
        if (pExpr)
        {
            CHECK(pExpr->Kind() == nlang::NK_LambdaExpr);
            auto *pLambda = static_cast<nlang::SnLambdaExpr *>(pExpr);
            CHECK(pLambda->Params().size() == 1);
            if (pLambda->Params().size() == 1)
            {
                CHECK(pLambda->Params()[0].name == "x");
                CHECK(pLambda->Params()[0].pType != nullptr);
                CHECK(pLambda->Params()[0].pValueExpr == nullptr);
            }
            delete pExpr;
        }
    }
    //(int& x) => x — the '&' mark rides the param type (spec §3.3).
    {
        nlang::SnExpression *pExpr = ParseOneSpan("(int& x) => x");
        CHECK(pExpr != nullptr);
        if (pExpr)
        {
            CHECK(pExpr->Kind() == nlang::NK_LambdaExpr);
            auto *pLambda = static_cast<nlang::SnLambdaExpr *>(pExpr);
            CHECK(pLambda->Params().size() == 1);
            if (pLambda->Params().size() == 1 && pLambda->Params()[0].pType)
                CHECK(pLambda->Params()[0].pType->ContainFlags(nlang::NF_Ref));
            delete pExpr;
        }
    }
    //(Point p) => p.x — a user-named type; ident-then-ident must route to
    //the typed-param reading (spec D9: only the TypeRef path survives).
    {
        nlang::SnExpression *pExpr = ParseOneSpan("(Point p) => p.x");
        CHECK(pExpr != nullptr);
        if (pExpr)
        {
            CHECK(pExpr->Kind() == nlang::NK_LambdaExpr);
            auto *pLambda = static_cast<nlang::SnLambdaExpr *>(pExpr);
            CHECK(pLambda->Params().size() == 1);
            if (pLambda->Params().size() == 1)
            {
                CHECK(pLambda->Params()[0].name == "p");
                CHECK(pLambda->Params()[0].pType != nullptr);
            }
            delete pExpr;
        }
    }
    //() => 1 — empty param list, expression body.
    {
        nlang::SnExpression *pExpr = ParseOneSpan("() => 1");
        CHECK(pExpr != nullptr);
        if (pExpr)
        {
            CHECK(pExpr->Kind() == nlang::NK_LambdaExpr);
            auto *pLambda = static_cast<nlang::SnLambdaExpr *>(pExpr);
            CHECK(pLambda->Params().empty());
            CHECK(pLambda->ExprBody() != nullptr);
            CHECK(pLambda->BlockBody() == nullptr);
            delete pExpr;
        }
    }
    //(x+y) => x — an Expression-arm param reaches the node as a raw
    //expression (name empty); Task 3's parameter gate rejects this shape,
    //Task 2 only pins that it parses through.
    {
        nlang::SnExpression *pExpr = ParseOneSpan("(x+y) => x");
        CHECK(pExpr != nullptr);
        if (pExpr)
        {
            CHECK(pExpr->Kind() == nlang::NK_LambdaExpr);
            auto *pLambda = static_cast<nlang::SnLambdaExpr *>(pExpr);
            CHECK(pLambda->Params().size() == 1);
            if (pLambda->Params().size() == 1)
            {
                CHECK(pLambda->Params()[0].name.empty());
                CHECK(pLambda->Params()[0].pValueExpr != nullptr);
            }
            delete pExpr;
        }
    }
    //(x) => { return x; } — block body (Paragraph), no expression body.
    {
        nlang::SnExpression *pExpr = ParseOneSpan("(x) => { return x; }");
        CHECK(pExpr != nullptr);
        if (pExpr)
        {
            CHECK(pExpr->Kind() == nlang::NK_LambdaExpr);
            auto *pLambda = static_cast<nlang::SnLambdaExpr *>(pExpr);
            CHECK(pLambda->Params().size() == 1);
            CHECK(pLambda->ExprBody() == nullptr);
            CHECK(pLambda->BlockBody() != nullptr);
            delete pExpr;
        }
    }
}

//The non-lambda分流: `(expr)` keeps today's parenthesized-expression
//semantics — the group post-check hands back the single inner expression.
static void TestParenthesizedStillWorks()
{
    nlang::SnExpression *pExpr = ParseOneSpan("(x)");
    CHECK(pExpr != nullptr);
    if (pExpr)
    {
        CHECK(pExpr->Kind() == nlang::NK_IdentifierExpr);
        auto *pIdent = static_cast<nlang::SnIdentifierExpr *>(pExpr);
        CHECK(pIdent->Name() == "x");
        delete pExpr;
    }
}

//The group shapes that used to be syntax errors are now rejected by the
//ParenPrefix post-check in a real initializer position.
static void TestGroupRejections()
{
    //Empty parenthesized group.
    {
        FullParse outcome = ParseFullUnit(
            "int main() { int e = (); return 0; }\n");
        CHECK(!outcome.ok);
        CHECK(AnyErrorMentions(outcome, "empty parenthesized group"));
    }
    //Multiple items without an arrow.
    {
        FullParse outcome = ParseFullUnit(
            "int main() { int a = 1; int b = 2; int p = (a, b); return 0; }\n");
        CHECK(!outcome.ok);
        CHECK(AnyErrorMentions(outcome, "multiple items"));
    }
    //A single typed item without an arrow.
    {
        FullParse outcome = ParseFullUnit(
            "int main() { int t = (int x); return 0; }\n");
        CHECK(!outcome.ok);
        CHECK(AnyErrorMentions(outcome, "type annotation"));
    }
}

//Direct-construction containment: every owned node (param types, raw
//param expressions, the body) is a real child; ReplaceChildNode keeps the
//typed slots and the children list in sync (dual-storage contract).
static void TestLambdaContainment()
{
    auto *pType = MakeIdent("int");
    auto *pRaw = MakeIdent("a+b");
    std::vector<nlang::LambdaParam> params;
    params.push_back({"x", pType, nullptr});
    params.push_back({"", nullptr, pRaw});
    auto *pBody = MakeIdent("x");
    nlang::SnLambdaExpr expr(std::move(params), pBody, nullptr, Loc());

    CHECK(expr.Params().size() == 2);
    CHECK(expr.Params()[0].name == "x");
    CHECK(expr.Params()[0].pType == pType);
    CHECK(expr.Params()[1].pValueExpr == pRaw);
    CHECK(expr.ExprBody() == pBody);
    CHECK(expr.BlockBody() == nullptr);
    CHECK(expr.Children().find(pType) != expr.Children().end());
    CHECK(expr.Children().find(pRaw) != expr.Children().end());
    CHECK(expr.Children().find(pBody) != expr.Children().end());
    CHECK(pType->Parent() == &expr);
    CHECK(pRaw->Parent() == &expr);
    CHECK(pBody->Parent() == &expr);
    CHECK(!expr.ToString().empty());

    //Slot splice: the body and the raw-expression param stay in sync.
    //ResetChild removes and DELETES the old child (SyntaxNode.h contract),
    //so the replaced nodes are touched no further.
    auto *pNewBody = MakeIdent("y");
    CHECK(expr.ReplaceChildNode(pBody, pNewBody));
    CHECK(expr.ExprBody() == pNewBody);
    CHECK(pNewBody->Parent() == &expr);
    CHECK(expr.Children().find(pBody) == expr.Children().end());

    auto *pNewRaw = MakeIdent("c");
    CHECK(expr.ReplaceChildNode(pRaw, pNewRaw));
    CHECK(expr.Params()[1].pValueExpr == pNewRaw);
    CHECK(pNewRaw->Parent() == &expr);
}

//The lambda body binds greedily: the body state's shift/reduce
//conflicts resolve to shift (nlang.y accepted-conflict ledger,
//2026-11-20), so every operator extends the body instead of truncating
//the lambda. Pins the resolution — a flip to reduce would fail here.
static void TestGreedyBody()
{
    nlang::SnExpression *pExpr = ParseOneSpan("(x) => x*2 + 3");
    CHECK(pExpr != nullptr);
    if (pExpr)
    {
        CHECK(pExpr->Kind() == nlang::NK_LambdaExpr);
        auto *pLambda = static_cast<nlang::SnLambdaExpr *>(pExpr);
        CHECK(pLambda->ExprBody() != nullptr);
        if (pLambda->ExprBody())
            CHECK(pLambda->ExprBody()->Kind() == nlang::NK_BinaryExpr);
        delete pExpr;
    }
}

int main()
{
    nlang::Runtime::StaticInit();
    nlang::TypeCastInfo::StaticInit();
    //Prime the global AST with the builtin type nodes, exactly like
    //ModuleBuilder::InitSyntaxTree before the parser runs: SnLiteralExpr
    //construction looks its type node up in TheAST, so a bare process
    //without this priming asserts on the first literal. The priming
    //environment lives for the whole process (leaked): the AST nodes it
    //resolves names for stay in the global tree after the call, and
    //production keeps the building environment alive alongside them.
    auto *pPrimingLogger = new nlang::ListCompileLogger();
    auto *pPrimingParams = new nlang::BuildParams();
    auto *pPrimingEnv = new nlang::BuildEnvironment(*pPrimingParams,
        *pPrimingLogger);
    nlang::TheAST().Clear();
    nlang::TheAST().BuildFromRuntime(*pPrimingEnv);
    std::fprintf(stderr, "priming ok\n");
    TestLambdaShapes();
    std::fprintf(stderr, "shapes ok\n");
    TestParenthesizedStillWorks();
    std::fprintf(stderr, "paren ok\n");
    TestGroupRejections();
    std::fprintf(stderr, "rejections ok\n");
    TestLambdaContainment();
    std::fprintf(stderr, "containment ok\n");
    TestGreedyBody();
    if (g_failures == 0)
        std::printf("lambda_parse: all passed\n");
    else
        std::printf("lambda_parse: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
