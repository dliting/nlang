/*-----------------------------------------------------------------------------
	SnExpressionsValues.cpp
	Value-shape expression syntax nodes: casts, arithmetic, allocations,
	subscripts and argument-marker forms. Split from SnExpressions.cpp
	(2026-09-27 maintainability refactor, zero behavior change).
-----------------------------------------------------------------------------*/

#include "SnExpressions.h"
#include "SyntaxNodeVisitor.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include <nlang/runtime/RnData.h>
#include <nlang/runtime/NodeContainers.h>
#include <iosfwd>
#include <sstream>

namespace nlang
{

SnCastExpr::SnCastExpr(SnExpression *pSource, TypeCastInfo &ci,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pSource(pSource), m_pTarget(ci.Target()),
	m_pTargetName(nullptr), m_CastKind(ci.Kind())
{
	assert(pSource);
	AddChild(m_pSource);
	AddFlags(NF_Resolved);
}

bool SnCastExpr::IsDataExpr() const
{
	return true;
}

void SnCastExpr::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnCastExpr::ToString() const
{
	assert(m_pSource);
	std::stringstream ss;
	ss << "cast<" << (m_pTarget ? m_pTarget->ToString() : "unknown type");
	ss << ">(" << m_pSource << ")";
	return std::move(ss.str());
}

//SnAsExpr (Phase 8e-1.5)

bool SnAsExpr::IsDataExpr() const
{
	return true;
}

void SnAsExpr::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnAsExpr::ToString() const
{
	std::stringstream ss;
	ss << "(";
	if (m_pOperand) ss << m_pOperand->ToString();
	else ss << "<null>";
	ss << " as ";
	if (m_pTargetType) ss << m_pTargetType->ToString();
	else if (m_pResolvedTarget) ss << m_pResolvedTarget->ToString();
	else ss << "<unknown>";
	ss << ")";
	return std::move(ss.str());
}

bool SnAsExpr::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pTargetType == pOld)
	{
		ResetChild(m_pTargetType, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

//SnBinaryExpr

SnBinaryExpr::SnBinaryExpr(Operator op, SnExpression *pLeft,
	SnExpression *pRight, const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_op(op), m_pLeft(pLeft), m_pRight(pRight)
{
	assert(m_pLeft);
	assert(m_pRight);
	AddChild(m_pLeft);
	AddChild(m_pRight);
}

SnBinaryExpr::SnBinaryExpr(Operator op, SnExpression *pLeft,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_op(op), m_pLeft(pLeft), m_pRight(nullptr)
{
	assert(m_pLeft);
	AddChild(m_pLeft);
}

bool SnBinaryExpr::IsDataExpr() const
{
	return true;
}

void SnBinaryExpr::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnBinaryExpr::ToString() const
{
	static const char* opNames[] = {
		"+", "-", "*", "/", "%",
		"<", "<=", ">", ">=",
		"==", "!=",
		"&&", "||",
		"-", "!",
	};
	std::stringstream ss;
	if (m_pRight)
		ss << m_pLeft->ToString() << " " << opNames[m_op]
		   << " " << m_pRight->ToString();
	else
		ss << opNames[m_op] << m_pLeft->ToString();
	return std::move(ss.str());
}

//--- SnNewExpr ---

SnNewExpr::SnNewExpr(SnFieldExpr *pClassName, UniquePtrList<SnExpression> upArgs,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pClassName(pClassName),
	m_pArgs(CreateChildNodes(upArgs, this)), m_pClassDecl(nullptr)
{
	assert(m_pClassName);
	AddChild(m_pClassName);
}

SnNewExpr::~SnNewExpr()
{
}

bool SnNewExpr::IsDataExpr() const
{
	return true;
}

void SnNewExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnNewExpr::ToString() const
{
	std::stringstream ss;
	ss << "new " << m_pClassName->ToString() << "()";
	return std::move(ss.str());
}

bool SnNewExpr::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pClassName == pOld)
	{
		ResetChild(m_pClassName, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

//--- SnThisExpr ---

SnThisExpr::SnThisExpr(const ISourceLocation &loc) :
	Super_(s_Kind, loc)
{
}

bool SnThisExpr::IsDataExpr() const
{
	return true;
}

void SnThisExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnThisExpr::ToString() const
{
	return "this";
}

SnSubscriptExpr::SnSubscriptExpr(SnExpression *pArray, SnExpression *pIndex,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pArray(pArray), m_pIndex(pIndex)
{
	//Phase 13 Step 0.5 container unification: both operands are regular
	//children (dual-storage invariant); the resolvers reach them through
	//the Array()/Index() accessors with direct Accepts.
	AddChild(m_pArray);
	AddChild(m_pIndex);
}

void SnSubscriptExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnSubscriptExpr::ToString() const
{
	return "subscript";
}

bool SnSubscriptExpr::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	//Both slots are cached typed members over the children list; route
	//splices through ResetChild to keep them in sync.
	if (m_pArray == pOld)
	{
		ResetChild(m_pArray, static_cast<SnExpression *>(pNew));
		return true;
	}
	if (m_pIndex == pOld)
	{
		ResetChild(m_pIndex, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

SnNewArrayExpr::SnNewArrayExpr(SnFieldExpr *pElemType, SnExpression *pSize,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pElemType(pElemType), m_pSize(pSize)
{
	//Phase 13 Step 0.5 container unification: the element type and size
	//are regular children (dual-storage invariant); the resolver reaches
	//them through ElementType()/Size() with direct Accepts.
	AddChild(m_pElemType);
	AddChild(m_pSize);
}

SnNewArrayExpr::~SnNewArrayExpr() = default;

bool SnNewArrayExpr::IsDataExpr() const
{
	return true;
}

void SnNewArrayExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnNewArrayExpr::ToString() const
{
	return "new_array";
}

bool SnNewArrayExpr::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	//Both slots are cached typed members over the children list; route
	//splices through ResetChild (the old pointer-swap form predates the
	//container unification and left Size() unreachable).
	if (m_pElemType == pOld)
	{
		ResetChild(m_pElemType, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	if (m_pSize == pOld)
	{
		ResetChild(m_pSize, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

//--- SnInitListExpr (Phase 8e-6) ---

SnInitListExpr::SnInitListExpr(SnFieldExpr *pExplicitType,
	std::vector<InitEntry> entries, bool isArrayForm,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc),
	m_pExplicitType(pExplicitType),
	m_entries(std::move(entries)),
	m_isArrayForm(isArrayForm),
	m_pInferredTarget(nullptr)
{
	if (m_pExplicitType)
		AddChild(m_pExplicitType);
	for (auto &e : m_entries)
		if (e.pValue)
			AddChild(e.pValue);
}

SnInitListExpr::~SnInitListExpr()
{
}

bool SnInitListExpr::IsDataExpr() const
{
	return true;
}

void SnInitListExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
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
	return Super_::ReplaceChildNode(pOld, pNew);
}

std::string SnInitListExpr::ToString() const
{
	std::stringstream ss;
	ss << (m_isArrayForm ? "init_list[" : "init_list{");
	ss << m_entries.size() << " entries";
	if (m_pExplicitType)
		ss << ", explicit=" << m_pExplicitType->ToString();
	ss << "}";
	return std::move(ss.str());
}

//--- SnNamedArgExpr (Phase 9c) ---

bool SnNamedArgExpr::IsDataExpr() const
{
	return m_pInner->IsDataExpr();
}

void SnNamedArgExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnNamedArgExpr::ToString() const
{
	std::stringstream ss;
	ss << *m_upName << " = " << m_pInner->ToString();
	return std::move(ss.str());
}

//--- SnOutArgExpr (Phase 9e) ---

bool SnOutArgExpr::IsDataExpr() const
{
	return m_pInner->IsDataExpr();
}

void SnOutArgExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnOutArgExpr::ToString() const
{
	std::stringstream ss;
	ss << "out " << m_pInner->ToString();
	return std::move(ss.str());
}

} //namespace nlang
