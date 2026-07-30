/*-----------------------------------------------------------------------------
ncomp/intf/RnNode.h
This file define the interface of basic runtime node classes in nlang.
-----------------------------------------------------------------------------*/

#pragma once
#include "Node.h"
#include <map>
#include <memory>

namespace nlang
{

struct IRuntimeNodeVisitor;

class NLANG_RUNTIME_API RuntimeNode : public Node
{
	typedef Node Super_;
public:
	RuntimeNode(NodeKind, FieldAccessType);

	//Get the parent.
	RuntimeNode *Parent() const
	{
		assert(!Super_::Parent() || 
			dynamic_cast<RuntimeNode*>(Super_::Parent()));
		return static_cast<RuntimeNode*>(Super_::Parent());
	}

	//Set the parent.
	void Parent(RuntimeNode *pParent)
	{
		Super_::Parent(pParent);
	}

	NodeBits DefaultFlags() const
	{
		return Traits().m_Flags;
	}

	//Get the static traits of this field.
	const NodeTraits &Traits() const
	{
		return TraitsOf(Kind());
	}

	//Get the traits of a specified node kind.
	static const NodeTraits &TraitsOf(NodeKind kind)
	{
		assert(kind < NK_RT_END);
		return s_TraitsTable[kind];
	}

	//Can we add a field as a child of this one?
	virtual bool CanAddChild(const RuntimeNode *pChild) const = 0;

	//Visitor pattern.
	virtual void Accept(IRuntimeNodeVisitor&) = 0;

	void Dump(std::ostream&) const override;

	static const NodeTraits s_TraitsTable[];
};

/*
A field node is a component of a type.
Every field has a name.
*/
class NLANG_RUNTIME_API RnField : public RuntimeNode
{
	typedef RuntimeNode Super_;
public:
	typedef IdString NameType;
public:
	/*
	Construct a named node.
	\param name The node name. If the name is not a valid node name, 
	assertion error will be triggered.
	*/
	RnField(NodeKind k, FieldAccessType, const IdString &name);

	//Get the field name.
	const IdString &Name() const
	{
		return m_Name;
	}

	std::string ToString() const override;

	/*
	Does this node in a node collection conflict with the given one?
	This method is usually used in validation when adding a new node to a
	field collection. \see NNodeCollection::FindConflictedChild().
	\param other The given field to be compared with.
	\note This node should already exist in a collection, while the given one
	should not in a collection.
	*/
	virtual bool ConflictedWith(const RnField &other) const;

	/*
	Validate a name string.
	\param name The name string to be validated.
	\return true if the input string is a valid field name.
	*/
	static bool IsValidName(const IdString &name);
private:
	IdString m_Name;
};

class NLANG_RUNTIME_API RnCompoundField : public RnField
{
	typedef RnField Super_;
public:
	RnCompoundField(NodeKind k, FieldAccessType, const IdString &name);

	~RnCompoundField() override;
protected:
	ImmutableNodeList *ChildrenPtr() const override;
private:
	std::unique_ptr<ImmutableNodeList> m_upChildren;
};

} //namespace nlang