/*-----------------------------------------------------------------------------
	ncomp/SnArrayTypeToken.cpp
	Implementation of the interned array type token (0.7.3 B).
-----------------------------------------------------------------------------*/

#include "SnArrayTypeToken.h"
#include "SyntaxNodeVisitor.h"

namespace nlang
{

SnArrayTypeToken::SnArrayTypeToken(SnField *pElemType,
	const ISourceLocation &loc) :
	//Interned tokens are synthetic (no source position of their own)
	//and carry an empty name — the display path is ToString().
	Super_(s_Kind, FA_Public, s_DefaultFlags, new std::string(), loc),
	m_pElemType(pElemType)
{
	assert(pElemType);
}

void SnArrayTypeToken::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnArrayTypeToken::ToString() const
{
	return m_pElemType->ToString() + "[]";
}

SnField *SnArrayTypeToken::FindField(const std::string&) const
{
	return nullptr;
}

//The element is a non-owning reference, so it must NOT enter the
//auto-deleting children list — the empty list keeps the containment
//walks from touching (and owning) the element.
ImmutableNodeList *SnArrayTypeToken::ChildrenPtr() const
{
	return ImmutableNodeList::NullList();
}

} //namespace nlang
