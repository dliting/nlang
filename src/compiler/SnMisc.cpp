/*-----------------------------------------------------------------------------
ncomp/intf/SnMisc.cpp
This file define the implementation of miscellaneous syntax node types.
-----------------------------------------------------------------------------*/

#include "SnMisc.h"
#include "SyntaxNodeVisitor.h"
#include "BuildEnvironment.h"

namespace nlang
{

SnFunctionParentField::SnFunctionParentField(NodeKind k, FieldAccessType at,
	NodeBits flags, std::string *pName, UniquePtrList<SnField> upMembers,
	const ISourceLocation &loc) :
	Super_(k, at, flags, pName, loc),
	m_upMembers(CreateChildFields(upMembers, this))
{
	assert(CanBeFuncParent(k));
}

SnFunctionParentField::SnFunctionParentField(RnCompoundField &rn) :
	Super_(rn), m_upMembers(new MemberList(this))
{
	assert(CanBeFuncParent(rn.Kind()));
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

SnUsing::SnUsing(SnNameExpr *pPath, const ISourceLocation &loc) :
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