/*-----------------------------------------------------------------------------
	nlang/intf/IdString.cpp
	This file includes the implementation of indexed strings.
-----------------------------------------------------------------------------*/

#include "IdString.h"
#include <mutex>

namespace nlang
{

namespace
{
//K2 slice 0: the intern registry is process-global, so concurrent first
//use from several threads (embedding hosts, the carrier pool) must be
//serialized. Interning concentrates in the compile/symbol phase; the
//lock cost is irrelevant, no read-side optimization.
std::mutex g_internMutex;
} // anonymous namespace

uint32										IdString::s_u4LastIndex	= 0;
std::deque<std::string>						*IdString::s_pIndexes	= nullptr;
std::unordered_map<std::string, uint32>		*IdString::s_pIndexMap	= nullptr;

void IdString::StaticInit()
{
	std::lock_guard<std::mutex> guard(g_internMutex);
	if (s_pIndexes)
		return;   //already initialized — idempotent
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
	std::lock_guard<std::mutex> guard(g_internMutex);
	assert(s_pIndexes && s_pIndexMap);
	const auto item = s_pIndexMap->find(sValue);
	if (item != s_pIndexMap->cend())
	{
		m_u4Index = item->second;
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
