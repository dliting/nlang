/*--- SnExpressionsInvoke.cpp — the SnInvokeExpr node family: the
    name and postfix-callee constructors, traversal and rendering. Split
    from SnExpressions.cpp by the source-size guard (v0.13.13a), zero
    behavior change; family precedent SnExpressionsLambda.cpp.
---*/

#include "SnExpressions.h"
#include "SyntaxNodeVisitor.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include <nlang/runtime/RnData.h>
#include <nlang/runtime/NodeContainers.h>
#include <iosfwd>
#include <sstream>
#include <nlang/runtime/StackDump.h>

namespace nlang

{

SnInvokeExpr::SnInvokeExpr(std::string *pCalleeName,
	UniquePtrList<SnExpression> upParams, const ISourceLocation &loc) :
	Super_(NK_InvokeExpr, loc), m_upCalleeName(pCalleeName),
	m_pParams(CreateChildList<SnInvokeParamList>(upParams, this))
{
	NLANG_ASSERT(m_pParams && pCalleeName);
}

//v0.13.13a D-3: the postfix-call shape — the callee is a child
//expression, wired after the arguments (Children() traversal order is
//not evaluation order; codegen stages the args first, then evaluates
//the callee into the delegate slot, matching the named delegate
//shape's handle staging). The callee name is an empty string, not
//null: CalleeName() readers (diagnostics, container-get probes)
//dereference it unconditionally.
SnInvokeExpr::SnInvokeExpr(SnExpression *pCallee,
	UniquePtrList<SnExpression> upParams, const ISourceLocation &loc) :
	Super_(NK_InvokeExpr, loc), m_upCalleeName(new std::string()),
	m_pParams(CreateChildList<SnInvokeParamList>(upParams, this))
{
	NLANG_ASSERT(m_pParams && pCallee);
	m_pCalleeExpr = pCallee;
	AddChild(m_pCalleeExpr);
}

SnInvokeExpr::~SnInvokeExpr()
{
	// m_upCalleeName is now unique_ptr - auto-deleted
}

void SnInvokeExpr::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnInvokeExpr::ToString() const
{
	std::stringstream ss;
	//v0.13.13a D-3: the postfix-call shape has no callee name — the
	//expression renders parenthesized in the name's place.
	if (m_pCalleeExpr)
		ss << "(" << m_pCalleeExpr->ToString() << ")";
	else
		ss << *m_upCalleeName;
	ss << "(";

	auto iEnd = Params().cend();
	auto iBegin = Params().cbegin();
	for (auto iParam = iBegin; iParam != iEnd; ++iParam)
	{
		if (iParam != iBegin)
			ss << ",";
		ss << iParam->ToString();
	}

	ss << ")";
	return ss.str();
}

} //namespace nlang
