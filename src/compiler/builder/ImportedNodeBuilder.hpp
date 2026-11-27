#pragma once
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include <nlang/runtime/RuntimeNodeVisitor.h>
#include <nlang/runtime/Runtime.h>
#include <nlang/runtime/StackDump.h>

namespace nlang
{

//Imported syntax node location.
class ImportedNodeLocation : public ISourceLocation
{
public:
	explicit ImportedNodeLocation(const RuntimeNode &rn) :
		m_pRuntimeNode(&rn)
	{
	}

	~ImportedNodeLocation() override
	{
	}

	//create a copy of this location.
	std::unique_ptr<ISourceLocation> Clone() const override
	{
		return std::make_unique<ImportedNodeLocation>(*m_pRuntimeNode);
	}

	std::string ToString() const override
	{
		return "imported node";
	}

	//Get the translation unit of this source location.
	TranslationUnit *TransUnit() const override
	{
		return nullptr;
	}

	const RuntimeNode *RuntimeInfo() const
	{
		return m_pRuntimeNode;
	}
private:
	const RuntimeNode *m_pRuntimeNode;
};

/*
A runtime node accessor implementation which can build syntax nodes.
After visiting a runtime node, the corresponding syntax node is created, but
the references to other syntax nodes may be not resolved since this nodes are
not created yet. A reference to a syntax node is represented as \a SnNameExpr,
which will be resolved in \a SyntaxTree::ResolveTypeNames().
*/
class ImportedNodeBuilder
{
public:
	explicit ImportedNodeBuilder(SyntaxTree &tree) : m_Tree(tree)
	{
	}

	//Template methods invoked the by a node visitor.
	//@{
	void Access(RnNamespace &rn)
	{
		SyntaxNode *pParentNode = ParentNodeOf(rn);
		SnNamespace *pNode = new SnNamespace(rn);
		if (pParentNode)
		{
			NLANG_ASSERT(dynamic_cast<SnNamespace*>(pParentNode));
			auto pParentNamespace = static_cast<SnNamespace*>(pParentNode);
			pParentNamespace->Members().push_back(pNode);
		}
		else
		{
			NLANG_ASSERT(rn.Name() == Runtime::GlobalNamespaceName());
			m_Tree.m_pRoot = pNode;
		}
		RegNode(rn, *pNode);
	}

	void Access(RnFunction &rn)
	{
		SyntaxNode *pParentNode = ParentNodeOf(rn);
		//TODO: parent class
		NLANG_ASSERT(dynamic_cast<SnNamespace*>(pParentNode));
		auto pParentType = static_cast<SnNamespace*>(pParentNode);
		SnFunction *pNode = new SnFunction(rn);
		pParentType->Members().push_back(pNode);
		RegNode(rn, *pNode);
	}

	void Access(RnBuiltinDataType &rn)
	{
		SnBuiltinDataType *pNode;
		switch (rn.Kind())
		{
#define MACRO_IMPL(T)														\
		case NK_##T:															\
			pNode = new Sn##T();												\
			break;
			BUILTIN_TYPE_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL
		default:
			NLANG_ASSERT(false && "invalid primitive type kind");
			pNode = nullptr;
		}
		SyntaxNode *pParentNode = ParentNodeOf(rn);
		NLANG_ASSERT(pParentNode == m_Tree.Root());
		m_Tree.Root()->Members().push_back(pNode);
		//Builtin-mirror name yield: the mirror keeps its node attached —
		//registry identity for SnBuiltinDataType::InstanceOf, tree ownership,
		//walks — but gives up the unqualified name, because that name is the
		//internal identifier the runtime tables stringize from the node kind
		//(`Int32`, `Void`, `Type`), not a spelling of the language: chapter 1
		//lists keywords, the numeric aliases and the declared names, and the
		//canonical diagnostics print those. Indexed, the internal names
		//resolved as types, which admitted `Void v;` (a type with no value
		//representation) and let a `Type`-typed actual reach the instantiation
		//descriptor layer, which defines no spelling for it. The keyword
		//spellings are unaffected: a keyword resolves pre-parsed through
		//SnBuiltinDataType::InstanceOf (runtime-node identity, no name lookup).
		//The prelude's declared face classes (String, Array) keep their own
		//names from their declarations, so DuplicateFieldChecker never sees a
		//pair (the v0.12.2 face-name yield, generalized to every mirror).
		m_Tree.Root()->Members().unindex_field(pNode);
		RegNode(rn, *pNode);
	}

	void Access(RnFormalParam &rn)
	{
		SyntaxNode *pParentNode = ParentNodeOf(rn);
		NLANG_ASSERT(pParentNode && pParentNode->Kind() == NK_Function);
		SnFormalParam *pNode = new SnFormalParam(rn);
		SnFunction *pParentFunc = static_cast<SnFunction*>(pParentNode);
		pParentFunc->Params().push_back(pNode);
		RegNode(rn, *pNode);
	}
	//@}
private:
	//Get the parent syntax node of corresponding to a runtime node.
	SyntaxNode *ParentNodeOf(const RuntimeNode &rn)
	{
		if (!rn.Parent())
			return nullptr;
		const RuntimeNode &parent = *static_cast<const RuntimeNode*>(rn.Parent());
		return m_Tree.FindNode(parent);
	}

	void RegNode(RnField &rn, SnField &sn)
	{
		NLANG_ASSERT(!m_Tree.FindNode(rn));
		m_Tree.RegNode(rn, sn);
	}

	SyntaxTree &m_Tree;
};

} //namespace nlang
