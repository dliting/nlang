/*-----------------------------------------------------------------------------
	ncomp/intf/SyntaxTree.cpp
	This file define the implementation of an nlang abstract syntax tree.
-----------------------------------------------------------------------------*/

#include "SyntaxTree.h"
#include "SnTypes.h"
#include "Utils.h"
#include "BuildEnvironment.h"
#include "builder/ImportedNodeBuilder.hpp"
#include "builder/ExprResolver.h"

namespace nlang
{

SyntaxTree SyntaxTree::s_Instance;

SyntaxTree::SyntaxTree() :
	m_pRoot(nullptr), m_upNodeMap(new NodeRegistry())
{
}

SyntaxTree::~SyntaxTree()
{
	// m_pRoot may already have been cleared by ModuleBuilder destructor.
	// If not, delete it here.
	if (m_pRoot)
	{
		m_upNodeMap->clear();
		delete m_pRoot;
		m_pRoot = nullptr;
	}
}

void SyntaxTree::Print(std::ostream &os)
{
	if (!m_pRoot)
		return;
	m_pRoot->Dump(os);
}

void SyntaxTree::BuildFromRuntime(BuildEnvironment &env)
{
	CreateNodesFromRuntime();
	ResolveNamesFromRuntime(env);
}

void SyntaxTree::Clear()
{
	delete m_pRoot;
	m_pRoot = nullptr;
	m_upNodeMap->clear();
}

void SyntaxTree::CreateNodesFromRuntime()
{

	assert(m_pRoot == nullptr);
	//Build type information except type name references from imported node.
	ImportedNodeBuilder builder(*this);
	RuntimeNodeVisitor<ImportedNodeBuilder> visitor(builder);
	Runtime::GlobalNamespace().Accept(visitor);
}

void SyntaxTree::ResolveNamesFromRuntime(BuildEnvironment &env)
{
	ExprResolver resolver(env);
	for (auto pair : *m_upNodeMap)
	{
		auto pNode = pair.second;
		if (pNode->Kind() != NK_NameExpr)
			continue;
		assert(!pNode->IsResolved() && pNode->IsImported());
		auto pNameExpr = static_cast<SnNameExpr*>(pNode);
		assert(pNameExpr->ImportedField());
		SyntaxNode *pType = FindNode(*pNameExpr->ImportedField());
		assert(pType && pType->IsField());
		resolver.ResolveFieldExprAs(*pNameExpr, static_cast<SnField*>(pType));
	}
}

} //namespace nlang