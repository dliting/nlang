/*-----------------------------------------------------------------------------
ncomp/intf/SnMisc.h
This file define the interface of miscellaneous syntax node types.
-----------------------------------------------------------------------------*/

#pragma once
#include "SnExpressions.h"
#include "SnData.h"
#include <nlang/runtime/RnMisc.h>
#include <memory>

namespace nlang
{

//Forward declaration so SnClassDecl can hold a vector of implements pointers.
class NLANG_COMPILER_API SnInterfaceDecl;

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
	/*
	Members and methods share one child node list, so both views are kind
	filtered: DefaultFieldFilter tests the IsField() flag, and every
	SnFunction carries NF_Field — a flag-based MemberList would iterate
	method nodes and cast them to SnEnumMember.
	*/
	typedef ChildFieldList<SnEnumMember, KindFieldFilter<NK_EnumMember>> MemberList;
	typedef ChildFieldList<SnFunction, KindFieldFilter<NK_Function>> MethodList;
public:
	SnEnumDecl(std::string *pName, UniquePtrList<SnEnumMember> upMembers,
		UniquePtrList<SnFunction> upMethods, const ISourceLocation &loc);

	~SnEnumDecl() override;

	MemberList &Members() { return *m_upMembers; }
	const MemberList &Members() const { return *m_upMembers; }

	MethodList &Methods() { return *m_upMethods; }
	const MethodList &Methods() const { return *m_upMethods; }

	//enum values are int32 at runtime.
	SnField *EvalDataType() const override;
	SnField *FindField(const std::string&) const override;
	void Accept(ISyntaxNodeVisitor&) override;
	std::string ToString() const override;
private:
	std::unique_ptr<MemberList> m_upMembers;
	std::unique_ptr<MethodList> m_upMethods;
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
	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;
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
	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;
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

	//Interfaces declared via "implements I1, I2" in the class header.
	//Returns the raw name expressions from the grammar; resolved pointers
	//are populated via AddImplements during semantic analysis.
	const std::vector<SnFieldExpr*> &ImplementsNames() const { return m_implementsNames; }
	void AddImplementsName(SnFieldExpr *pName);
	const std::vector<SnInterfaceDecl*> &ImplementsList() const { return m_implements; }
	void AddImplements(SnInterfaceDecl *pInterface) { m_implements.push_back(pInterface); }

	//Count data fields (ClassField only, not Function members).
	size_t FieldCount() const;

	//Class type IS the type — returns itself.
	SnField *EvalDataType() const override;
	SnField *FindField(const std::string&) const override;
	void Accept(ISyntaxNodeVisitor&) override;
	std::string ToString() const override;
	bool ReplaceChildNode(SyntaxNode*, SyntaxNode*) override;

	//Built-in class marker (ByteStream, FileStream, etc.).
	//Set by the resolver when it synthesizes a SnClassDecl for
	//a built-in name that doesn't exist in user code.
	bool IsBuiltinClass() const { return m_bIsBuiltinClass; }
	void SetBuiltinClass() { m_bIsBuiltinClass = true; }

	//Phase 8e-3: built-in generic instantiation marker (e.g. List<int>).
	//Set by ExprResolver when minting a synthetic SnClassDecl for a
	//built-in generic type. m_genericTypeArgs carries the resolved type
	//arguments (e.g. {SnInt32} for List<int>); VmBackend reads these to
	//emit OP_Box/OP_Unbox around primitive-T method args/returns.
	bool IsGenericInstantiation() const { return m_bIsGenericInst; }
	void SetGenericInstantiation() { m_bIsGenericInst = true; }
	const std::vector<SnField*>& GenericTypeArgs() const { return m_genericTypeArgs; }
	void SetGenericTypeArgs(std::vector<SnField*> args) { m_genericTypeArgs = std::move(args); }
	//Base name without <...> suffix. For generic instantiations only;
	//Equals Name() for ordinary classes. Used by VmBackend to look up the
	//shared backing CompiledClass (e.g. "List" for List<int>).
	const std::string& BaseName() const
	{ return m_bIsGenericInst ? m_baseName : Name(); }
	void SetBaseName(const std::string& name) { m_baseName = name; }

private:
	SnFieldExpr *m_pSuper;
	SnClassDecl *m_pSuperClass;
	std::vector<SnFieldExpr*> m_implementsNames;
	std::vector<SnInterfaceDecl*> m_implements;
	bool m_bIsBuiltinClass = false;
	bool m_bIsGenericInst = false;
	std::vector<SnField*> m_genericTypeArgs;
	std::string m_baseName;  //e.g. "List" (without <T>) for generic instances
};

//An interface type declaration (e.g. interface IPrintable { void Print(); }).
//Members are method signatures only (no fields, no bodies). An interface is
//"implemented" by a class via the implements clause; the class must declare
//matching methods. Interfaces support multiple implementation (one class may
//implement several), but interfaces themselves cannot extend one another
//in this phase.
class NLANG_COMPILER_API SnInterfaceDecl : public SnFunctionParentField
{
	typedef SnFunctionParentField Super_;
public:
	static const NodeKind	s_Kind			= NK_InterfaceDecl;
	static const NodeBits	s_DefaultFlags	= NF_Type | NF_Field | NF_Plain;
public:
	SnInterfaceDecl(std::string *pName, PtrList<SnField> *pMembers,
		const ISourceLocation &loc);

	~SnInterfaceDecl() override;

	//Interface type IS the type — returns itself.
	SnField *EvalDataType() const override;
	SnField *FindField(const std::string&) const override;
	void Accept(ISyntaxNodeVisitor&) override;
	std::string ToString() const override;
};

//The "using" directive in nlang.
//Phase 13 adds the type alias form: "using Name = Type;". The namespace
//form ("using NameSpace;") is unchanged; the two are distinguished by
//the presence of the alias target node.
class NLANG_COMPILER_API SnUsing : public SyntaxNode
{
	friend class ModuleBuilder;
	typedef SyntaxNode Super_;
public:
	static const NodeKind	s_Kind			= NK_Using;
	static const NodeBits	s_DefaultFlags  = NF_NONE;
public:
	//Namespace form: pPath is the namespace path to resolve.
	SnUsing(SnFieldExpr *pPath, const ISourceLocation &loc);

	//Type alias form: pPath is the alias name (a plain name expression),
	//pAliasType is the aliased target type expression.
	SnUsing(SnFieldExpr *pPath, SnFieldExpr *pAliasType,
		const ISourceLocation &loc);

	~SnUsing() override;

	//Is this the type alias form?
	bool IsAlias() const
	{
		return m_pAliasType != nullptr;
	}

	//Get the aliased target type expression (alias form only).
	SnFieldExpr *AliasType() const
	{
		return m_pAliasType;
	}

	//Get the alias name (alias form only; the identifier of the path
	//name expression). Returned by value — SnIdentifierExpr::Name()
	//materializes the string.
	std::string AliasName() const;

	//Get the namespace which is used (namespace form only).
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
	//The unresolved namespace path / the alias name.
	SnFieldExpr *m_pPath;
	//The aliased target type (null for the namespace form).
	SnFieldExpr *m_pAliasType;
	SnNamespace *m_pNamespace;
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

} //namespace nlang