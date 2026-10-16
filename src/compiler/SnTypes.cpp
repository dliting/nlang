/*-----------------------------------------------------------------------------
	ncomp/intf/SnTypes.h
	This file define the implementation of data type syntax nodes in an nlang 
AST.
-----------------------------------------------------------------------------*/

#include "SnTypes.h"
#include "SyntaxNodeVisitor.h"
#include "SyntaxTree.h"

namespace nlang
{

SnBuiltinDataType::SnBuiltinDataType(RnBuiltinDataType &rtti) : Super_(rtti)
{
	//The RTTI side carries NF_DontDelete because runtime-side built-ins are
	//process-lifetime singletons. This syntax node is not: it hangs under
	//the per-build AST root, so the tree owns it and SyntaxTree::Clear
	//deletes it, which also resets the s_pInstance construction guard in
	//~SnBuiltinDataTypeT. Keeping the copied flag would leak the node on
	//every build, and a second build in the same process would then trip
	//the guard's assert (Debug) or silently overwrite the singleton
	//(Release).
	RemoveFlags(NF_DontDelete);
}

std::string SnBuiltinDataType::ToString() const
{
	return RuntimeType()->ToString();
}

SnBuiltinDataType * SnBuiltinDataType::InstanceOf(NodeKind builtinKind)
{
	assert(IsBuiltinType(builtinKind));
	auto pRTTI = RnBuiltinDataType::InstanceOf(builtinKind);
	assert(pRTTI);
	return static_cast<SnBuiltinDataType *>(TheAST().FindNode(*pRTTI));
}

SnField *SnBuiltinDataType::FindField(const std::string& sName) const
{
	return nullptr;
}

bool SnBuiltinDataType::AllowPublicAccess(const SnField &accessor) const
{
	return true;
}

ImmutableNodeList *SnBuiltinDataType::ChildrenPtr() const
{
	return ImmutableNodeList::NullList();
}

SnField * SnBuiltinDataType::EvalDataType() const
{
	return SnType::Instance(); 
}

void SnInt32::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

void SnFloat::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

void SnString::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

//Accept one-liners for the ten registry-generated scalar type nodes
//(same shape as the hand-written SnInt32/SnFloat above).
#define IMPL_SCALAR_SN_ACCEPT(CLASS, KW, WIDTH, CARRIER, CAT, RANK)          \
void Sn##CLASS::Accept(ISyntaxNodeVisitor &v)                                \
{                                                                            \
	v.Visit(*this);                                                          \
}
SCALAR_PRIMITIVE_NEW_DECL(IMPL_SCALAR_SN_ACCEPT)
#undef IMPL_SCALAR_SN_ACCEPT

void SnType::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

void SnVoid::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

}
