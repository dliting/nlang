/*-----------------------------------------------------------------------------
	ncomp/intf/SyntaxNode.h
	This file includes the definition of common constants and common syntax 
node types in an nlang AST.
-----------------------------------------------------------------------------*/

#include "SyntaxNode.h"
#include "SnStatements.h"
#include "SnExpressions.h"
#include "SnMisc.h"
#include "SyntaxTree.h"
#include "TranslationUnit.h"
#include "SyntaxNodeVisitor.h"
#include <nlang/runtime/Utils.h>

#ifdef NLANG_ENABLE_LLVM
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#endif
#include <sstream>

namespace nlang
{

//Get compile only syntax node traits.
const NodeTraits &GetCompileTraits_(NodeKind k)
{
	static const NodeTraits s_CompileTraitsTable[] = 
	{
#define MACRO_IMPL(T) 														\
		{																	\
			/* kind */														\
			NK_##T,															\
			/* default flags */												\
			Sn##T::s_DefaultFlags,											\
			/* description */												\
			#T																\
		},

		COMPILE_ONLY_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL

		{NK_CP_END, NF_CP_END}
	};
	return s_CompileTraitsTable[k];
}

SyntaxNode::SyntaxNode(NodeKind k, FieldAccessType at, NodeBits flags) :
	Super_(k, at, flags)
{
}

SyntaxNode::SyntaxNode(NodeKind k, FieldAccessType at, NodeBits flags,
	const ISourceLocation &loc) :
	Super_(k, at, flags), m_upLocation(loc.Clone())
{
}

SyntaxNode::SyntaxNode(RuntimeNode &rn) :
	Super_(rn.Kind(), rn.AccessType(), rn.Flags())
{
	AddFlags(NF_IMPORTED_SYMBOL);
}

SyntaxNode::~SyntaxNode()
{
	// m_upLocation is now unique_ptr - auto-deleted
}

const NodeTraits &SyntaxNode::TraitsOf(NodeKind k)
{
	assert(k < NK_CP_END);
	return k < NK_RT_END ? RuntimeNode::TraitsOf(k) : GetCompileTraits_(k);
}

const UsingList *SyntaxNode::Usings() const
{
	auto pTransUnit = Location()->TransUnit();
	return pTransUnit ? pTransUnit->Usings() : nullptr;
}

bool SyntaxNode::ReplaceChildNode(SyntaxNode *pOld, SyntaxNode *pNew)
{
	assert(pOld && pOld->Parent() == this);
	assert(!pNew || !pNew->Parent());

	auto &children = Children();
	auto iRemove = children.find(pOld);
	if (iRemove == children.end())
		return false;
	auto iInsert = RemoveChild(iRemove);
	delete pOld;
	if (pNew)
		InsertChild(iInsert, pNew);
	return true;
}

SyntaxNode *SyntaxNode::DetachChild(SyntaxNode *pChild)
{
	if (!pChild || pChild->Parent() != this)
		return nullptr;
	auto &children = Children();
	auto iRemove = children.find(pChild);
	if (iRemove == children.end())
		return nullptr;
	RemoveChild(iRemove);
	return pChild;
}

void SyntaxNode::Dump(std::ostream &os) const
{
	SyntaxNode &root = const_cast<SyntaxNode&>(*this);
	NodeDumpAccessor accessor(os, root);
	SyntaxNodeVisitor<NodeDumpAccessor> visitor(accessor);
	root.Accept(visitor);
}

SyntaxNode *SyntaxNode::DoResetChild(SyntaxNode *pOld, SyntaxNode *pNew)
{
	assert(!pOld || pOld->Parent() == this);
	assert(!pNew || !pNew->Parent());

	if (pOld)
	{
		auto iRemove = Children().find(pOld);
		assert(iRemove != Children().end());
		auto iInsert = RemoveChild(iRemove);
		delete pOld;
		if (pNew)
			InsertChild(iInsert, pNew);
	}
	else
	{
		if (pNew)
			AddChild(pNew);
	}
	return pNew;
}

SnField::SnField(NodeKind k, FieldAccessType at, NodeBits flags,
	std::string *pName, const ISourceLocation &loc) :
	Super_(k, at, flags, loc), m_upName(pName), m_pImportInfo(nullptr),
	m_upMetaName(std::make_unique<std::string>())
#ifdef NLANG_ENABLE_LLVM
	, m_pMetaType(nullptr), m_pMetaValue(nullptr)
#endif
{
	assert(pName);
}

SnField::SnField(RnField &rn) :
	Super_(rn), m_upName(nullptr), m_pImportInfo(&rn),
	m_upMetaName(std::make_unique<std::string>())
#ifdef NLANG_ENABLE_LLVM
	, m_pMetaType(nullptr), m_pMetaValue(nullptr)
#endif
{
	AccessType(rn.AccessType());
	Flags(rn.Flags());
}

SnField::~SnField()
{
	// m_upName and m_upMetaName are now unique_ptr - auto-deleted
}

std::string SnField::TypedName() const
{
	std::stringstream ss;
	ss << TraitsOf(Kind()).m_szDesc << " " << Name();
	return ss.str();
}

std::string SnField::ToString() const
{
	if (MetaName().empty())
		return Name();
	std::stringstream ss;
	ss << Name() << "\t//" << MetaName();
	return ss.str();
}

bool SnField::AllowAccess(const SnField &accessor) const
{
	switch (AccessType())
	{
	case FA_Public:
		return AllowPublicAccess(accessor);
	case FA_Protected:
		return AllowProtectedAccess(accessor);
	default:
		assert(AccessType() == FA_Private);
		return AllowPrivateAccess(accessor);
	}
}

bool SnField::AllowPublicAccess(const SnField &accessor) const
{
	if (AllowProtectedAccess(accessor))
		return true;

	auto pParent = Parent();
	if (!pParent || !pParent->IsField())
		return true;  //Parent is a paragraph or namespace, allow access
	auto pParentField = static_cast<const SnField *>(pParent);
	return pParentField->AllowAccess(accessor);
}

bool SnField::AllowProtectedAccess(const SnField &accessor) const
{
	if (AllowPrivateAccess(accessor))
		return true;
	return false; //TODO
}

bool SnField::AllowPrivateAccess(const SnField &accessor) const
{
	return (&accessor == this) || &accessor == Parent() ||
		accessor.Parent() == Parent() || accessor.IsDecedentOf(*this);
}

void SnField::OnAddedToParent(Node& parent)
{
	if (AccessType() != FA_Default)
		return;
	if (parent.Kind() == NK_Namespace)
		AccessType(FA_Public);
	else
		AccessType(FA_Private);
}

bool SnField::ConflictedWith(const SnField &other) const
{
	assert(&other != this);
	return other.Name() == Name();
}

SnCompoundField::SnCompoundField(NodeKind k, FieldAccessType at,
	NodeBits flags, std::string *pName, const ISourceLocation &loc) :
	Super_(k, at, flags, pName, loc), m_upChildren(new ImmutableNodeList())
{
}

SnCompoundField::SnCompoundField(RnCompoundField &rn) :
	Super_(rn), m_upChildren(new ImmutableNodeList())
{
}

SnCompoundField::~SnCompoundField()
{
	// m_upChildren is now unique_ptr - auto-deleted
}

ImmutableNodeList *SnCompoundField::ChildrenPtr() const
{
	return m_upChildren.get();
}

} //namespace nlang

