#pragma once
#include "BuildEnvironment.h"
#include "SyntaxNodeVisitor.h"

namespace nlang
{

//A syntax node accessor which keeps the visitor information.
class SyntaxNodeBuildAccessor
{
public:
	SyntaxNodeBuildAccessor(BuildEnvironment &env) : m_Env(env)
	{
	}

	void Visitor(ISyntaxNodeVisitor *pVisitor)
	{
		m_pVisitor = pVisitor;
	}
protected:
	~SyntaxNodeBuildAccessor()
	{
	}

	ISyntaxNodeVisitor *m_pVisitor;
	BuildEnvironment &m_Env;
};

template <class ACCESSOR_T>
class SyntaxNodeVisitorBuilder
{
public:
	typedef ACCESSOR_T AccessorType;
	typedef SyntaxNodeVisitor<AccessorType> VisitorType;
public:
	SyntaxNodeVisitorBuilder(BuildEnvironment &env, NodeVisitKind vk) :
		m_Accessor(env), m_Visitor(m_Accessor, vk)
	{
		m_Accessor.Visitor(&m_Visitor);
	}
protected:
	ACCESSOR_T m_Accessor;
	VisitorType m_Visitor;
};

} //namespace nlang