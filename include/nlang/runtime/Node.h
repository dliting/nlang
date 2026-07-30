/*-----------------------------------------------------------------------------
nlang/intf/Node.h
This file define the interface of basic node classes in nlang.
-----------------------------------------------------------------------------*/

#pragma once
#include "IdString.h"
#include "NodeIterators.h"
#include "NodeConsts.h"
#include <functional>

namespace nlang
{

//Static trait information of node.
struct NodeTraits
{
	NodeKind			m_Kind;
	NodeBits			m_Flags;			//Default flags.
	const char*			m_szDesc;			//Description.
};

//The interface of a node visitor.
struct INodeVisitor
{
	virtual ~INodeVisitor() = 0 {}
};

/*
The basic element in a type information tree.
It's a base class provides some bits access methods.
*/
class NLANG_RUNTIME_API Node : private CopyDisabled
{
	friend class ChildNodeListController;
public:
	Node(NodeKind k, FieldAccessType at, NodeBits flags);

	virtual ~Node() = 0 {};

	NodeKind Kind() const
	{
		return m_Kind;
	}

	//Field access type methods only available in field nodes.
	//@}
	FieldAccessType AccessType() const
	{
		return static_cast<FieldAccessType>(m_AccessType);
	}

	void AccessType(FieldAccessType at)
	{
		m_AccessType = at;
	}
	//@}

	//Is this node a type declaration?
	bool IsTypeField() const
	{
		return ContainFlags(NF_Type | NF_Field);
	}

	//Is this node a data field?
	bool IsDataField() const
	{
		return ContainFlags(NF_Data | NF_Field);
	}

	bool IsOptional() const
	{
		return ContainFlags(NF_Optional);
	}
		
	bool IsStatic() const
	{
		return ContainFlags(NF_Static);
	}

	bool IsField() const
	{
		return ContainFlags(NF_Field);
	}

	bool IsExternal() const
	{
		return ContainFlags(NF_External);
	}

	bool IsReference() const
	{
		return ContainFlags(NF_Reference);
	}
	
	//Is this node a decedent of the given node?
	bool IsDecedentOf(const Node& other) const;

	//Is this node a ancestor of the given node?
	bool IsAncestorOf(const Node& other) const
	{
		return other.IsDecedentOf(*this);
	}

	bool IsDataType() const
	{
		return m_Kind < NK_DT_COUNT;
	}

	bool IsPrimitiveType() const
	{
		return ::nlang::IsPrimitiveType(m_Kind);
	}

	//Get the parent of this node.
	Node *Parent() const
	{
		return m_pParent;
	}

	//Get the children nodes list.
	//@{
	ImmutableNodeList &Children()
	{
		return *ChildrenPtr();
	}

	const ImmutableNodeList &Children() const
	{
		return *ChildrenPtr();
	}
	//@}

	virtual std::string ToString() const = 0;

	//Output the contents of a node and its descendants.
	virtual void Dump(std::ostream&) const = 0;

	BIT_SET_METHODS_IMPL(NodeBits, Flags);
protected:
	void Kind(NodeKind k)
	{
		m_Kind = k;
	}

	/*
	Set the parent.
	If the parent of the this node will be changed and is not null, this node 
	will be removed form the old parent firstly, and then add to the new parent.
	*/
	void Parent(Node *pParent);

	/*
	Get the pointer to the children list implementation.
	A decedent class of this one can return a custom child list or just return 
	a null list(ChildNodeList::Instance()).
	The return pointer should not be null.
	*/
	virtual ImmutableNodeList *ChildrenPtr() const = 0;

	/*
	Children modification methods.
	Instead of be public accessed by client application, these methods should 
	be accessed by its subclasses which provide wrapper public methods of these
	ones. e. g., given a parameter node will be a child of a function node, add 
	a parameter to a function like this: 
		f.Params().Add(p);
	The above statement will call Node::AddChild() internally.
	@{
	*/

	/*
	Add a node as a child of this node and take the ownership of it.
	Precondition: pNode && !pNode->Parent().
	The node will be added to the end of the children list.
	*/
	void AddChild(Node *pNode);

	/*
	Add a node to as a child to the children list and take the ownership of it.
	Precondition: pNode && !pNode->Parent().
	The node will be added before the location pointed by iPos.
	\return The iterator pointing to the inserted node. 
	*/
	NodeIterator InsertChild(NodeIterator& iPos, Node *pNode);

	/*
	Remove a child from this node and release the ownership of it.
	Precondition: iPos != Children().end() && (*iPos)->Parent() == this.
	\return The iterator following the removed node.
	\note Remove a child will not destroy it.
	*/
	NodeIterator RemoveChild(NodeIterator& iPos);

	/*
	Remove a child from this node and release the ownership of it.
	Precondition: pNode && pNode->Parent() == this.
	\return The iterator following the removed node.
	\note Remove a child will not destroy it.
	*/
	NodeIterator RemoveChild(Node *pNode);

	/*
	Replace the child at the give position.
	The effect of this mothod is the combination of RemoveChild() and 
	InsertChild().
	\return The iterator pointing to the new node.
	*/
	NodeIterator ReplaceChild(NodeIterator iPos, Node *pNode)
	{
		auto iRemoved = RemoveChild(iPos);
		return InsertChild(iRemoved, pNode);
	}
	//@}

	//Callback method after this node is added to a parent.
	virtual void OnAddedToParent(Node& parent);
private:
	NodeBits m_Kind : NODE_KIND_BITS;
	NodeBits m_AccessType	: FIELD_ACCESS_BITS;
	NodeBits m_Flags		: NODE_FLAG_BITS;
	Node *m_pParent;
	Node *m_pNext; //Next sibling node of the same parent.
};

inline void DeleteNode(Node *pNode)
{
	if (pNode && !pNode->ContainFlags(NF_DontDelete))
		delete pNode;
}

template<class NODE_T>
inline void ResetNode(NODE_T *&pDst, NODE_T *pSrc)
{
	if (pDst == pSrc)
		return;
	DeleteNode(pDst);
	pDst = pSrc;
}

//Node deletion functor.
struct NodeDeleter
{
	void operator()(Node *pNode)
	{
		DeleteNode(pNode);
	}
};

} //namespace nlang