/*-----------------------------------------------------------------------------
	nlang/intf/SyntaxNodeVisitor.h
	This file define the interface of a visitor of syntax nodes in nlang.
-----------------------------------------------------------------------------*/
#pragma once
#include "SnTypes.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"

namespace nlang
{

//A syntax nodes visitor using the Visitor design pattern.
struct ISyntaxNodeVisitor : INodeVisitor
{
//Declare all the visit methods.
#define MACRO_IMPL(T) \
	virtual void Visit(Sn##T&) =  0;
	SYNTAX_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL
};

enum NodeVisitKind : uint8
{
	NVK_DefaultTranverse	= 1,
	NVK_CustomTraverse		= 2
};

/*
A preorder traverse syntax node visitor with customized access mechanism.
\param ACCESSOR_T The accessor class perform actual visits.
The accessor should implements the method Access(N), where N is a node type.
*/
template<class ACCESSOR_T>
class SyntaxNodeVisitor : public ISyntaxNodeVisitor
{
	typedef ACCESSOR_T	AccessorType;
	typedef SyntaxNode	NodeType;
public:
	explicit SyntaxNodeVisitor(AccessorType &accessor, 
		NodeVisitKind vk = NVK_DefaultTranverse) :
		m_Accessor(accessor), m_VisitKind(vk)
	{
	}

	//Visit a node and its children.
#define MACRO_IMPL(T)														\
	void Visit(Sn##T &sn)	override										\
	{																		\
		m_Accessor.Access(sn);												\
		if (m_VisitKind == NVK_DefaultTranverse)							\
			VisitChildren(sn.Children());									\
	}
	SYNTAX_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL		
private:
	void VisitChildren(ImmutableNodeList &children)
	{
		if (children.empty())
			return;
		for (auto &child : children)
			static_cast<NodeType &>(child).Accept(*this);
	}
private:
	NodeVisitKind m_VisitKind;
	AccessorType &m_Accessor;
};


} //namespace nlang
