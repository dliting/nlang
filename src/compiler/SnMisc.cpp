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
				"The package member \"%s\" has already been defined.",
				pOther->Name().c_str());
			env.Log(CLL_Error, pExisted->Location(),
				"See also the definition of  \"%s\".",
				pExisted->ToString().c_str());
		}
		iOther++;
	}
}

//Function-overload coarse filter of TestMemberAdding below: scans the
//same-named members and returns the one that BLOCKS the add (a non-
//function member with the name). At this point (MergeFrom, before
//ResolveDataTypes), type references are unresolved SnFieldExpr nodes —
//pointer comparison can't determine type equality. So param count is
//the coarse filter: same-count functions are allowed through (they
//might be overloads with different types), and
//DuplicateFieldChecker (after ResolveDataTypes) does the precise
//same-signature conflict detection using EvalDataType().
static SnField *FindOverloadBlocker(SnFunctionParentField::MemberList &members,
	const SnFunction &otherFunc)
{
	auto range = members.NameDict().equal_range(otherFunc.Name());
	for (auto iField = range.first; iField != range.second; ++iField)
	{
		SnField *pExisted = iField->second;
		if (pExisted->Kind() != NK_Function)
			return pExisted;
		auto &existFunc = static_cast<const SnFunction&>(*pExisted);
		//If param counts differ, it's a legitimate overload.
		if (otherFunc.Params().size() != existFunc.Params().size())
			continue;
		//Same param count — might be same signature or different
		//types. Can't tell yet, so allow through; let
		//DuplicateFieldChecker decide after type resolution.
	}
	return nullptr;
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
		pExisted = FindOverloadBlocker(Members(),
			static_cast<const SnFunction&>(other));
		return pExisted ? MAK_Conflicted : MAK_Add;
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
			"The field \"%s\" is not a package.",
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