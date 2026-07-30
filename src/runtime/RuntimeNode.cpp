/*-----------------------------------------------------------------------------
ncomp/intf/RnNode.h
This file define the implementation of basic runtime node classes in nlang.
-----------------------------------------------------------------------------*/

#include "RuntimeNode.h"
#include "RnTypes.h"
#include "RnMisc.h"
#include "RuntimeNodeVisitor.h"
#include "Utils.h"
#include <regex>

namespace nlang
{

const NodeTraits RuntimeNode::s_TraitsTable[] = 
{
#define MACRO_IMPL(T)														\
	{ 																		\
		/* kind */															\
		NK_##T,																\
		/* default flags */													\
		Rn##T::s_DefaultFlags,												\
		/* description */													\
		#T																	\
	},
	RUNTIME_NODE_DECL(MACRO_IMPL)
#undef MACRO_IMPL	
	{NK_RT_END, NF_RT_END, "unknown"}
};

RuntimeNode::RuntimeNode(NodeKind k, FieldAccessType at) :
	Super_(k, at, NF_NONE)
{
	const NodeTraits &t = TraitsOf(k);
	Flags(t.m_Flags);
}

void RuntimeNode::Dump(std::ostream &os) const
{
	RuntimeNode &root = const_cast<RuntimeNode &>(*this);
	NodeDumpAccessor accessor(os, root);
	RuntimeNodeVisitor<NodeDumpAccessor> visitor(accessor);
	root.Accept(visitor);
}

RnField::RnField(NodeKind k, FieldAccessType at, const IdString &name) :
	Super_(k, at), m_Name(name)
{
	assert(RnField::IsValidName(name));
}

bool RnField::IsValidName(const IdString &name)
{
	static const std::regex s_Pattern(N_ALL_NAME_PATTERN);
	return std::regex_match(name.ToString(), s_Pattern);
}

bool RnField::ConflictedWith(const RnField &other) const
{
	assert(Parent() && !other.Parent());
	return m_Name == other.m_Name;
}

std::string RnField::ToString() const
{
	return m_Name.ToString();
}

RnCompoundField::RnCompoundField(NodeKind k, FieldAccessType at,
	const IdString &name) :
	Super_(k, at, name), m_upChildren(new ImmutableNodeList())
{
}

RnCompoundField::~RnCompoundField()
{
	// m_upChildren is now unique_ptr - auto-deleted
}

ImmutableNodeList *RnCompoundField::ChildrenPtr() const
{
	return m_upChildren.get();
}

} //namespace nlang