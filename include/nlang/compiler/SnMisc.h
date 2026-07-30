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

//The "using" directive in nlang.
class NLANG_COMPILER_API SnUsing : public SyntaxNode
{
	friend class ModuleBuilder;
	typedef SyntaxNode Super_;
public:
	static const NodeKind	s_Kind			= NK_Using;
	static const NodeBits	s_DefaultFlags  = NF_NONE;
public:
	SnUsing(SnNameExpr *pPath, const ISourceLocation &loc);

	~SnUsing() override;

	//Get the namespace which is used.
	SnNamespace *Namespace() const
	{
		return m_pNamespace;
	}

	SnNameExpr *Path() const
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
	SnNameExpr *m_pPath;
	SnNamespace *m_pNamespace;
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

} //namespace nlang