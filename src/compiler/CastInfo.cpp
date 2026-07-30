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
	const auto k = s_CastTable[m_pSource->Kind()][m_pTarget->Kind()];
	//TODO: class cast.
	m_Kind = k;
}


} //namespace nlang