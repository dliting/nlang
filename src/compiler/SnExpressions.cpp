/*-----------------------------------------------------------------------------
	ncomp/intf/SnExpressions.h
	This file define the implementation of expression syntax nodes in an nlang 
AST.
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

SnExpression::SnExpression(NodeKind k) :
	Super_(k, FA_Public, NF_Expression), m_pEvalDataType(nullptr)
#ifdef NLANG_ENABLE_LLVM
	, m_pMetaValue(nullptr)
#endif
{

}

SnExpression::SnExpression(NodeKind k, const ISourceLocation &loc) :
	Super_(k, FA_Public, NF_Expression, loc), m_pEvalDataType(nullptr)
#ifdef NLANG_ENABLE_LLVM
	, m_pMetaValue(nullptr)
#endif
{
}

ImmutableNodeList * SnExpression::ChildrenPtr() const
{
	return ImmutableNodeList::NullList();
}

SnField * SnExpression::FindField(const std::string& sName) const
{
	return nullptr;
}

SnField * SnExpression::OwnerType() const
{
	auto pParent = Parent();
	while (pParent)
	{
		if (pParent->IsTypeField())
			return static_cast<SnField *>(pParent);
		if (pParent->Kind() == NK_Using)
			return TheAST().Root();
		pParent = pParent->Parent();
	}
	assert(false && "An expression has not outer type");
	return nullptr;
}

SnCompoundPlainExpr::SnCompoundPlainExpr(NodeKind k) :
	Super_(k), m_upChildren(new ImmutableNodeList())
{

}

SnCompoundPlainExpr::SnCompoundPlainExpr(NodeKind k, const ISourceLocation &loc) :
	Super_(k, loc), m_upChildren(new ImmutableNodeList())
{
}

SnCompoundPlainExpr::~SnCompoundPlainExpr()
{
	// m_upChildren is now unique_ptr - auto-deleted
}

ImmutableNodeList * SnCompoundPlainExpr::ChildrenPtr() const
{
	return m_upChildren.get();
}

void SnLiteralExpr::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

SnFieldExpr::SnFieldExpr(NodeKind k) : 
	Super_(k), m_pField(nullptr), m_pChecker(nullptr)
{
}

SnFieldExpr::SnFieldExpr(NodeKind k, const ISourceLocation & loc) :
	Super_(k, loc), m_pField(nullptr), m_pChecker(nullptr)
{
}

SnFieldExpr::FieldChecker& SnFieldExpr::DataTypeChecker()
{
	static SnFieldExpr::FieldChecker checker =
		[](SnFieldExpr& path, BuildEnvironment& env)->bool
	{
		auto pField = path.Field();
		assert(pField);
		if (pField->IsTypeField())
			return true;
		env.Log(CLL_Error, path.Location(),
			"The field \"%s\" is not a valid data type.",
			pField->ToString().c_str());
		env.Log(CLL_More, pField->Location(),
			"See also the declaration of \"%s\".",
			pField->ToString().c_str());
		return false;
	};
	return checker;
}

bool SnFieldExpr::IsDataExpr() const
{
	assert(m_pField);
	return m_pField->IsDataField();
}

SnCompoundFieldExpr::SnCompoundFieldExpr(NodeKind k) :
	Super_(k), m_upChildren(new ImmutableNodeList())
{

}

SnCompoundFieldExpr::SnCompoundFieldExpr(NodeKind k,
	const ISourceLocation &loc) :
	Super_(k, loc), m_upChildren(new ImmutableNodeList())
{
}

SnCompoundFieldExpr::~SnCompoundFieldExpr()
{
	// m_upChildren is now unique_ptr - auto-deleted
}

ImmutableNodeList * SnCompoundFieldExpr::ChildrenPtr() const
{
	return m_upChildren.get();
}

std::string SnLiteralExpr::ToString() const
{
	return m_Value.ToString();
}

SnLiteralExpr::SnLiteralExpr(RnDataType &rtti, void *pValue) :
	Super_(s_Kind), m_Value(rtti, pValue)
{
	EvalByRTTI(rtti);
}

SnLiteralExpr::~SnLiteralExpr()
{
	if (m_Value.Type()->IsReference()) 
	{
		//Non-value type should be deleted.
		void *pData = &m_Value.Data();
		m_Value.Type()->DestroyValue(pData);
	}
}

void SnLiteralExpr::EvalByRTTI(RnDataType &rtti)
{
	auto pTypeNode = TheAST().FindNode(rtti);
	assert(pTypeNode && pTypeNode->IsTypeField());
	EvalDataType(static_cast<SnField*>(pTypeNode));
	AddFlags(NF_Resolved);
}

bool SnLiteralExpr::IsDataExpr() const
{
	return true;
}

SnIdentifierExpr::SnIdentifierExpr(NodeKind builtinKind,
	const ISourceLocation &loc) : 
	Super_(s_Kind, loc)
{
	assert(IsBuiltinType(builtinKind));
	m_pField = SnBuiltinDataType::InstanceOf(builtinKind);
	EvalDataType(SnType::Instance());
	AddFlags(NF_Resolved);
}

SnIdentifierExpr::SnIdentifierExpr(std::string *pName,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_upName(pName)
{
	assert(m_upName);
}

SnIdentifierExpr::~SnIdentifierExpr()
{
	// m_upName is now unique_ptr - auto-deleted
}

void SnIdentifierExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnIdentifierExpr::ToString() const
{
	return Name();
}

SnInvokeExpr::SnInvokeExpr(std::string *pCalleeName,
	UniquePtrList<SnExpression> upParams, const ISourceLocation &loc) :
	Super_(NK_InvokeExpr, loc), m_upCalleeName(pCalleeName),
	m_pParams(CreateChildNodes(upParams, this))
{
	assert(m_pParams && pCalleeName);
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
	ss << *m_upCalleeName << "(";

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

SnMemberExpr::SnMemberExpr(SnExpression *pOuter, SnIdentifierExpr *pInner, 
	const ISourceLocation &loc) : 
	Super_(s_Kind, loc), m_pOuter(pOuter), m_pInner(pInner)
{
	Init();
}

SnMemberExpr::SnMemberExpr(SnExpression *pOuter, SnInvokeExpr *pInner,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pOuter(pOuter), m_pInner(pInner)
{
	Init();
}

void SnMemberExpr::Init()
{
	assert(m_pOuter && m_pInner);
	AddChild(m_pOuter);
	AddChild(m_pInner);
}

bool SnMemberExpr::IsNameExpr() const
{
	if (m_pInner->Kind() != NK_IdentifierExpr)
		return false;
	switch (m_pOuter->Kind())
	{
	case NK_IdentifierExpr:
		return true;
	case NK_MemberExpr:
		return static_cast<const SnMemberExpr&>(*m_pOuter).IsNameExpr();
	default:
		return false;
	}
}

void SnMemberExpr::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnMemberExpr::ToString() const
{
	std::stringstream ss;
	ss << m_pOuter->ToString() << '.' << m_pInner->ToString();
	return std::move(ss.str());
}

SnFieldExpr::FieldChecker& SnMemberExpr::NameExprChecker()
{
	static FieldChecker checker =
		[](SnFieldExpr& sn, BuildEnvironment& env)->bool
	{
		assert(sn.Kind() == NK_MemberExpr);
		auto& memberExpr = static_cast<SnMemberExpr &>(sn);
		if (!memberExpr.IsNameExpr())
		{
			env.Log(CLL_Error, sn.Location(), "Invalid field name: %s.",
				sn.ToString().c_str());
			return false;
		}
		return true;
	};
	return checker;
}

SnFieldExpr::FieldChecker &SnMemberExpr::InvokeExprChecker()
{
	static FieldChecker checker =
		[](SnFieldExpr& sn, BuildEnvironment& env)->bool
	{
		assert(sn.Kind() == NK_MemberExpr);
		auto& memberExpr = static_cast<SnMemberExpr &>(sn);
		assert(memberExpr.Inner());
		if (memberExpr.Inner()->Kind() != NK_InvokeExpr)
		{
			env.Log(CLL_Error, sn.Location(), "Invalid invoke expression: %s.",
				sn.ToString().c_str());
			return false;
		}
		return true;
	};
	return checker;
}

SnNameExpr::SnNameExpr(RnField &rn) :
	Super_(s_Kind), m_pExpr(nullptr), m_pImportedField(&rn)
{
	AddFlags(NF_Imported);
}

SnNameExpr::SnNameExpr(SnMemberExpr* pExpr, const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pExpr(pExpr), m_pImportedField(nullptr)
{
	assert(pExpr);
	pExpr->Checker(&SnMemberExpr::NameExprChecker());
	AddChild(m_pExpr);
}

SnNameExpr::SnNameExpr(SnIdentifierExpr* pExpr, const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pExpr(pExpr), m_pImportedField(nullptr)
{
	assert(m_pExpr);
	AddChild(m_pExpr);
}

void SnNameExpr::Accept(nlang::ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnNameExpr::ToString() const
{
    if (!m_pExpr)
        return m_pImportedField ? m_pImportedField->Name() : "!null";
    return m_pExpr->ToString();
}

//--- SnArrayTypeExpr ---

SnArrayTypeExpr::SnArrayTypeExpr(SnFieldExpr *pElemType,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pElementType(pElemType)
{
	assert(pElemType);
	AddChild(pElemType);
}

void SnArrayTypeExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnArrayTypeExpr::ToString() const
{
	return m_pElementType->ToString() + "[]";
}

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
}

void SnSubscriptExpr::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnSubscriptExpr::ToString() const
{
	return "subscript";
}

SnNewArrayExpr::SnNewArrayExpr(SnFieldExpr *pElemType, SnExpression *pSize,
	const ISourceLocation &loc) :
	Super_(s_Kind, loc), m_pElemType(pElemType), m_pSize(pSize)
{
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

} //namespace nlang