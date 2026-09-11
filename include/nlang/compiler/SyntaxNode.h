/*-----------------------------------------------------------------------------
	ncomp/intf/SyntaxNode.h
	This file includes the declaration of common constants and common syntax
node types in an nlang AST.
-----------------------------------------------------------------------------*/

#pragma once
#include "TypeDef.h"
#include "SyntaxNodeConsts.h"
#include <nlang/runtime/AutoPointers.h>
#include <nlang/runtime/Flagable.h>
#include <nlang/runtime/RuntimeNode.h>
#include <nlang/runtime/CommonIterators.h>
#include <nlang/runtime/NodeContainers.h>
#include <memory>

//Forward declarations.
#ifdef NLANG_ENABLE_LLVM
namespace llvm
{

class Type;
class Value;

} //namespace llvm
#endif

namespace nlang
{

struct ISyntaxNodeVisitor;
class SnUsing;
typedef UniquePtrList<SnUsing> UsingList;

class SnField;

//The abstract base class of all the other syntax node classes in an AST.
class NLANG_COMPILER_API SyntaxNode: public Node
{
	friend class StatementResolveAccessor;
	typedef Node Super_;
public:
	SyntaxNode(NodeKind, FieldAccessType, NodeBits flags);

	SyntaxNode(NodeKind, FieldAccessType, NodeBits flags,
		const ISourceLocation &);

	//Construct from an existing runtime type.
	explicit SyntaxNode(RuntimeNode &);

	~SyntaxNode() override;

	//Get the parent.
	SyntaxNode *Parent() const
	{
		assert(!Super_::Parent() ||
			dynamic_cast<SyntaxNode*>(Super_::Parent()));
		return static_cast<SyntaxNode*>(Super_::Parent());
	}

	/*
	Set the parent.
	If the parents are not null, this node will be a child of the new parent
	and remove from the old parent.
	*/
	void Parent(SyntaxNode *pParent)
	{
		Super_::Parent(pParent);
	}

	//Get the source location.
	const ISourceLocation *Location() const
	{
		return m_upLocation.get();
	}

	bool IsResolved() const
	{
		return ContainFlags(NF_Resolved);
	}

	bool IsImported() const
	{
		return ContainFlags(NF_Imported);
	}

	bool IsGenerated() const
	{
		return ContainFlags(NF_Generated);
	}

	bool IsInvalid() const
	{
		return ContainFlags(NF_Invalid);
	}

	bool IsExpression() const
	{
		return ContainFlags(NF_Expression);
	}

	/*
	Find a child field by name.
	\return null if not found.
	*/
	virtual SnField *FindField(const std::string& sName) const = 0;

	//Get the namespace using list of this node.
	const UsingList *Usings() const;

	/*
	Replace a direct child node with another one, in the same slot.
	\return false if pOld is not a child of this node.
	Precondition: pOld is a child of this node; pNew has no parent.

	The base implementation only splices the children list. Node classes
	that cache a typed member pointer to a child (e.g. a type slot like
	SnDataField::m_pType) MUST override this to route through ResetChild,
	keeping the member and the children list in sync.
	Used by the Phase 13 TU-level alias expansion pre-pass; node classes
	that may hold a type name expression child must stay reachable here.
	*/
	virtual bool ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew);

	/*
	Detach a direct child from this node without deleting it; ownership
	passes back to the caller, who typically re-parents it with AddChild
	on the new owner (DOM removeChild protocol). The typed member slot of
	the old owner is intentionally left pointing at the detached node so
	existing readers keep working until the caller re-parents it.
	\return the detached node, or null if pChild is not a child of this
	node (including null input and foreign nodes).
	Used by the local-decl decomposition in StatementResolver (Phase 13
	Step 0.5 container unification).
	*/
	SyntaxNode *DetachChild(SyntaxNode *pChild);
	template <class NODE_T>
	NODE_T *DetachChild(NODE_T *pChild)
	{
		return static_cast<NODE_T *>(DetachChild(static_cast<SyntaxNode *>(pChild)));
	}

	//Accept a visitor using the Visitor design pattern.
	virtual void Accept(ISyntaxNodeVisitor&) = 0;

	void Dump(std::ostream&) const override;

	//Get the syntax node type traits.
	static const NodeTraits &TraitsOf(NodeKind);

	static const std::string &EmptyMetaName()
	{
		static const std::string s_EmptyMetaName;
		return s_EmptyMetaName;
	}
protected:
	/*
	Replace a exist child node with a new one and reset the pointer reference.
	The pointer reference to the exist child will be set to the new pointer.
	\param pOld The pointer reference to a exist child node or to null.
	\param pNew The node pointer should be replace the old one.

	Precondition: pOld is child or null; pNew is null or pNew->Parent()
	is null.

	Effect:
	1) If pOld is not null and pNew is not null, pOld will be removed
	from children list and deleted.
	2) If pOld is null, behaves like 1), but no exist child is removed.
	3) If pNew is null, behaves like 1), but no child is inserted.
	*/
	template <class NODE_T>
	void ResetChild(NODE_T *&pOld, NODE_T *pNew)
	{
		pOld = static_cast<NODE_T *>(DoResetChild(pOld, pNew));
	}
private:
	SyntaxNode *DoResetChild(SyntaxNode *pOld, SyntaxNode *pNew);

	//Assign node flags and access type to a given runtime node.
	//The compile only bits of this node will be ignored.
	void AssignRuntimeBitsTo(RuntimeNode &meta)
	{
		meta.AccessType(AccessType());
		meta.Flags(Flags()  &RUNTIME_NODE_FLAGS_MASK);
	}
	std::unique_ptr<ISourceLocation> m_upLocation;
};

//A named syntax node acts as a part of a composite node.
class NLANG_COMPILER_API SnField : public SyntaxNode
{
	friend class DataGenerateAccessor;
	friend class TypeGenerateAccessor;
	typedef SyntaxNode Super_;
public:
	typedef std::string NameType;
public:
	//Construct from a parser.
	SnField(NodeKind k, FieldAccessType at, NodeBits flags,
		std::string *pName, const ISourceLocation &loc);

	//Construct from an existing runtime type.
	explicit SnField(RnField &rtti);

	~SnField() override;

	//Get the name.
	const std::string &Name() const
	{
		assert(m_upName || RuntimeType());
		return m_upName ? *m_upName :
			static_cast<RnField*>(RuntimeType())->Name().ToString();
	}

	/*
	Get the imported runtime node of this syntax field.
	The meta type is the the runtime object corresponding to this syntax node.
	\return If the node is not resolved, this meta data will be null.
	*/
	RnField *RuntimeType() const
	{
		return m_pImportInfo;
	}

	//Get the imported runtime node of its parent.
	RnField *ParentRuntimeType() const
	{
		assert(!Parent() || Parent()->IsField());
		return Parent() ? static_cast<SnField *>(Parent())->RuntimeType() : nullptr;
	}

	/*
	Get the evaluated data type of this field.
	\return Return null if this field is not resolved correctly.
	*/
	virtual SnField *EvalDataType() const = 0;

	//Is this field's type an array type (e.g. int[] x)?
	//Subclasses with a type expression override this to forward to the SnNameExpr.
	virtual bool IsArrayType() const
	{
		return false;
	}

	//For a variable/field/param declared with a built-in generic type
	//(List<T>, Dict<K,V>, Func<R,...>): the index of the first type
	//argument that is an ARRAY type (the T of List<T[]>), or
	//kNoArrayTypeArg when none. Generic instantiation keys erase
	//array-ness — an ArrayTypeExpr type argument resolves to its ELEMENT
	//field — so the declaration records it here for the resolver gates
	//(see RecordArrayTypeArg in ExprResolver).
	static const uint8 kNoArrayTypeArg = 0xFF;

	uint8 ArrayTypeArg() const
	{
		return m_uArrayTypeArg;
	}

	void SetArrayTypeArg(uint8 uArgIdx)
	{
		m_uArrayTypeArg = uArgIdx;
	}

	std::string TypedName() const;

	/*
	Is this field conflicted with the given field?
	This method is often used to compare two child fields in a same parent.
	*/
	virtual bool ConflictedWith(const SnField &other) const;

	std::string ToString() const override;

	/*
	Does the given node have access to this field?
	The result depand on SnField::AccessType() and the accessor.
	*/
	bool AllowAccess(const SnField &accessor) const;

	/*
	Get the corresponding LLVM type name.
	@return a empty string if the meta type information is not built.
	The value stored here to meet the need of LLVM::StringRef memory management.
	*/
	const std::string &MetaName() const
	{
		return *m_upMetaName;
	}

#ifdef NLANG_ENABLE_LLVM
	//Get the corresponding LLVM type.
	llvm::Type *MetaType() const
	{
		return m_pMetaType;
	}

	//Get the corresponding LLVM value.
	llvm::Value *MetaValue() const
	{
		return m_pMetaValue;
	}
#endif
protected:
	void Name(const std::string &sName)
	{
		assert(!RuntimeType());
		*m_upName = sName;
	}

	void MetaName(const std::string &sMetaName)
	{
		*m_upMetaName = sMetaName;
	}

	virtual bool AllowPublicAccess(const SnField & accessor) const;

	bool AllowProtectedAccess(const SnField &accessor) const;

	bool AllowPrivateAccess(const SnField &accessor) const;

	void OnAddedToParent(Node& parent) override;
private:
	std::unique_ptr<std::string> m_upName;
	RnField *m_pImportInfo;
	std::unique_ptr<std::string> m_upMetaName;
	uint8 m_uArrayTypeArg = kNoArrayTypeArg;
#ifdef NLANG_ENABLE_LLVM
	llvm::Type *m_pMetaType;
	llvm::Value *m_pMetaValue;
#endif
};

//Composite type syntax node with some member nodes.
class NLANG_COMPILER_API SnCompoundField : public SnField
{
	typedef SnField Super_;
public:
	SnCompoundField(NodeKind k, FieldAccessType at, NodeBits flags,
		std::string *pName, const ISourceLocation &loc);

	//Construct from an existing runtime type.
	explicit SnCompoundField(RnCompoundField &);

	~SnCompoundField() override;
protected:
	ImmutableNodeList *ChildrenPtr() const override;
private:
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

/*
Create a mutable child node list from an auto pointer list and delete the source
list.
*/
template <class NODE_T>
MutableChildNodeList<NODE_T> *CreateChildNodes(
	UniquePtrList<NODE_T> &upSrcNodes, Node *pParent)
{
	return CreateChildList<MutableChildNodeList<NODE_T>>(upSrcNodes, pParent);
}

//Create a child node list from an auto pointer list and delete the source list.
template <class LIST_T>
LIST_T *CreateChildList(UniquePtrList<typename LIST_T::NodeType> &upSrcNodes,
	Node *pParent)
{
	assert(pParent);
	auto pFields = new LIST_T(pParent);
	for (auto pNode : upSrcNodes)
		pFields->push_back(pNode);
	upSrcNodes.Release();
	return pFields;
}

/*
Create a child field list from an auto pointer list and delete the source list.
*/
template <class NODE_T>
inline ChildFieldList<NODE_T> *CreateChildFields(
	UniquePtrList<NODE_T>& upSrcFields, Node *pParent)
{
	return CreateChildList<ChildFieldList<NODE_T>>(upSrcFields, pParent);
}

template <typename T>
using PtrList = std::list<T *>;

} //namespace nlang
