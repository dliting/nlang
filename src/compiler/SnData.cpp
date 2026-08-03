#include "SnData.h"
#include "SyntaxNodeVisitor.h"
#include <sstream>

#ifdef NLANG_ENABLE_LLVM
#include <llvm/IR/Function.h>
#endif

namespace nlang
{

SnDataField::SnDataField(NodeKind k, FieldAccessType at, NodeBits flags,
	SnFieldExpr *pType, std::string *pName, SnExpression *pDefault,
	const ISourceLocation &loc):
	Super_(k, at, flags, pName, loc), m_pType(pType), m_pValue(pDefault),
	m_upChildren(new ImmutableNodeList())
{
	Init();
}

SnDataField::SnDataField(RnDataField &rn) :
	Super_(rn), m_pType(new SnNameExpr(rn.Type())), m_pValue(nullptr),
	m_upChildren(new ImmutableNodeList())
{
	if (rn.Value())
		m_pValue = new SnLiteralExpr(rn.Type(), rn.Value());
	Init();
	AddFlags(NF_Resolved);
}

SnDataField::~SnDataField()
{
	// m_upChildren is now unique_ptr - auto-deleted
}

void SnDataField::Init()
{
	assert(m_pType);
	m_pType->Checker(&SnFieldExpr::DataTypeChecker());
	AddChild(m_pType);
	if (m_pValue)
		AddChild(m_pValue);
}

SnField *SnDataField::EvalDataType() const
{
	if (!m_pType || !m_pType->Field())
		return nullptr;
	assert(m_pType->Field()->IsTypeField());
	return static_cast<SnField *>(m_pType->Field());
}

bool SnDataField::IsArrayType() const
{
	return m_pType && m_pType->IsArrayType();
}

std::string SnDataField::ToString() const
{
	std::stringstream ss;
	ss << (m_pType ? m_pType->ToString() : "!unknown") << " ";
	ss << Name();
	if (m_pValue)
		ss << " = " << m_pValue->ToString();
	return ss.str();
}

ImmutableNodeList * SnDataField::ChildrenPtr() const
{
	return m_upChildren.get();
}

SnField * SnDataField::FindField(const std::string& sName) const
{
	return nullptr;
}

SnFormalParam::SnFormalParam(NodeBits flags, SnFieldExpr *pType,
	std::string *pName, SnExpression *pDefault, const ISourceLocation &loc) :
	Super_(RuntimeType::s_Kind, FA_Public, flags | RuntimeType::s_DefaultFlags,
		pType, pName, pDefault, loc)
{
}

SnFormalParam::SnFormalParam(RnFormalParam &rn) : Super_(rn)
{
}

void SnFormalParam::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

SnFunction::SnFunction(FieldAccessType at, NodeBits flags,
	SnFieldExpr *pReturnType, std::string *pName,
	UniquePtrList<SnFormalParam> upParams, const ISourceLocation &loc) :
	Super_(RuntimeType::s_Kind, at, flags | RuntimeType::s_DefaultFlags,
		pName, loc),
	m_pReturnType(pReturnType),
	m_upParams(CreateChildFields(upParams, this)), m_pBody(nullptr)
{
	Init();
	if (m_pReturnType)
		m_pReturnType->Checker(&SnFieldExpr::DataTypeChecker());
}

SnFunction::SnFunction(RnFunction &rn):
	Super_(rn), m_pReturnType(nullptr), m_upParams(new ParamList(this)),
	m_pBody(nullptr)
{
	if (rn.ReturnType())
	{
		assert(rn.ReturnType());
		m_pReturnType = new SnNameExpr(*rn.ReturnType());
		AddChild(m_pReturnType);
	}
	Init();
}

void SnFunction::Init()
{
	if (m_pReturnType)
		AddChild(m_pReturnType);
}

void SnFunction::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnFunction::ToString() const
{
	std::stringstream ss;
	if (m_pReturnType)
		ss << (m_pReturnType ? m_pReturnType->ToString() : "void") << " ";
	ss << Name();

	ss << "(";
	size_t i = 0;
	for (auto &param : Params())
	{
		if (i != 0)
			ss << ", ";
		ss << param.ToString();
		i++;
	}
	ss << ")";
	return ss.str();
}

bool SnFunction::ConflictedWith(const SnField &other) const
{
	const bool bSameName = other.Name() == Name();
	if (other.Kind() != NK_Function)
		return bSameName;

	auto &otherFunc		= static_cast<const SnFunction &>(other);
	auto iOther			= otherFunc.Params().begin();
	auto iOtherEnd		= otherFunc.Params().end();
	auto iThis			= Params().begin();
	auto iThisEnd		= Params().end();
	//Compare the params one by one.
	while (true)
	{
		auto *pThisParam = (iThis == iThisEnd ? nullptr : &(*iThis));
		auto *pOtherParam = (iOther == iOtherEnd ? nullptr : &(*iOther));

		if (pThisParam && !pOtherParam)
			return pThisParam->IsOptional();

		if (!pThisParam && pOtherParam)
			return pOtherParam->IsOptional();

		if (pThisParam->IsOptional() && pOtherParam->IsOptional())
			return true;

		if (pOtherParam->Type() != pThisParam->Type())
			return false;

		++iThis;
		++iOther;
	}
	return true;
}

SnField *SnFunction::FindField(const std::string& sName) const
{
	//Search in parameters first
	SnField *pParam = m_upParams->find(sName);
	if (pParam)
		return pParam;
	//Search in body locals
	if (m_pBody) {
		return m_pBody->FindLocal(sName);
	}
	return nullptr;
}

SnField *SnFunction::EvalDataType() const
{
	return m_pReturnType ?
		static_cast<SnField *>(m_pReturnType->Field()) : nullptr;
}

#ifdef NLANG_ENABLE_LLVM
llvm::Function * SnFunction::MetaFunc() const
{
	assert(!MetaValue() || llvm::isa<llvm::Function>(MetaValue()));
	return static_cast<llvm::Function *>(MetaValue());
}
#endif

} //namespace nlang
