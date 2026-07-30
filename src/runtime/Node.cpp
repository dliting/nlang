/*-----------------------------------------------------------------------------
ncomp/intf/Node.cpp
This file define the implementation of basic node classes in nlang.
-----------------------------------------------------------------------------*/

#include "Node.h"
#include "NodeContainers.h"

namespace nlang
{

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
	assert(pNode && !pNode->Parent());
	Children().push_back(pNode);
	pNode->m_pParent = this;
	pNode->OnAddedToParent(*this);
}

NodeIterator Node::InsertChild(NodeIterator& iPos, Node *pNode)
{
	assert(pNode && !pNode->Parent());
	pNode->m_pParent = this;
	auto it = Children().insert(iPos, pNode);
	pNode->OnAddedToParent(*this);
	return it;
}

NodeIterator Node::RemoveChild(NodeIterator& iPos)
{
	assert(iPos != Children().end() && iPos->Parent() == this);
	iPos->m_pParent = nullptr;
	return Children().erase(iPos);
}

NodeIterator Node::RemoveChild(Node *pNode)
{
	assert(pNode && pNode->Parent() == this);
	auto iRemove = Children().find(pNode); //TODO: optimize it
	return RemoveChild(iRemove);
}

void Node::OnAddedToParent(Node& parent)
{
	//Do nothing by default.
}

} //namespace nlang