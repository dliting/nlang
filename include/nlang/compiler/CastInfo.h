/*-----------------------------------------------------------------------------
ncomp/intf/CastInfo.h
This file define the interface of the type cast information.
-----------------------------------------------------------------------------*/
#pragma once
#include "SyntaxNode.h"

namespace nlang
{

enum TypeCastKind
{
	TCK_None = 1,	//No such cast.
	TCK_Same,		//Same types and need not to cast.
	TCK_Auto,		//Automatic type cast.
	TCK_Dynamic		//Coerce dynamic type cast.
};

class NLANG_COMPILER_API TypeCastInfo
{
public:
	TypeCastInfo(SnField *pSrc, SnField *pTgt) :
		m_pSource(pSrc), m_pTarget(pTgt)
	{
		CalcCastKind();
	}

	TypeCastKind Kind() const
	{
		return m_Kind;
	}

	SnField *Source() const
	{
		return m_pSource;
	}

	SnField *Target() const
	{
		return m_pTarget;
	}

	static void StaticInit();
private:
	void CalcCastKind();

	static TypeCastKind s_CastTable[NK_DT_COUNT][NK_DT_COUNT];

	TypeCastKind m_Kind;
	SnField *m_pSource;
	SnField *m_pTarget;
};

inline TypeCastInfo GetCastInfo(SnField *pSrc, SnField *pDst)
{
	return TypeCastInfo(pSrc, pDst);
}

} //namespace nlang