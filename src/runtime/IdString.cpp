/*-----------------------------------------------------------------------------
	nlang/intf/IdString.cpp
	This file includes the implementation of indexed strings.
-----------------------------------------------------------------------------*/

#include "IdString.h"

namespace nlang
{

uint32										IdString::s_u4LastIndex	= 0;
std::deque<std::string>						*IdString::s_pIndexes	= nullptr;
std::unordered_map<std::string, uint32>		*IdString::s_pIndexMap	= nullptr;

void IdString::StaticInit()
{
	static std::deque<std::string> s_Indexes(IdString::INIT_INDEX_COUNT);
	s_pIndexes = &s_Indexes;
	static std::unordered_map<std::string, uint32> s_IndexMap(IdString::INIT_INDEX_COUNT);
	s_pIndexMap = &s_IndexMap;
}

IdString::IdString(const std::string& sValue)
{
	Init(sValue);
}

IdString::IdString(const char* szValue)
{
	std::string s(szValue);
	Init(s);
}

void IdString::Init(const std::string& sValue)
{
	assert(s_pIndexes && s_pIndexMap);
	const auto item = s_pIndexMap->find(sValue);
	if (item != s_pIndexMap->cend())
	{
		m_u4Index = (*s_pIndexMap)[sValue];
		return;
	}

	if (s_u4LastIndex == s_pIndexes->size())
	{
		//Expand the indexes list.
		const uint32 u4NewSize = s_u4LastIndex + IdString::INIT_INDEX_COUNT;
		assert(u4NewSize <= s_pIndexes->max_size());
		s_pIndexes->resize(u4NewSize);
	}

	m_u4Index = s_u4LastIndex++;
	(*s_pIndexes)[m_u4Index] = sValue;
	(*s_pIndexMap)[sValue] = m_u4Index;
}

} //namespace nlang
