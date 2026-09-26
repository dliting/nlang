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

} //namespace nlang
