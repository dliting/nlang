/*-----------------------------------------------------------------------------
ncomp/intf/Utils.cpp
This file implements the utility classes and functions in the nlang compiler.
-----------------------------------------------------------------------------*/

#include "Utils.h"
#include <nlang/runtime/Utils.h>
#include <sstream>
#include <cassert>

namespace nlang
{

const char *NodeFlagInfo::NameOf(NodeBits flag)
{
	switch (flag)
	{
#define MACRO_IMPL(name, offset, desc) \
	case NF_##name: return #name;
	SYNTAX_NODE_FLAG_DECL(MACRO_IMPL)
#undef MACRO_IMPL
	default:		return "unknown flag";
	}
}


const std::string NodeFlagInfo::NamesOf(NodeBits flags, 
	const char *szSplitter /*= ", "*/)
{
	std::stringstream ss;

	bool bIsFirst = true;
	for (NodeBits flag = 1; flag < NF_CP_END; flag <<= 1)
	{
		if ((flag  &flags) != 0)
		{
			if (bIsFirst)
				bIsFirst = false;
			else
				ss << szSplitter;
			ss << NameOf(flag);
		}
	}
	return ss.str();
}

} //namespace nlang