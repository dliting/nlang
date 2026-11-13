/*---
    SnChildSync.cpp — v0.13.7: the ReplaceChildNode overrides for every
    AST class that caches a typed expression pointer able to hold a
    tensor-lowered binary (the dual-storage sync contract,
    SyntaxNode.h:118-131 — "Node classes that cache a typed member
    pointer to a child MUST override this"). The audit table behind
    this file lives in the M5 design appendix
    (docs/superpowers/specs/2026-11-12-m5-array-operator-lowering-
    design.md §4.2); every row here pairs with a minimal e2e case in
    tests/e2e/op_array_*.n. Classes whose existing overrides already
    sync their slots (SnSubscriptExpr, SnSliceItemExpr, SnCastExpr,
    SnNewArrayExpr, SnNewExpr, SnLocalDeclStmt, SnCatchClause, the type
    slots) are deliberately absent. SnSuperCallStmt's args are
    non-owning mirrors of the ctor declaration's children — a tensor
    binary there is unreachable by construction and caught by the
    TU-end sweep if that ever changes.
---*/
#include "SnExpressions.h"
#include "SnStatements.h"
#include "SyntaxTree.h"

namespace nlang
{

//--- statements --------------------------------------------------------------

bool SnReturnStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pResult == pOld)
	{
		ResetChild(m_pResult, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnAssignStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pRight == pOld)
	{
		ResetChild(m_pRight, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnCompoundAssignStmt::ReplaceChildNode(SyntaxNode *pOld,
	SyntaxNode *pNew)
{
	if (m_pRight == pOld)
	{
		ResetChild(m_pRight, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnAssertStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pCond == pOld)
	{
		ResetChild(m_pCond, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnSubscriptAssignStmt::ReplaceChildNode(SyntaxNode *pOld,
	SyntaxNode *pNew)
{
	//The index vector and the value slot are dual storage over the
	//children list (the SnSubscriptExpr indices precedent).
	for (auto *&pIndex : m_pIndices)
	{
		if (pIndex == pOld)
		{
			ResetChild(pIndex, static_cast<SnExpression *>(pNew));
			return true;
		}
	}
	if (m_pValue == pOld)
	{
		ResetChild(m_pValue, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnIfStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pCond == pOld)
	{
		ResetChild(m_pCond, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnWhileStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pCond == pOld)
	{
		ResetChild(m_pCond, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnDoStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pCond == pOld)
	{
		ResetChild(m_pCond, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnForStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	//The condition is the only expression slot (init/fini are
	//statements — a swap there happens inside the child statement).
	if (m_pCond == pOld)
	{
		ResetChild(m_pCond, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnCaseClause::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	for (auto *&pLabel : *m_upLabels)
	{
		if (pLabel == pOld)
		{
			ResetChild(pLabel, static_cast<SnExpression *>(pNew));
			return true;
		}
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnSwitchStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pCond == pOld)
	{
		ResetChild(m_pCond, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnThrowStmt::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pExpr == pOld)
	{
		ResetChild(m_pExpr, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

//--- expressions -------------------------------------------------------------

bool SnBinaryExpr::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	//Both operand slots — the nested-lowering channel (`a * 3 - b`):
	//the inner binary swaps while the outer one is mid-Access, so this
	//override is what keeps m_pLeft/m_pRight from dangling.
	if (m_pLeft == pOld)
	{
		ResetChild(m_pLeft, static_cast<SnExpression *>(pNew));
		return true;
	}
	if (m_pRight == pOld)
	{
		ResetChild(m_pRight, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnMemberExpr::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pOuter == pOld)
	{
		ResetChild(m_pOuter, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnAsExpr::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pTargetType == pOld)
	{
		ResetChild(m_pTargetType, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	//v0.13.7: the operand is an expression slot that can hold a
	//tensor-lowered binary (`x = (a + b) as int`).
	if (m_pOperand == pOld)
	{
		ResetChild(m_pOperand, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

bool SnInitListExpr::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	//The explicit type of "new Type{...}" is cached in a typed slot beside
	//the children list; keep it in sync when an alias expansion splices it.
	if (m_pExplicitType == pOld)
	{
		ResetChild(m_pExplicitType, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	//v0.13.7: the entries are dual storage too (`[a + b, c]`) — sync
	//the value/key slots beside the children list.
	for (auto &entry : m_entries)
	{
		if (entry.pValue == pOld)
		{
			ResetChild(entry.pValue, static_cast<SnExpression *>(pNew));
			return true;
		}
		if (entry.pKey == pOld)
		{
			ResetChild(entry.pKey, static_cast<SnExpression *>(pNew));
			return true;
		}
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

} //namespace nlang
