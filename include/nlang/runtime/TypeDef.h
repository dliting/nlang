/*-----------------------------------------------------------------------------
	nlang/intf/TypeDef.h
	This file define the type constants in the N language runtime library.
-----------------------------------------------------------------------------*/

#pragma once
#include "Config.h"
#include "Flagable.h"
#include <cstdint>

namespace nlang
{

typedef std::int8_t			int8;   //i1
typedef std::uint8_t		uint8;  //u1
typedef std::int16_t       	int16;  //i2
typedef std::uint16_t		uint16; //u2
typedef std::int32_t		int32;  //i4
typedef std::uint32_t		uint32; //u4
typedef std::int64_t        int64;  //i8
typedef std::uint64_t		uint64; //u8
typedef char32_t			nchar;  //utf8-char

/*
The regular express pattern of all valid name in nlang.
Include the names defined by the system and the ones defined by user programs.
*/
#define N_ALL_NAME_PATTERN	"[A-Za-z_$]+([A-Za-z0-9_$\\[\\]])*"

//The regular express pattern of valid user defined name in nlang.
#define N_USER_NAME_PATTERN	"[A-Za-z_]+([A-Za-z0-9_])*"

//Max number of elements in an array in nlang.
const uint32 N_MAX_ARRAY_LENGTH = UINT32_MAX;

//Max length of a module name in bytes.
const uint8	MODULE_NAME_MAX	= 255;

class NLANG_RUNTIME_API CopyDisabled
{
public:
	CopyDisabled()
	{
	}
private:
	CopyDisabled(const CopyDisabled&);
	CopyDisabled& operator=(const CopyDisabled&);
};

} //namespace nlang

