/*---
    ExprResolverAlias.cpp — 表达式位的别名支持（0.9.0 创建孪生）。
    AliasExpander 预扫描只展开类型位的 NameExpr；`P[n]` / `P(args)` 的
    名字位于表达式位，到达解析器时仍是裸标识符，这里补齐同一套机制。
---*/
#include "ExprResolver.h"
#include "AliasExpander.hpp"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include <set>
#include <string>

namespace nlang
{

//TU alias table lookup following chains (`using A = B; using B = Point;`).
//A repeated name stops the walk — the pre-pass already diagnoses
//registration cycles; the visited set only keeps a malformed table from
//looping here. Null = the name addresses no alias of the use site's unit.
SnFieldExpr *ExprResolveAccessor::FindAliasTarget(const ISourceLocation *pLoc,
	const std::string &name)
{
	auto *pTransUnit = pLoc ? pLoc->TransUnit() : nullptr;
	if (!pTransUnit)
		return nullptr;
	std::set<std::string> visited;
	auto *pTarget = pTransUnit->FindAlias(name);
	while (pTarget)
	{
		auto *pRootId = AliasExpander::RootIdentifier(*pTarget);
		if (!pRootId || !visited.insert(pRootId->Name()).second)
			break;
		auto *pNext = pTransUnit->FindAlias(pRootId->Name());
		if (!pNext)
			break;  //chain end: pTarget names a real type
		pTarget = pNext;
	}
	return pTarget;
}

//0.9.0: resolve the subscript base, splicing an alias type in when the
//base names an alias (`P[2]`) — expression positions never reach the
//pre-pass. The splice runs BEFORE the regular resolve: the identifier
//resolve's miss path logs its failure text, so a post-miss repair would
//leave a stale diagnostic next to the twin's success. The splice probe
//is silent and value-first (a local shadowing the alias keeps the plain
//subscript reading); a real miss splices nothing and the regular
//resolve produces its one diagnostic as before. False = the base stays
//unresolved (the pending diagnostic is the caller's cue to stop).
bool ExprResolveAccessor::ResolveArrayBase(SnSubscriptExpr &sn)
{
	if (sn.Array()->Kind() == NK_IdentifierExpr)
	{
		TrySpliceAliasType(static_cast<SnIdentifierExpr &>(*sn.Array()));
		//The splice replaced the base in place; the Accept below
		//resolves the spliced type through the re-fetched reference.
	}
	sn.Array()->Accept(*m_pVisitor);
	return sn.Array()->IsResolved();
}

//0.9.0: splice the aliased type over an expression-position use site.
//A silent value probe keeps every resolvable name (local, field,
//function, class) on the regular channel — only a pure alias name pays
//the splice. The clone becomes tree-owned in ReplaceChildNode (the
//replaced node is deleted, so the caller must re-fetch its reference).
void ExprResolveAccessor::TrySpliceAliasType(SnIdentifierExpr &idExpr)
{
	const auto &name = idExpr.Name();
	if (FindFieldInAncestor(name, *m_pContext, *m_pAccessor, Flags()))
		return;
	if (IsBuiltinClassName(name))
		return;
	auto *pTarget = FindAliasTarget(idExpr.Location(), name);
	if (!pTarget)
		return;
	auto *pClone = AliasExpander::CloneTypeNode(*pTarget, idExpr);
	if (!pClone)
		return;
	idExpr.Parent()->ReplaceChildNode(&idExpr, pClone);
}

//0.9.0: the class or struct a ctor-invoke twin reaches through the TU
//alias table (`using P = Point;` then `P(args)`). Silent probe: null when
//the name addresses no alias, or the chain does not end at a constructible
//type — the caller keeps its own generic failure text as the single
//diagnostic. 0.9.11 D2: structs join classes behind the alias table.
SnField *ExprResolveAccessor::ProbeAliasType(const std::string &name,
	const ISourceLocation *pLoc)
{
	auto *pTarget = FindAliasTarget(pLoc, name);
	if (!pTarget)
		return nullptr;
	auto *pRootId = AliasExpander::RootIdentifier(*pTarget);
	if (!pRootId)
		return nullptr;  //no root identifier (defensive)
	const auto &rootName = pRootId->Name();
	if (IsBuiltinClassName(rootName))
		return GetBuiltinClassDecl(rootName, pLoc);
	if (pRootId->Field())
		return nullptr;  //a pre-resolved non-class built-in type
	auto *pField = FindFieldInAncestor(rootName, *m_pContext,
		*m_pAccessor, Flags());
	return (pField && (pField->Kind() == NK_ClassDecl
		|| pField->Kind() == NK_StructDecl))
		? pField : nullptr;
}

} //namespace nlang
