/*-----------------------------------------------------------------------------
	nlang/intf/NodeVisitor.h
	This file define the visitor interface of an runtime node.
-----------------------------------------------------------------------------*/
#pragma once
#include "RuntimeNode.h"
#include "RnMisc.h"
#include "RnTypes.h"

namespace nlang
{

struct IRuntimeNodeVisitor : public INodeVisitor
{
	//Implement all the visit methods.
#define MACRO_IMPL(T) \
	virtual void Visit(Rn##T &node)	= 0;											
	RUNTIME_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL															
};

/*
A preorder traverse runtime node visitor with customized access mechanism.
\param ACCESSOR_T The accessor class perform actual visits.
The accessor should implements the method Access(N), where N is a node type. 
*/
template<class ACCESSOR_T>
class RuntimeNodeVisitor : public IRuntimeNodeVisitor
{
	typedef ACCESSOR_T	AccessorType;
	typedef RuntimeNode	NodeType;
public:
	explicit RuntimeNodeVisitor(AccessorType &accessor) : 
		m_Accessor(accessor)
	{
	}

	//Visit a node and its children.
#define MACRO_IMPL(T)														\
	void Visit(Rn##T &rn)	override										\
	{																		\
		m_Accessor.Access(rn);												\
		VisitChildren(rn.Children());										\
	}
	RUNTIME_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL		
private:
	void VisitChildren(ImmutableNodeList &children)
	{
		if (children.empty())
			return;
		for (auto& child : children)
			static_cast<NodeType &>(child).Accept(*this);
	}
private:
	AccessorType &m_Accessor;
};

}// namespace nlang 
