/*-----------------------------------------------------------------------------
	ncomp/SnArrayTypeToken.cpp
	Implementation of the interned array type token (0.7.3 B).
-----------------------------------------------------------------------------*/

#include "SnArrayTypeToken.h"
#include "SyntaxNodeVisitor.h"
#include "SnMisc.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/StackDump.h>

namespace nlang
{

SnArrayTypeToken::SnArrayTypeToken(SnField *pElemType,
	const ISourceLocation &loc, uint8_t rank) :
	//Interned tokens are synthetic (no source position of their own)
	//and carry an empty name — the display path is ToString().
	Super_(s_Kind, FA_Public, s_DefaultFlags, new std::string(), loc),
	m_pElemType(pElemType), m_Rank(rank)
{
	NLANG_ASSERT(pElemType);
	NLANG_ASSERT(m_Rank >= 1);
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

//v0.12.7: Array<E> is the generic spelling of the single array type
//(spec §4.1) — the prelude template mints the instantiation, and its
//one type argument IS the element type. Same bare-BaseName policy as
//the List/Dict erasure keys (TypeDesc.cpp). Declared in
//SnExpressions.h beside its consumer (SnExpression::IsArrayValued).
SnField *ArrayInstantiationElem(SnField *pType)
{
	if (pType->Kind() != NK_ClassDecl)
		return nullptr;
	auto* pClass = static_cast<SnClassDecl*>(pType);
	if (!pClass->IsGenericInstantiation()
		|| pClass->BaseName() != kBuiltinArrayTypeName)
		return nullptr;
	const auto& args = pClass->GenericTypeArgs();
	return args.size() == 1 ? args[0] : nullptr;
}

//v0.12.7: array-ness now covers both spellings of the one type.
bool SnExpression::IsArrayValued() const
{
	auto* pType = EvalDataType();
	return pType && (pType->Kind() == NK_ArrayTypeToken
		|| ArrayInstantiationElem(pType) != nullptr);
}

//v0.12.7: declared in SnExpressions.h. Node equality, or token ×
//Array<E> unification recursing on the elements (so nested spellings,
//Array<Array<int>> vs int[][], unify through the same rule). Annotated
//x annotated compares ranks — the annotation is part of the promise,
//so double[] and double[,] stay distinct types at every compile-time
//check (overload distance included); only the unannotated generic
//spelling matches any rank. Elements compare by node identity only:
//the primitive carriers are process singletons and declarations are
//unique per build, so pointer equality IS type identity here —
//enum/keyword widening must never apply to array elements (invariance).
bool SameArrayType(SnField *pLeft, SnField *pRight)
{
	if (pLeft == pRight)
		return true;
	const bool leftToken = pLeft->Kind() == NK_ArrayTypeToken;
	const bool rightToken = pRight->Kind() == NK_ArrayTypeToken;
	if (leftToken && rightToken
		&& static_cast<SnArrayTypeToken*>(pLeft)->Rank()
			!= static_cast<SnArrayTypeToken*>(pRight)->Rank())
		return false;
	SnField* pLeftElem = leftToken
		? static_cast<SnArrayTypeToken*>(pLeft)->ElemTypeOf()
		: ArrayInstantiationElem(pLeft);
	SnField* pRightElem = rightToken
		? static_cast<SnArrayTypeToken*>(pRight)->ElemTypeOf()
		: ArrayInstantiationElem(pRight);
	return pLeftElem && pRightElem && SameArrayType(pLeftElem, pRightElem);
}

} //namespace nlang
