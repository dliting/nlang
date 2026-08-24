// --- AST container model tests (Phase 13 Step 0.5) ---
// Pins the dual-storage invariant for the three former out-of-list
// exception slots: every contained member is a real child (ctor AddChild,
// Parent() set, ReplaceChildNode splices the list) and the typed accessor
// stays in sync. Fixtures use SnIdentifierExpr only: it touches no
// runtime singletons (SnLiteralExpr would pull RnInt32::Instance()).
#include "nlang/compiler/SnExpressions.h"
#include "nlang/compiler/SnStatements.h"
#include "nlang/compiler/ScriptLocation.h"

#include <cstdio>
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
    return nlang::ScriptLocation();
}

static nlang::SnIdentifierExpr *MakeIdent(const char *zName)
{
    return new nlang::SnIdentifierExpr(new std::string(zName), Loc());
}

//SnNewArrayExpr: element type AND size are regular children.
static void TestNewArrayContainment()
{
    auto *pElemType = MakeIdent("int");
    auto *pSize = MakeIdent("n");
    nlang::SnNewArrayExpr expr(pElemType, pSize, Loc());

    CHECK(expr.ElementType() == pElemType);
    CHECK(expr.Size() == pSize);
    CHECK(expr.Children().find(pElemType) != expr.Children().end());
    CHECK(expr.Children().find(pSize) != expr.Children().end());
    CHECK(pElemType->Parent() == &expr);
    CHECK(pSize->Parent() == &expr);

    //Splice routing: replacing a slot must keep accessor and list in sync.
    auto *pNewType = MakeIdent("float");
    CHECK(expr.ReplaceChildNode(pElemType, pNewType));
    CHECK(expr.ElementType() == pNewType);
    CHECK(pNewType->Parent() == &expr);
}

//SnSubscriptExpr: array base AND index are regular children.
static void TestSubscriptContainment()
{
    auto *pArray = MakeIdent("a");
    auto *pIndex = MakeIdent("i");
    nlang::SnSubscriptExpr expr(pArray, pIndex, Loc());

    CHECK(expr.Array() == pArray);
    CHECK(expr.Index() == pIndex);
    CHECK(expr.Children().find(pArray) != expr.Children().end());
    CHECK(expr.Children().find(pIndex) != expr.Children().end());
    CHECK(pArray->Parent() == &expr);
    CHECK(pIndex->Parent() == &expr);

    auto *pNewIndex = MakeIdent("j");
    CHECK(expr.ReplaceChildNode(pIndex, pNewIndex));
    CHECK(expr.Index() == pNewIndex);
    CHECK(pNewIndex->Parent() == &expr);
}

//SnLocalDeclStmt: init expressions are regular children until the
//resolver's decomposition detaches them.
static void TestLocalDeclContainment()
{
    auto *pType = MakeIdent("int");
    auto *pInit = MakeIdent("v");
    auto *pDecls = new std::vector<nlang::SnLocalDeclStmt::LocalDecl>();
    pDecls->push_back({std::string("x"), pInit});
    nlang::SnLocalDeclStmt stmt(pType, pDecls, Loc());

    CHECK(stmt.Type() == pType);
    CHECK(stmt.Decls()[0].pInitExpr == pInit);
    CHECK(stmt.Children().find(pType) != stmt.Children().end());
    CHECK(stmt.Children().find(pInit) != stmt.Children().end());
    CHECK(pInit->Parent() == &stmt);

    auto *pNewInit = MakeIdent("w");
    CHECK(stmt.ReplaceChildNode(pInit, pNewInit));
    CHECK(stmt.Decls()[0].pInitExpr == pNewInit);
    CHECK(pNewInit->Parent() == &stmt);
}

//SyntaxNode::DetachChild: DOM removeChild-style ownership handoff.
static void TestDetachChild()
{
    auto *pArray = MakeIdent("a");
    auto *pIndex = MakeIdent("i");
    nlang::SnSubscriptExpr expr(pArray, pIndex, Loc());

    //Detach: same pointer back, parent cleared, out of the children list.
    //The typed-pointer return pins the template overload (a plain
    //SyntaxNode* return would not convert down to SnExpression*).
    nlang::SnExpression *pDetached = expr.DetachChild(pIndex);
    CHECK(pDetached == pIndex);
    CHECK(pIndex->Parent() == nullptr);
    CHECK(expr.Children().find(pIndex) == expr.Children().end());
    //The typed slot is NOT cleared — alias semantics; the old owner's
    //accessor stays valid so existing readers keep working until the
    //caller re-parents the node.
    CHECK(expr.Index() == pIndex);

    //A foreign node (not a child) is rejected with null, list untouched.
    auto *pForeign = MakeIdent("z");
    CHECK(expr.DetachChild(pForeign) == nullptr);
    CHECK(expr.Children().find(pArray) != expr.Children().end());

    //The detached node is caller-owned now: re-parent or delete it.
    delete pIndex;
    delete pForeign;
}

int main()
{
    TestNewArrayContainment();
    TestSubscriptContainment();
    TestLocalDeclContainment();
    TestDetachChild();
    if (g_failures == 0)
        std::printf("ast_containment: all passed\n");
    else
        std::printf("ast_containment: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
