/*-----------------------------------------------------------------------------
	nlang/intf/IdString.h
	This file define the interface of indexed strings.
-----------------------------------------------------------------------------*/
#pragma once
#include "TypeDef.h"
#include <string>
#include <deque>
#include <unordered_map>
#include <cassert>

namespace nlang
{

/*
 The class represent a fixed string witch can be accessed by index.

An IndStr has an integer index and a reference to a fixed string. We can 
compare strings by indexes instead of do it byte-by-byte to gain higher 
performance. 

The value of the string is stored in a global registry per process, and the
index of an IndStr is depend on its create order. As it turned out, the indexes 
of the same string value are likely to be different in two processes.
*/
class NLANG_RUNTIME_API IdString 
{
public:
	static const size_t INIT_INDEX_COUNT = 4096;

	//A functor behaves like std::less and comparing two strings by their 
	//Names. 
	struct ValueLess
	{
		bool operator() (const IdString& lhs, const IdString& rhs)
		{
			return lhs.ToString() < rhs.ToString();
		}
	};
public:
	IdString(const std::string&);

	IdString(const char*);

	IdString(const IdString& src): m_u4Index(src.m_u4Index)
	{
	}

	/*
	 Create a IndStr by a known index.
		The index should have existed.
	*/
	explicit IdString(uint32 u4Index): m_u4Index(u4Index)
	{
		assert(s_pIndexes);
		assert(u4Index < s_pIndexes->size());
	}

	//Get the index.
	uint32 Index() const 
	{ 
		return m_u4Index; 
	}

	//Get the string value.
	//The address of inner string should not be changed.
	const std::string& ToString() const 
	{ 
		assert(s_pIndexes && s_pIndexes->size() > m_u4Index);
		return (*s_pIndexes)[m_u4Index];
	}

	//Cast this to a string.
	operator const std::string&() const 
	{
		return ToString();
	}

	const char* ToCString() const
	{
		return ToString().c_str();
	}

	//Compare two indexed strings.
	//@return true on succeed.
	bool operator==(const IdString& rhs) const 
	{ 
		return rhs.m_u4Index == m_u4Index; 
	}

	//Compare two index strings.
	//@return false on succeed.
	bool operator!=(const IdString& rhs) const 
	{ 
		return !operator==(rhs); 
	}

	/*
	Use index compare to fulfill "<" operator.
	\note To compare the value of two IdString, please use \see IdString::ValueLess.
	*/
	bool operator<(const IdString& rhs) const
	{
		return rhs.m_u4Index < m_u4Index;
	}

	static void StaticInit();
private:
	void Init(const std::string& Value);
	uint32 m_u4Index;
	static uint32 s_u4LastIndex;
	//TODO: optimize the implementation with a bytes buffer.
	static std::deque<std::string>* s_pIndexes;
	static std::unordered_map<std::string, uint32>* s_pIndexMap;
};

} //namespace nlang