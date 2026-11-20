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
