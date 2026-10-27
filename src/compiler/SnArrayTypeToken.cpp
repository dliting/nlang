/*-----------------------------------------------------------------------------
	ncomp/SnArrayTypeToken.cpp
	Implementation of the interned array type token (0.7.3 B).
-----------------------------------------------------------------------------*/

#include "SnArrayTypeToken.h"
#include "SyntaxNodeVisitor.h"

namespace nlang
{

SnArrayTypeToken::SnArrayTypeToken(SnField *pElemType,
	const ISourceLocation &loc, uint8_t rank) :
	//Interned tokens are synthetic (no source position of their own)
	//and carry an empty name — the display path is ToString().
	Super_(s_Kind, FA_Public, s_DefaultFlags, new std::string(), loc),
	m_pElemType(pElemType), m_Rank(rank)
{
	assert(pElemType);
	assert(m_Rank >= 1);
}

void SnArrayTypeToken::Accept(ISyntaxNodeVisitor &v)
{
	v.Visit(*this);
}

std::string SnArrayTypeToken::ToString() const
{
	std::string suffix = m_Rank == 1 ? "[]" : "[" + std::string(m_Rank - 1, ',') + "]";
	return m_pElemType->ToString() + suffix;
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
