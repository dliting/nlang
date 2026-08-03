/*-----------------------------------------------------------------------------
ncomp/intf/SnMisc.h
This file define the interface of miscellaneous syntax node types.
-----------------------------------------------------------------------------*/

#pragma once
#include "SnExpressions.h"
#include <nlang/runtime/RnMisc.h>
#include <memory>

namespace nlang
{

//The base class of a field which can be the parent of a function.
class NLANG_COMPILER_API SnFunctionParentField : public SnCompoundField
{
	typedef SnCompoundField Super_;
public:
	typedef ChildFieldList<SnField> MemberList;
public:
	//Create from parsing information.
	SnFunctionParentField(NodeKind k, FieldAccessType, NodeBits flags,
		std::string *pName, UniquePtrList<SnField> upMembers,
		const ISourceLocation&);

	//Create from an existing meta namespace.
	explicit SnFunctionParentField(RnCompoundField &);

	~SnFunctionParentField() override;

	//Get the members of this namespace.
	//@{
	MemberList &Members()
	{
		return *m_upMembers;
	}

	const MemberList &Members() const
	{
		return *m_upMembers;
	}
	//@}

	SnField *FindField(const std::string& sName) const override;
private:
	std::unique_ptr<MemberList> m_upMembers;
};

//The result of TestMemberAdding()
enum MemberAddingKind
{
	MAK_Add,				//Can be added as a member.
	MAK_Merge,				//Should be merged with an existing member.
	MAK_InvalidType,		//Invalid member type.
	MAK_Conflicted			//The field to be added is conflicted with the existing member.
};

class NLANG_COMPILER_API SnNamespace : public SnFunctionParentField
{
	typedef SnFunctionParentField Super_;
public:
	typedef RnNamespace RuntimeType;
public:
	//Create from parsing information.
	SnNamespace(std::string *pName, std::list<SnField *>* pMembers,
		const ISourceLocation&);

	//Create from an existing meta namespace.
	explicit SnNamespace(RnNamespace &);

	SnField *EvalDataType() const override;

	/*
	Merge the given members as the children of this node.
	The source member list will be cleared after merge.
	Full conflict check will not be done during merge, since the field type
	may not be resolved.
	*/
	void MergeFrom(SnNamespace &other, BuildEnvironment&);

	//Accept a visitor using the Visitor design pattern.
	void Accept(ISyntaxNodeVisitor &v) override;
protected:
	virtual bool AllowPublicAccess(const SnField & accessor) const;
private:
	MemberAddingKind TestMemberAdding(const SnField &other,
		SnField *&pExist);
	bool AllowMember(NodeKind) const;
};

class SnNameExpr;

//An enum member constant (e.g. Red = 0 in enum Color { Red, Green, Blue }).
class NLANG_COMPILER_API SnEnumMember : public SnField
{
	typedef SnField Super_;
public:
	static const NodeKind	s_Kind			= NK_EnumMember;
	static const NodeBits	s_DefaultFlags	= NF_Field | NF_Const;
public:
	SnEnumMember(std::string *pName, SnExpression *pValue,
		const ISourceLocation &loc);

	~SnEnumMember() override;

	int32_t Value() const { return m_value; }
	void SetValue(int32_t v) { m_value = v; }
	SnExpression *ValueExpr() const { return m_pValueExpr; }

	SnField *EvalDataType() const override;
	SnField *FindField(const std::string&) const override;
	void Accept(ISyntaxNodeVisitor&) override;
	std::string ToString() const override;
private:
	ImmutableNodeList *ChildrenPtr() const override;
	SnExpression *m_pValueExpr;	 //explicit value expression (may be nullptr)
	int32_t m_value;			 //resolved value
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

//An enum type declaration.
//Inherits SnCompoundField to hold SnEnumMember children and support FindField.
class NLANG_COMPILER_API SnEnumDecl : public SnCompoundField
{
	typedef SnCompoundField Super_;
public:
	static const NodeKind	s_Kind			= NK_EnumDecl;
	static const NodeBits	s_DefaultFlags	= NF_Type | NF_Field | NF_Plain;
	typedef ChildFieldList<SnEnumMember> MemberList;
public:
	SnEnumDecl(std::string *pName, UniquePtrList<SnEnumMember> upMembers,
		const ISourceLocation &loc);

	~SnEnumDecl() override;

	MemberList &Members() { return *m_upMembers; }
	const MemberList &Members() const { return *m_upMembers; }

	//enum values are int32 at runtime.
	SnField *EvalDataType() const override;
	SnField *FindField(const std::string&) const override;
	void Accept(ISyntaxNodeVisitor&) override;
	std::string ToString() const override;
private:
	std::unique_ptr<MemberList> m_upMembers;
};

//A struct field declaration (e.g. "int x" in struct Point { int x; int y; }).
//Inherits SnField like SnEnumMember.
class NLANG_COMPILER_API SnStructField : public SnField
{
	typedef SnField Super_;
public:
	static const NodeKind	s_Kind			= NK_StructField;
	static const NodeBits	s_DefaultFlags	= NF_Field | NF_Data;
public:
	SnStructField(SnFieldExpr *pType, std::string *pName,
		const ISourceLocation &loc);

	~SnStructField() override;

	SnFieldExpr *Type() const { return m_pType; }

	SnField *EvalDataType() const override;
	bool IsArrayType() const override;
	SnField *FindField(const std::string&) const override;
	void Accept(ISyntaxNodeVisitor&) override;
	std::string ToString() const override;
private:
	ImmutableNodeList *ChildrenPtr() const override;
	SnFieldExpr *m_pType;
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

	//A struct type declaration (e.g. struct Point { int x; int y; }).
//Inherits SnCompoundField to hold SnStructField children and support FindField.
class NLANG_COMPILER_API SnStructDecl : public SnCompoundField
{
	typedef SnCompoundField Super_;
public:
	static const NodeKind	s_Kind			= NK_StructDecl;
	static const NodeBits	s_DefaultFlags	= NF_Type | NF_Field | NF_Plain;
	typedef ChildFieldList<SnStructField> MemberList;
public:
	SnStructDecl(std::string *pName, UniquePtrList<SnStructField> upMembers,
		const ISourceLocation &loc);

	~SnStructDecl() override;

	MemberList &Members() { return *m_upMembers; }
	const MemberList &Members() const { return *m_upMembers; }

	size_t FieldCount() const { return m_upMembers->size(); }

	//Struct type IS the type — returns itself.
	SnField *EvalDataType() const override;
	SnField *FindField(const std::string&) const override;
	void Accept(ISyntaxNodeVisitor&) override;
	std::string ToString() const override;
private:
	std::unique_ptr<MemberList> m_upMembers;
};

//A class field declaration (e.g. "int x" in class Point { int x; int y; }).
//Similar to SnStructField but with access control (public/private/protected).
class NLANG_COMPILER_API SnClassField : public SnField
{
	typedef SnField Super_;
public:
	static const NodeKind	s_Kind			= NK_ClassField;
	static const NodeBits	s_DefaultFlags	= NF_Field | NF_Data;
public:
	SnClassField(SnFieldExpr *pType, std::string *pName, FieldAccessType access,
		const ISourceLocation &loc);

	~SnClassField() override;

	SnFieldExpr *Type() const { return m_pType; }
	FieldAccessType Access() const { return m_access; }

	SnField *EvalDataType() const override;
	bool IsArrayType() const override;
	SnField *FindField(const std::string&) const override;
	void Accept(ISyntaxNodeVisitor&) override;
	std::string ToString() const override;
private:
	ImmutableNodeList *ChildrenPtr() const override;
	SnFieldExpr *m_pType;
	FieldAccessType m_access;
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

//A class type declaration (e.g. class Point { int x; int y; int sum() { ... } }).
//Inherits SnFunctionParentField to hold both SnClassField and SnFunction members.
class NLANG_COMPILER_API SnClassDecl : public SnFunctionParentField
{
	typedef SnFunctionParentField Super_;
public:
	static const NodeKind	s_Kind			= NK_ClassDecl;
	static const NodeBits	s_DefaultFlags	= NF_Type | NF_Field | NF_Plain;
public:
	SnClassDecl(std::string *pName, SnFieldExpr *pSuper,
		PtrList<SnField> *pMembers, const ISourceLocation &loc);

	~SnClassDecl() override;

	//Super class (parent) access.
	SnFieldExpr *SuperName() const { return m_pSuper; }
	SnClassDecl *SuperClass() const { return m_pSuperClass; }
	void SuperClass(SnClassDecl *pSuper) { m_pSuperClass = pSuper; }

	//Count data fields (ClassField only, not Function members).
	size_t FieldCount() const;

	//Class type IS the type — returns itself.
	SnField *EvalDataType() const override;
	SnField *FindField(const std::string&) const override;
	void Accept(ISyntaxNodeVisitor&) override;
	std::string ToString() const override;
private:
	SnFieldExpr *m_pSuper;
	SnClassDecl *m_pSuperClass;
};

//The "using" directive in nlang.
class NLANG_COMPILER_API SnUsing : public SyntaxNode
{
	friend class ModuleBuilder;
	typedef SyntaxNode Super_;
public:
	static const NodeKind	s_Kind			= NK_Using;
	static const NodeBits	s_DefaultFlags  = NF_NONE;
public:
	SnUsing(SnFieldExpr *pPath, const ISourceLocation &loc);

	~SnUsing() override;

	//Get the namespace which is used.
	SnNamespace *Namespace() const
	{
		return m_pNamespace;
	}

	SnFieldExpr *Path() const
	{
		return m_pPath;
	}

	std::string ToString() const override;

	SnField *FindField(const std::string& sName) const override;

	void Accept(ISyntaxNodeVisitor&) override;
protected:
	ImmutableNodeList *ChildrenPtr() const override;
private:
	//The unresolved namespace path.
	SnFieldExpr *m_pPath;
	SnNamespace *m_pNamespace;
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

} //namespace nlang