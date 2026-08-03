#include "CastInfo.h"
#include "SnMisc.h"

namespace nlang
{

TypeCastKind TypeCastInfo::s_CastTable[NK_DT_COUNT][NK_DT_COUNT];

void TypeCastInfo::StaticInit()
{
	s_CastTable[NK_Int32][NK_Int32]		= TCK_Same;
	s_CastTable[NK_Int32][NK_Float]		= TCK_Auto;
	s_CastTable[NK_Int32][NK_String]	= TCK_Auto;
	s_CastTable[NK_Int32][NK_Type]		= TCK_None;

	s_CastTable[NK_Float][NK_Int32]		= TCK_Auto;
	s_CastTable[NK_Float][NK_Float]		= TCK_Same;
	s_CastTable[NK_Float][NK_String]	= TCK_Auto;
	s_CastTable[NK_Float][NK_Type]		= TCK_None;

	s_CastTable[NK_String][NK_Int32]	= TCK_None;
	s_CastTable[NK_String][NK_Float]	= TCK_None;
	s_CastTable[NK_String][NK_String]	= TCK_Same;
	s_CastTable[NK_String][NK_Type]		= TCK_None;

	s_CastTable[NK_Type][NK_Int32]		= TCK_None;
	s_CastTable[NK_Type][NK_Float]		= TCK_None;
	s_CastTable[NK_Type][NK_String]		= TCK_None;
	s_CastTable[NK_Type][NK_Type]		= TCK_Same;
#ifndef NDEBUG
	for (size_t i = NK_Int32; i < NK_DT_COUNT; ++i)
		for (size_t j = NK_Int32; j < NK_DT_COUNT; ++j)
		{
			assert(s_CastTable[i][j] != 0 &&
				"The cast table is not initialized properly");
		}
#endif
}

void TypeCastInfo::CalcCastKind()
{
	if (!m_pSource || !m_pTarget)
	{
		m_Kind = TCK_None;
		return;
	}
	auto srcKind = m_pSource->Kind();
	auto tgtKind = m_pTarget->Kind();
	//Enum types are int32 at runtime — treat them as NK_Int32 for casting.
	if (srcKind == NK_EnumDecl) srcKind = NK_Int32;
	if (tgtKind == NK_EnumDecl) tgtKind = NK_Int32;
	//Struct types: same struct type → TCK_Same; otherwise incompatible.
	if (srcKind == NK_StructDecl && tgtKind == NK_StructDecl)
	{
		m_Kind = (m_pSource == m_pTarget) ? TCK_Same : TCK_None;
		return;
	}
	if (srcKind == NK_StructDecl || tgtKind == NK_StructDecl)
	{
		m_Kind = TCK_None;
		return;
	}
	//Class types: same class → TCK_Same; subclass to parent → TCK_Same (implicit); otherwise incompatible.
	if (srcKind == NK_ClassDecl && tgtKind == NK_ClassDecl)
	{
		if (m_pSource == m_pTarget)
		{
			m_Kind = TCK_Same;
			return;
		}
		//Check inheritance chain for implicit conversion.
		auto *pSrc = static_cast<const SnClassDecl*>(m_pSource);
		auto *pParent = pSrc->SuperClass();
		while (pParent)
		{
			if (pParent == m_pTarget)
			{
				m_Kind = TCK_Same;
				return;
			}
			pParent = pParent->SuperClass();
		}
		m_Kind = TCK_None;
		return;
	}
	if (srcKind == NK_ClassDecl || tgtKind == NK_ClassDecl)
	{
		//Allow int (null literal) to be assigned to class type.
		if (srcKind == NK_Int32 && tgtKind == NK_ClassDecl)
		{
			m_Kind = TCK_Auto;
			return;
		}
		m_Kind = TCK_None;
		return;
	}
	if (srcKind >= NK_DT_COUNT || tgtKind >= NK_DT_COUNT)
	{
		m_Kind = TCK_None;
		return;
	}
	const auto k = s_CastTable[srcKind][tgtKind];
	m_Kind = k;
}


} //namespace nlang
