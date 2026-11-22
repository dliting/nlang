/*---
SnExpressionsLambda.cpp — the SnLambdaExpr node (lambda/closure plan
Task 2). Sibling of SnExpressions.cpp / SnExpressionsValues.cpp.
---*/
#include "nlang/compiler/SnExpressions.h"
#include "nlang/compiler/SnStatements.h"
#include "SyntaxNodeVisitor.h"

#include <sstream>

namespace nlang
{

SnLambdaExpr::SnLambdaExpr(std::vector<LambdaParam> params,
	SnExpression *pExprBody, SnParagraph *pBlockBody,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc),
	m_params(std::move(params)),
	m_pExprBody(pExprBody),
	m_pBlockBody(pBlockBody),
	m_pInferredTarget(nullptr)
{
	//Children() order: each param's type then raw item, then the body —
	//every owned node is a real child (dual-storage contract).
	for (auto &param : m_params)
	{
		if (param.pType)
			AddChild(param.pType);
		if (param.pValueExpr)
			AddChild(param.pValueExpr);
	}
	if (m_pExprBody)
		AddChild(m_pExprBody);
	if (m_pBlockBody)
		AddChild(m_pBlockBody);
}

SnLambdaExpr::~SnLambdaExpr()
{
}

SnLocalVar *SnLambdaExpr::AddParamLocal(const std::string &sName,
	SnField *pTypeField, const ISourceLocation &loc)
{
	auto upLocal = std::make_unique<SnLocalVar>(sName, pTypeField, loc);
	auto *pLocal = upLocal.get();
	m_upParamLocals.push_back(std::move(upLocal));
	m_paramLookup[sName] = pLocal;
	return pLocal;
}

SnField *SnLambdaExpr::FindParamLocal(const std::string &sName) const
{
	auto it = m_paramLookup.find(sName);
	return it != m_paramLookup.end() ? it->second : nullptr;
}

SnField *SnLambdaExpr::FindField(const std::string &sName) const
{
	//Lambda parameters answer before the parent chain — the body's
	//name lookup walks context → params → enclosing scopes, so a
	//parameter shadows a same-named outer local (the SnFunction
	//params-first discipline).
	if (auto *pLocal = FindParamLocal(sName))
		return pLocal;
	auto *pParent = Parent();
	return pParent ? pParent->FindField(sName) : nullptr;
}

SnExpression *SnLambdaExpr::DetachExprBody()
{
	//Capture lowering moves the body into the synthetic method. The
	//mirror is nulled beside the ownership list — a dangling
	//ExprBody()/BlockBody() behind a detached child would be a
	//landmine for any later walk or ToString.
	NLANG_ASSERT(m_pExprBody);
	SnExpression *pBody = DetachChild(m_pExprBody);
	m_pExprBody = nullptr;
	return pBody;
}

SnParagraph *SnLambdaExpr::DetachBlockBody()
{
	NLANG_ASSERT(m_pBlockBody);
	SnParagraph *pBody = DetachChild(m_pBlockBody);
	m_pBlockBody = nullptr;
	return pBody;
}

void SnLambdaExpr::AdoptBlockBody(SnParagraph *pBody)
{
	//Phase-0 normalization only: the expression body was detached (and
	//its mirror nulled) immediately before, so both slots must be free.
	NLANG_ASSERT(!m_pExprBody && !m_pBlockBody);
	AddChild(pBody);
	m_pBlockBody = pBody;
}

bool SnLambdaExpr::IsDataExpr() const
{
	//A lambda is a function value, not data.
	return false;
}

void SnLambdaExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnLambdaExpr::ToString() const
{
	std::stringstream ss;
	ss << "lambda(" << m_params.size() << " params)";
	return std::move(ss.str());
}

bool SnLambdaExpr::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	//The params and the body slots are dual storage (typed mirrors
	//beside the children list); splice both sides through ResetChild.
	for (auto &param : m_params)
	{
		if (param.pType == pOld)
		{
			ResetChild(param.pType, static_cast<SnFieldExpr *>(pNew));
			return true;
		}
		if (param.pValueExpr == pOld)
		{
			ResetChild(param.pValueExpr, static_cast<SnExpression *>(pNew));
			return true;
		}
	}
	if (m_pExprBody == pOld)
	{
		ResetChild(m_pExprBody, static_cast<SnExpression *>(pNew));
		return true;
	}
	if (m_pBlockBody == pOld)
	{
		ResetChild(m_pBlockBody, static_cast<SnParagraph *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

} //namespace nlang
