/*-----------------------------------------------------------------------------
	SnTypeDecls.cpp
	Type-declaration syntax nodes: enum members and decls, struct fields
	and decls, class fields and decls, interface decls. Split from
	SnMisc.cpp (2026-09-27 maintainability refactor, zero behavior change).
-----------------------------------------------------------------------------*/

#include "SnMisc.h"
#include "SyntaxNodeVisitor.h"
#include "BuildEnvironment.h"
#include <nlang/compiler/SnTypes.h>
#include <nlang/runtime/StackDump.h>
#include <set>
#include <vector>

namespace nlang
{
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
	FieldAccessType access, const ISourceLocation &loc,
	SnExpression *pInitializer) :
	Super_(s_Kind, access, s_DefaultFlags, pName, loc),
	m_pType(pType), m_pInitializer(pInitializer),
	m_upChildren(new ImmutableNodeList())
{
	NLANG_ASSERT(m_pType);
	AddChild(m_pType);
	if (m_pInitializer)
		AddChild(m_pInitializer);
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
	if (m_pInitializer)
		result += " = " + m_pInitializer->ToString();
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
	if (m_pInitializer == pOld)
	{
		ResetChild(m_pInitializer, static_cast<SnExpression *>(pNew));
		return true;
	}
	return Super_::ReplaceChildNode(pOld, pNew);
}

//--- SnStructDecl ---

SnStructDecl::SnStructDecl(std::string *pName, UniquePtrList<SnField> upMembers,
	const ISourceLocation &loc) :
	Super_(s_Kind, FA_Public, s_DefaultFlags, pName, loc),
	m_upMembers(CreateChildList<MemberList>(upMembers, this)),
	m_upFields(new FieldList(this)),
	m_upCtors(new CtorList(this))
{
	//The merged MemberList already attached every child once (Node::AddChild
	//takes an unattached node, so a node lives in exactly one child list);
	//the kind views only index names into their own dictionaries.
	for (auto &field : *m_upMembers) {
		if (field.Kind() == NK_StructField)
			m_upFields->index_field(static_cast<SnStructField*>(&field));
		else if (field.Kind() == NK_Function)
			m_upCtors->index_field(static_cast<SnFunction*>(&field));
	}
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
	//Fields first, then constructors (see the class comment): the merged
	//pool rejects a field/constructor name clash, but resolution stays
	//deterministic while that error is being reported.
	SnField *pField = m_upFields->find(sName);
	return pField ? pField : m_upCtors->find(sName);
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
	FieldAccessType access, const ISourceLocation &loc,
	SnExpression *pInitializer) :
	Super_(s_Kind, access, s_DefaultFlags, pName, loc),
	m_pType(pType), m_pInitializer(pInitializer),
	m_upChildren(new ImmutableNodeList())
{
	NLANG_ASSERT(m_pType);
	AddChild(m_pType);
	if (m_pInitializer)
		AddChild(m_pInitializer);
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

//Out-of-line to keep the class body free of layout-bearing inline
//definitions (see the SnField::DeclaredTypeExpr discipline note).
SnFieldExpr *SnClassField::DeclaredTypeExpr() const
{
	return m_pType;
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
	if (m_pInitializer)
		result += " = " + m_pInitializer->ToString();
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
	if (m_pInitializer == pOld)
	{
		ResetChild(m_pInitializer, static_cast<SnExpression *>(pNew));
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

//First same-name non-static method in dispatch order (see SnMisc.h).
const SnFunction *SnClassDecl::FindDispatchWinner(
	const std::string &name) const
{
	std::set<const SnClassDecl*> seen;
	for (auto *pChain = this; pChain && !seen.count(pChain);
		pChain = pChain->SuperClass())
	{
		seen.insert(pChain);
		if (pChain->IsBuiltinClass())
			break;
		for (auto &member : pChain->Members())
			if (member.Kind() == NK_Function && member.Name() == name
				&& !member.ContainFlags(NF_Static))
				return static_cast<const SnFunction*>(&member);
	}
	return nullptr;
}

//Abstract obligations of the whole chain, outermost ancestor first (the
//order the instantiation gate reports). NF_Static | NF_Native members
//never carry the obligation pair (the grammar exempts natives; the
//static conflict is a declaration error), so the skip here is a shape
//guard, not the primary defense.
void SnClassDecl::CollectAbstractObligations(
	std::vector<const SnFunction*> &out) const
{
	std::vector<const SnClassDecl*> chain;
	std::set<const SnClassDecl*> seen;
	for (auto *pChain = this; pChain && !seen.count(pChain);
		pChain = pChain->SuperClass())
	{
		seen.insert(pChain);
		if (pChain->IsBuiltinClass())
			break;
		chain.push_back(pChain);
	}
	for (auto pIt = chain.rbegin(); pIt != chain.rend(); ++pIt)
		for (auto &member : (*pIt)->Members())
		{
			if (member.Kind() != NK_Function
				|| !member.ContainFlags(NF_Abstract)
				|| member.IntersectFlags(NF_Static | NF_Native))
				continue;
			//Winner-still-abstract rule: a concrete override anywhere in
			//dispatch order satisfies the obligation; an abstract winner
			//(own re-declaration, re-abstraction) keeps it open.
			auto *pWinner = FindDispatchWinner(member.Name());
			if (pWinner && !pWinner->ContainFlags(NF_Abstract))
				continue;
			out.push_back(static_cast<const SnFunction*>(&member));
		}
}

bool SnClassDecl::IsAbstractClass() const
{
	std::vector<const SnFunction*> obligations;
	CollectAbstractObligations(obligations);
	return !obligations.empty();
}

//v0.13.7: the row-level twin of SnExpression::IsArrayValued — an
//Array<E> instantiation IS the array type (the generic spelling of
//`T[]`); every other class row is not.
bool SnClassDecl::IsArrayType() const
{
	return ArrayInstantiationElem(
		const_cast<SnClassDecl *>(this)) != nullptr;
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

} //namespace nlang
