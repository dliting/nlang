/*-----------------------------------------------------------------------------
ncomp/intf/SnMisc.cpp
This file define the implementation of miscellaneous syntax node types.
-----------------------------------------------------------------------------*/

#include "SnMisc.h"
#include "SyntaxNodeVisitor.h"
#include "BuildEnvironment.h"
#include <nlang/compiler/SnTypes.h>

namespace nlang
{

SnFunctionParentField::SnFunctionParentField(NodeKind k, FieldAccessType at,
	NodeBits flags, std::string *pName, UniquePtrList<SnField> upMembers,
	const ISourceLocation &loc) :
	Super_(k, at, flags, pName, loc),
	m_upMembers(CreateChildFields(upMembers, this))
{
	assert(CanBeFuncParentEx(k));
}

SnFunctionParentField::SnFunctionParentField(RnCompoundField &rn) :
	Super_(rn), m_upMembers(new MemberList(this))
{
	assert(CanBeFuncParentEx(rn.Kind()));
}

SnFunctionParentField::~SnFunctionParentField()
{
	// m_upMembers is now unique_ptr - auto-deleted
}

SnField *SnFunctionParentField::FindField(const std::string& sName) const
{
	return m_upMembers->find(sName);
}

SnNamespace::SnNamespace(std::string *pName, PtrList<SnField> *pMembers,
	const ISourceLocation &loc) :
	Super_(RuntimeType::s_Kind, FA_Public, RuntimeType::s_DefaultFlags, 
		pName, pMembers, loc)
{
}

SnNamespace::SnNamespace(RnNamespace &rn) : Super_(rn)
{
}

void SnNamespace::MergeFrom(SnNamespace &other, BuildEnvironment &env)
{
	auto &otherMembers = other.Members();
	auto iOther = otherMembers.begin();
	auto iEnd = otherMembers.end();
	while (iOther != iEnd)
	{
		SnField *pOther		= &(*iOther);
		SnField *pExisted	= nullptr;
		switch (auto mak = TestMemberAdding(*pOther, pExisted))
		{
		case MAK_Add:
			iOther = otherMembers.erase(iOther);
			Members().push_back(pOther);
			continue;
		case MAK_Merge:
			assert(pOther->Kind() == NK_Namespace && pExisted);
			static_cast<SnNamespace *>(pExisted)->MergeFrom(
				static_cast<SnNamespace &>(*pOther), env);
			break;
		case MAK_InvalidType:
			env.Log(CLL_Error, pOther->Location(),
				"%s can not be a member of %s.",
				pOther->TypedName().c_str(), this->TypedName().c_str());
			break;
		default:
			assert(mak == MAK_Conflicted && pExisted);
			env.Log(CLL_Error, pOther->Location(),
				"The namespace member \"%s\" has has already been defined.",
				pOther->Name().c_str());
			env.Log(CLL_Error, pExisted->Location(),
				"See also the definition of  \"%s\".",
				pExisted->ToString().c_str());
		}
		iOther++;
	}
}

MemberAddingKind SnNamespace::TestMemberAdding(const SnField &other, 
	SnField *&pExisted)
{
	if (!AllowMember(other.Kind()))
		return MAK_InvalidType;
	if (other.Kind() == NK_Namespace)
	{
		//Prefer to merge with an existing namespace if there are more than one
		//fields is found with the same name.
		auto range = Members().NameDict().equal_range(other.Name());
		for (auto iField = range.first; iField != range.second; ++iField)
		{
			pExisted = iField->second;
			if (pExisted->Kind() == NK_Namespace)
				return MAK_Merge;
		}
		if (range.first != range.second)
			return MAK_Conflicted;
	}
	return MAK_Add;
}

bool SnNamespace::AllowMember(NodeKind k) const
{
	switch (k)
	{
	case NK_Function:
	case NK_Namespace:
	case NK_EnumDecl:
	case NK_StructDecl:
	case NK_ClassDecl:
		return true;
	default:
		return IsBuiltinType(k) && (Name() == GLOBAL_NAMESPACE_NAME);
	}
}

bool SnNamespace::AllowPublicAccess(const SnField &accessor) const
{
	return true;
}

void SnNamespace::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

SnField * SnNamespace::EvalDataType() const
{
	return SnType::Instance();
}

//--- SnEnumMember ---

SnEnumMember::SnEnumMember(std::string *pName, SnExpression *pValue,
	const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, s_DefaultFlags, pName, loc),
	m_pValueExpr(pValue), m_value(0),
	m_upChildren(new ImmutableNodeList())
{
	if (m_pValueExpr)
		AddChild(m_pValueExpr);
}

SnEnumMember::~SnEnumMember()
{
}

SnField *SnEnumMember::EvalDataType() const
{
	return SnBuiltinDataType::InstanceOf(NK_Int32);
}

SnField *SnEnumMember::FindField(const std::string&) const
{
	return nullptr;
}

void SnEnumMember::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnEnumMember::ToString() const
{
	return Name() + " = " + std::to_string(m_value);
}

ImmutableNodeList *SnEnumMember::ChildrenPtr() const
{
	return m_upChildren.get();
}

//--- SnEnumDecl ---

SnEnumDecl::SnEnumDecl(std::string *pName, UniquePtrList<SnEnumMember> upMembers,
	const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, s_DefaultFlags, pName, loc),
	m_upMembers(CreateChildFields(upMembers, this))
{
}

SnEnumDecl::~SnEnumDecl()
{
}

SnField *SnEnumDecl::EvalDataType() const
{
	return SnBuiltinDataType::InstanceOf(NK_Int32);
}

SnField *SnEnumDecl::FindField(const std::string& sName) const
{
	return m_upMembers->find(sName);
}

void SnEnumDecl::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnEnumDecl::ToString() const
{
	return "enum " + Name();
}

//--- SnStructField ---

SnStructField::SnStructField(SnFieldExpr *pType, std::string *pName,
	const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, s_DefaultFlags, pName, loc),
	m_pType(pType), m_upChildren(new ImmutableNodeList())
{
	assert(m_pType);
	AddChild(m_pType);
}

SnStructField::~SnStructField()
{
}

SnField *SnStructField::EvalDataType() const
{
	if (m_pType && m_pType->Field())
		return m_pType->Field();
	return nullptr;
}

bool SnStructField::IsArrayType() const
{
	return m_pType && m_pType->IsArrayType();
}

SnField *SnStructField::FindField(const std::string&) const
{
	return nullptr;
}

void SnStructField::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnStructField::ToString() const
{
	std::string result;
	if (m_pType && m_pType->Field())
		result = m_pType->Field()->Name();
	else if (m_pType)
		result = m_pType->ToString();
	result += " " + Name();
	return result;
}

ImmutableNodeList *SnStructField::ChildrenPtr() const
{
	return m_upChildren.get();
}

//--- SnStructDecl ---

SnStructDecl::SnStructDecl(std::string *pName, UniquePtrList<SnStructField> upMembers,
	const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, s_DefaultFlags, pName, loc),
	m_upMembers(CreateChildFields(upMembers, this))
{
}

SnStructDecl::~SnStructDecl()
{
}

SnField *SnStructDecl::EvalDataType() const
{
	return const_cast<SnStructDecl*>(this);
}

SnField *SnStructDecl::FindField(const std::string& sName) const
{
	return m_upMembers->find(sName);
}

void SnStructDecl::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnStructDecl::ToString() const
{
	return "struct " + Name();
}

//--- SnClassField ---

SnClassField::SnClassField(SnFieldExpr *pType, std::string *pName,
	FieldAccessType access, const ISourceLocation &loc) :
	Super_(s_Kind, access, s_DefaultFlags, pName, loc),
	m_pType(pType), m_upChildren(new ImmutableNodeList())
{
	assert(m_pType);
	AddChild(m_pType);
}

SnClassField::~SnClassField()
{
}

SnField *SnClassField::EvalDataType() const
{
	if (m_pType && m_pType->Field())
		return m_pType->Field();
	return nullptr;
}

bool SnClassField::IsArrayType() const
{
	return m_pType && m_pType->IsArrayType();
}

SnField *SnClassField::FindField(const std::string&) const
{
	return nullptr;
}

void SnClassField::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnClassField::ToString() const
{
	std::string result;
	if (m_pType && m_pType->Field())
		result = m_pType->Field()->Name();
	else if (m_pType)
		result = m_pType->ToString();
	result += " " + Name();
	return result;
}

ImmutableNodeList *SnClassField::ChildrenPtr() const
{
	return m_upChildren.get();
}

//--- SnClassDecl ---

SnClassDecl::SnClassDecl(std::string *pName, SnFieldExpr *pSuper,
	PtrList<SnField> *pMembers, const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, s_DefaultFlags, pName, pMembers, loc),
	m_pSuper(pSuper), m_pSuperClass(nullptr)
{
	if (m_pSuper)
		AddChild(m_pSuper);
}

SnClassDecl::~SnClassDecl()
{
}

size_t SnClassDecl::FieldCount() const
{
	size_t count = 0;
	for (auto &member : Members())
	{
		if (member.Kind() == NK_ClassField)
			++count;
	}
	return count;
}

SnField *SnClassDecl::EvalDataType() const
{
	return const_cast<SnClassDecl*>(this);
}

SnField *SnClassDecl::FindField(const std::string& sName) const
{
	if (auto pField = SnFunctionParentField::FindField(sName))
		return pField;
	if (m_pSuperClass)
		return m_pSuperClass->FindField(sName);
	return nullptr;
}

void SnClassDecl::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnClassDecl::ToString() const
{
	return "class " + Name();
}

//--- SnUsing ---

SnUsing::SnUsing(SnFieldExpr *pPath, const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, NF_NONE, loc), m_pPath(pPath),
	m_pNamespace(nullptr), m_upChildren(new ImmutableNodeList())
{
	static SnFieldExpr::FieldChecker pathChecker =
		[](SnFieldExpr& path, BuildEnvironment& env)->bool 
	{
		auto pField = path.Field();
		assert(pField);
		if (pField->Kind() == NK_Namespace)
			return true;
		env.Log(CLL_Error, path.Location(),
			"The field \"%s\" is not a namespace.", 
			pField->ToString().c_str());
		env.Log(CLL_More, pField->Location(),
			"See the declaration of \"%s\".",
			pField->ToString().c_str());
		return false;
	};

	assert(m_pPath);
	m_pPath->Checker(&pathChecker);
	AddChild(m_pPath);
}

SnUsing::~SnUsing()
{
	// m_upChildren is now unique_ptr - auto-deleted
}

std::string SnUsing::ToString() const
{
	return "using " + m_pPath->ToString();
}

SnField * SnUsing::FindField(const std::string& sName) const
{
	return nullptr;
}

void SnUsing::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

ImmutableNodeList * SnUsing::ChildrenPtr() const
{
	return m_upChildren.get();
}

} //namespace nlang