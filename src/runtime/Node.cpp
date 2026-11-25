/*-----------------------------------------------------------------------------
ncomp/intf/Node.cpp
This file define the implementation of basic node classes in nlang.
-----------------------------------------------------------------------------*/

#include "Node.h"
#include "NodeContainers.h"
#include <nlang/runtime/StackDump.h>

namespace nlang
{

//Definitions of the pure virtual destructors: a pure virtual function cannot
//carry a body in its class definition ([class.pure]), and base-subobject
//destruction in derived destructors calls them, so the definitions must
//exist.
INodeVisitor::~INodeVisitor() {}

Node::~Node() {}

Node::Node(NodeKind k, FieldAccessType at, NodeBits flags) :
	m_Kind(k), m_AccessType(at), m_Flags(flags), m_pParent(nullptr), 
	m_pNext(nullptr)
{
}

bool Node::IsDecedentOf(const Node& other) const
{
	auto pParent = m_pParent;
	while (pParent)
	{
		if (pParent == &other)
			return true;
		pParent = pParent->Parent();
	}
	return false;
}

void Node::Parent(Node *pParent)
{
	if (pParent == m_pParent)
		return;

	//Remove form the old parent.
	auto pOldParent = Parent();
	if (pOldParent)
		pOldParent->RemoveChild(this);

	//Add to the new parent.
	if (pParent)
		pParent->AddChild(this);
}

void Node::AddChild(Node *pNode)
{
	NLANG_ASSERT(pNode && !pNode->Parent());
	Children().push_back(pNode);
	pNode->m_pParent = this;
	pNode->OnAddedToParent(*this);
}

NodeIterator Node::InsertChild(const NodeIterator& iPos, Node *pNode)
{
	NLANG_ASSERT(pNode && !pNode->Parent());
	pNode->m_pParent = this;
	auto it = Children().insert(iPos, pNode);
	pNode->OnAddedToParent(*this);
	return it;
}

void Node::ReplaceChildCell(const NodeIterator &iPos, Node *pNode)
{
	NLANG_ASSERT(iPos != Children().end() && iPos->Parent() == this);
	NLANG_ASSERT(pNode && !pNode->Parent());
	Children().assign(iPos, pNode);
	pNode->m_pParent = this;
	pNode->OnAddedToParent(*this);
}

NodeIterator Node::RemoveChild(const NodeIterator& iPos)
{
	NLANG_ASSERT(iPos != Children().end() && iPos->Parent() == this);
	iPos->m_pParent = nullptr;
	return Children().erase(iPos);
}

NodeIterator Node::RemoveChild(Node *pNode)
{
	NLANG_ASSERT(pNode && pNode->Parent() == this);
	auto iRemove = Children().find(pNode); //TODO: optimize it
	return RemoveChild(iRemove);
}

void Node::OnAddedToParent(Node& parent)
{
	//Do nothing by default.
}

} //namespace nlang