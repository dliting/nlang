#include "CastInfo.h"

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
	if (srcKind >= NK_DT_COUNT || tgtKind >= NK_DT_COUNT)
	{
		m_Kind = TCK_None;
		return;
	}
	const auto k = s_CastTable[srcKind][tgtKind];
	m_Kind = k;
}


} //namespace nlang