/*-----------------------------------------------------------------------------
	ncomp/intf/SyntaxTree.h
	This file define the interface of an nlang abstract syntax tree.
-----------------------------------------------------------------------------*/

#pragma once
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnTypes.h"
#include <map>
#include <memory>

namespace nlang
{

class BuildEnvironment;

//The abstract syntax tree for nlang compilation.
class NLANG_COMPILER_API SyntaxTree : CopyDisabled
{
	friend class ImportedNodeBuilder;
	typedef std::map<const RuntimeNode*, SyntaxNode*> NodeRegistry;
public:
	SyntaxTree();

	~SyntaxTree();

	//Get the root namespace.
	SnNamespace *Root() const
	{
		return m_pRoot;
	}

	//Get node a by meta node.
	SyntaxNode *FindNode(const RuntimeNode &rtti) const
	{
		auto it = m_upNodeMap->find(&rtti);
		return it == m_upNodeMap->cend() ? 0 : it->second;
	}

	//Get the cast node type by a meta field.
	template<class FIELD_T>
	FIELD_T *FindNodeT(const RuntimeNode &rtti) const
	{
		SyntaxNode *pNode = FindNode(rtti);
		assert(dynamic_cast<FIELD_T*>(pNode));
		return static_cast<FIELD_T*>(pNode);
	}

	void RegNode(const RuntimeNode &rtti, SyntaxNode &node)
	{
		assert(m_upNodeMap->find(&rtti) == m_upNodeMap->cend());
		m_upNodeMap->emplace(std::make_pair(&rtti, &node));
	}
	
	//Destroy all nodes.
	void Clear();

	//Rebuild the tree from existing runtime information.
	void BuildFromRuntime(BuildEnvironment &);

	//Print the tree nodes.
	void Print(std::ostream &os);

	static SyntaxTree s_Instance;
private:
	//Build type name references of existing nodes.
	void CreateNodesFromRuntime();
	void ResolveNamesFromRuntime(BuildEnvironment &env);
	SnNamespace *m_pRoot;
	std::unique_ptr<NodeRegistry> m_upNodeMap;
};

//Get the global syntax tree instance.
inline SyntaxTree &TheAST()
{
	return SyntaxTree::s_Instance;
}

} //namespace nlang
