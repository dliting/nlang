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
	if (other.Kind() == NK_Function)
	{
		//Function overloads: allow a new function with the same name only
		//if no existing function has a conflicting signature.
		//
		//At this point (MergeFrom, before ResolveDataTypes), type
		//references are unresolved SnFieldExpr nodes — pointer comparison
		//can't determine type equality. So we use param count as a
		//coarse filter: same-count functions are allowed through (they
		//might be overloads with different types), and
		//DuplicateFieldChecker (after ResolveDataTypes) does the precise
		//same-signature conflict detection using EvalDataType().
		auto range = Members().NameDict().equal_range(other.Name());
		for (auto iField = range.first; iField != range.second; ++iField)
		{
			pExisted = iField->second;
			if (pExisted->Kind() != NK_Function)
				return MAK_Conflicted;
			auto &otherFunc = static_cast<const SnFunction&>(other);
			auto &existFunc = static_cast<const SnFunction&>(*pExisted);
			//If param counts differ, it's a legitimate overload.
			if (otherFunc.Params().size() != existFunc.Params().size())
				continue;
			//Same param count — might be same signature or different
			//types. Can't tell yet, so allow through; let
			//DuplicateFieldChecker decide after type resolution.
		}
		pExisted = nullptr;
		return MAK_Add;
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
	case NK_InterfaceDecl:
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
	UniquePtrList<SnFunction> upMethods, const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, s_DefaultFlags, pName, loc),
	m_upMembers(CreateChildList<MemberList>(upMembers, this)),
	m_upMethods(CreateChildList<MethodList>(upMethods, this))
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
	//Members first, then methods: "Color.Red" is a member lookup while
	//"color.rank()" resolves the method through the same entry point.
	SnField *pMember = m_upMembers->find(sName);
	return pMember ? pMember : m_upMethods->find(sName);
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

bool SnStructField::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pType == pOld)
	{
		ResetChild(m_pType, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
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

bool SnClassField::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	if (m_pType == pOld)
	{
		ResetChild(m_pType, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
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

void SnClassDecl::AddImplementsName(SnFieldExpr *pName)
{
	m_implementsNames.push_back(pName);
	if (pName)
		AddChild(pName);
}

bool SnClassDecl::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	//The super-class name and the implements names are both cached in
	//typed slots beside the children list; keep them in sync on splice.
	if (m_pSuper == pOld)
	{
		ResetChild(m_pSuper, static_cast<SnFieldExpr *>(pNew));
		return true;
	}
	for (auto &pName : m_implementsNames)
	{
		if (pName == pOld)
		{
			ResetChild(pName, static_cast<SnFieldExpr *>(pNew));
			return true;
		}
	}
	return Super_::ReplaceChildNode(pOld, pNew);
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

//--- SnInterfaceDecl ---

SnInterfaceDecl::SnInterfaceDecl(std::string *pName, PtrList<SnField> *pMembers,
	const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, s_DefaultFlags, pName, pMembers, loc)
{
}

SnInterfaceDecl::~SnInterfaceDecl()
{
}

SnField *SnInterfaceDecl::EvalDataType() const
{
	return const_cast<SnInterfaceDecl*>(this);
}

SnField *SnInterfaceDecl::FindField(const std::string& sName) const
{
	return SnFunctionParentField::FindField(sName);
}

void SnInterfaceDecl::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnInterfaceDecl::ToString() const
{
	return "interface " + Name();
}

//--- SnUsing ---

SnUsing::SnUsing(SnFieldExpr *pPath, const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, NF_NONE, loc), m_pPath(pPath),
	m_pAliasType(nullptr), m_pNamespace(nullptr),
	m_upChildren(new ImmutableNodeList())
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

SnUsing::SnUsing(SnFieldExpr *pPath, SnFieldExpr *pAliasType,
	const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, NF_NONE, loc), m_pPath(pPath),
	m_pAliasType(pAliasType), m_pNamespace(nullptr),
	m_upChildren(new ImmutableNodeList())
{
	//Alias form: the path is the alias name, never resolved as a
	//namespace — no path checker is installed and ModuleBuilder's
	//ResolveUsingLists skips alias-form usings entirely.
	assert(m_pPath);
	assert(m_pAliasType);
	AddChild(m_pPath);
	AddChild(m_pAliasType);
}

SnUsing::~SnUsing()
{
	// m_upChildren is now unique_ptr - auto-deleted
}

std::string SnUsing::AliasName() const
{
	assert(IsAlias());
	assert(m_pPath->Kind() == NK_NameExpr);
	auto pExpr = static_cast<SnNameExpr *>(m_pPath)->Expr();
	assert(pExpr && pExpr->Kind() == NK_IdentifierExpr);
	return static_cast<SnIdentifierExpr *>(pExpr)->Name();
}

std::string SnUsing::ToString() const
{
	if (IsAlias())
		return "using " + m_pPath->ToString() + " = " +
			m_pAliasType->ToString();
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