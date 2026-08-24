/*-----------------------------------------------------------------------------
	ncomp/builder/AliasExpander.hpp
	This file implements the Phase 13 TU-level type alias pre-pass: it
registers the `using Name = Type;` directives of one translation unit and
expands every alias use site by splicing a deep clone of the aliased type
expression into the parent slot.

Pipeline position (pinned): after ParseTransUnits and before
MergeTransUnits/LoadImports. Alias scope is the translation unit — after
the merge the unit roots are cleared and the boundaries are gone, so the
per-unit table could no longer be applied correctly. At this point the
AST root still holds only the built-in types, which keeps the clash check
(see DuplicateFieldChecker::CheckUnitAliases) TU-local by construction.

The alias target expressions stay owned by their SnUsing nodes; the table
in TranslationUnit holds non-owning pointers and is only read here.
-----------------------------------------------------------------------------*/
#pragma once
#include "BuildEnvironment.h"
#include "TranslationUnit.h"
#include "SnExtraTypes.h"
#include "ScriptLocation.h"
#include <set>
#include <string>
#include <vector>

namespace nlang
{

class AliasExpander
{
public:
	explicit AliasExpander(BuildEnvironment &env) : m_Env(env), m_pUnit(nullptr)
	{
	}

	/*
	Register and expand all alias-form usings of one translation unit.
	Registration runs in text order: an alias whose target references an
	alias that appears later in the file (or forms a cycle) is diagnosed
	here with the regular unknown-type message and the unit's expansion
	is skipped — Build() aborts on the pending error anyway.
	*/
	void ProcessUnit(TranslationUnit &unit)
	{
		auto pUsings = unit.Usings();
		if (!pUsings)
			return;

		//Roster pre-scan: every alias name of this unit, text order of the
		//directives is iterated separately below.
		std::set<std::string> roster;
		size_t nAliasCount = 0;
		for (auto pUsing : *pUsings)
		{
			if (!pUsing->IsAlias())
				continue;
			roster.insert(pUsing->AliasName());
			++nAliasCount;
		}
		if (nAliasCount == 0)
			return;  //nothing to expand — do not touch the tree at all

		bool bErrored = false;
		for (auto pUsing : *pUsings)
		{
			if (!pUsing->IsAlias())
				continue;
			CheckRegistrationOrder(*pUsing, roster, unit, bErrored);
			//Register even after a cycle diagnostic: the visited-set of the
			//expansion walk is the actual recursion guard, and Build()
			//aborts on the logged error regardless.
			unit.SetAlias(pUsing->AliasName(), pUsing->AliasType());
		}
		if (bErrored || !unit.Root())
			return;

		m_pUnit = &unit;
		std::set<std::string> visitedNames;
		ExpandChildren(unit.Root(), visitedNames);
		m_pUnit = nullptr;
	}

private:
	//Diagnose a target that names an alias which is not yet registered:
	//either a forward reference to a later alias or an alias cycle. The
	//message reuses the regular unknown-type text so the diagnostic reads
	//exactly like a use site that cannot be resolved.
	void CheckRegistrationOrder(SnUsing &usingNode,
		const std::set<std::string> &roster, TranslationUnit &unit,
		bool &bErrored)
	{
		auto *pRootId = RootIdentifier(*usingNode.AliasType());
		if (!pRootId || pRootId->Field())
			return;  //no root name, or a pre-resolved built-in type
		const auto &sName = pRootId->Name();
		if (roster.count(sName) != 0 && unit.FindAlias(sName) == nullptr)
		{
			m_Env.Log(CLL_Error, "Cannot resolve the field: %s.",
				sName.c_str());
			bErrored = true;
		}
	}

	//Peel array/generic wrappers down to the root identifier of a type
	//expression (e.g. Dict<string,int[]> -> "Dict"). Null for shapes the
	//grammar does not produce in alias targets (defensive).
	static SnIdentifierExpr *RootIdentifier(SnFieldExpr &typeExpr)
	{
		auto *pCur = &typeExpr;
		for (;;)
		{
			switch (pCur->Kind())
			{
			case NK_NameExpr:
				pCur = static_cast<SnNameExpr*>(pCur)->Expr();
				break;
			case NK_ArrayTypeExpr:
				pCur = static_cast<SnArrayTypeExpr*>(pCur)->ElementType();
				break;
			case NK_GenericTypeExpr:
				pCur = static_cast<SnGenericTypeExpr*>(pCur)->Base();
				break;
			case NK_IdentifierExpr:
				return static_cast<SnIdentifierExpr*>(pCur);
			default:
				return nullptr;
			}
			if (!pCur)
				return nullptr;
		}
	}

	//The identifier name inside a name expression, or an empty string for
	//non-identifier inners (the grammar only produces identifier inners).
	static std::string NameOf(SnNameExpr &nameExpr)
	{
		auto *pInner = nameExpr.Expr();
		if (!pInner || pInner->Kind() != NK_IdentifierExpr)
			return std::string();
		return static_cast<SnIdentifierExpr*>(pInner)->Name();
	}

	//Expand the alias references among the direct children of one node.
	void ExpandChildren(SyntaxNode *pParent, std::set<std::string> &visitedNames)
	{
		//Since the Phase 13 Step 0.5 container unification every
		//contained member is a regular child — including the former
		//out-of-list slots (new-array element type/size, subscript
		//base/index, local-decl init expressions) that this walk used to
		//special-case — so one loop covers them all. Snapshot first:
		//ReplaceChildNode splices the children list while the loop runs
		//(the replaced node is deleted in place).
		std::vector<SyntaxNode*> children;
		for (auto &child : pParent->Children())
			children.push_back(static_cast<SyntaxNode*>(&child));
		for (auto *pChild : children)
			ExpandSlot(pParent, pChild, visitedNames);
	}

	//Try to expand one child node as an alias reference; otherwise just
	//recurse into it.
	void ExpandSlot(SyntaxNode *pParent, SyntaxNode *pChild,
		std::set<std::string> &visitedNames)
	{
		if (pChild->Kind() == NK_NameExpr)
		{
			auto &nameNode = static_cast<SnNameExpr&>(*pChild);
			const auto sName = NameOf(nameNode);
			auto *pTarget = sName.empty() ? nullptr : m_pUnit->FindAlias(sName);
			if (pTarget)
			{
				//Alias cycles terminate here: a name repeated on the current
				//expansion path is left as a plain name expression and fails
				//at resolve time with the regular unknown-type diagnostic.
				if (visitedNames.count(sName) != 0)
					return;
				auto *pClone = CloneTypeNode(*pTarget, nameNode);
				if (pClone)
				{
					visitedNames.insert(sName);
					pParent->ReplaceChildNode(pChild, pClone);
					//Chained aliases: the CLONE itself may name another
					//alias (e.g. `using A = B;` with `using B = int;`),
					//so it goes through the same slot check, not just a
					//descend into its children.
					ExpandSlot(pParent, pClone, visitedNames);
					visitedNames.erase(sName);
					return;
				}
			}
		}
		ExpandChildren(pChild, visitedNames);
	}

	//Deep-clone a type expression (identifier / name / array / generic).
	//Node flags and source locations are copied along: built-in identifiers
	//carry NF_Resolved, and Usings() resolves through Location()->TransUnit().
	//Null only for grammar-impossible shapes (defensive — the caller then
	//leaves the original name expression untouched).
	static SnFieldExpr *CloneTypeNode(SnFieldExpr &src, SyntaxNode &useSite)
	{
		const ISourceLocation &loc = LocationOf(src, useSite);
		switch (src.Kind())
		{
		case NK_IdentifierExpr:
		{
			auto &idExpr = static_cast<SnIdentifierExpr&>(src);
			//Pre-resolved built-in identifiers are rebuilt through their
			//field kind so the clone is resolved exactly like the source.
			auto *pClone = idExpr.Field()
				? new SnIdentifierExpr(idExpr.Field()->Kind(), loc)
				: new SnIdentifierExpr(new std::string(idExpr.Name()), loc);
			pClone->Flags(idExpr.Flags());
			return pClone;
		}
		case NK_NameExpr:
		{
			auto &nameExpr = static_cast<SnNameExpr&>(src);
			auto *pInner = nameExpr.Expr();
			if (!pInner || pInner->Kind() != NK_IdentifierExpr)
				return nullptr;
			auto *pInnerClone = CloneTypeNode(*pInner, useSite);
			if (!pInnerClone)
				return nullptr;
			auto *pClone = new SnNameExpr(
				static_cast<SnIdentifierExpr*>(pInnerClone), loc);
			pClone->Flags(nameExpr.Flags());
			return pClone;
		}
		case NK_ArrayTypeExpr:
		{
			auto &arrExpr = static_cast<SnArrayTypeExpr&>(src);
			auto *pElemClone = CloneTypeNode(*arrExpr.ElementType(), useSite);
			if (!pElemClone)
				return nullptr;
			auto *pClone = new SnArrayTypeExpr(pElemClone, loc);
			pClone->Flags(arrExpr.Flags());
			return pClone;
		}
		case NK_GenericTypeExpr:
		{
			auto &genExpr = static_cast<SnGenericTypeExpr&>(src);
			auto *pBaseClone = CloneTypeNode(*genExpr.Base(), useSite);
			if (!pBaseClone)
				return nullptr;
			auto *pTypeArgs = new std::vector<SnFieldExpr*>();
			for (auto *pArg : genExpr.TypeArgs())
			{
				auto *pArgClone = CloneTypeNode(*pArg, useSite);
				if (!pArgClone)
				{
					delete pBaseClone;
					for (auto *pMade : *pTypeArgs)
						delete pMade;
					delete pTypeArgs;
					return nullptr;
				}
				pTypeArgs->push_back(pArgClone);
			}
			auto *pClone = new SnGenericTypeExpr(pBaseClone, pTypeArgs, loc);
			pClone->Flags(genExpr.Flags());
			return pClone;
		}
		default:
			return nullptr;
		}
	}

	static const ISourceLocation &LocationOf(SyntaxNode &node,
		SyntaxNode &fallback)
	{
		if (node.Location())
			return *node.Location();
		if (fallback.Location())
			return *fallback.Location();
		//Grammar-produced nodes always carry a location; this default only
		//guards against synthesized nodes without one.
		static ScriptLocation s_DefaultLoc;
		return s_DefaultLoc;
	}

	BuildEnvironment &m_Env;
	TranslationUnit *m_pUnit;
};

} //namespace nlang
