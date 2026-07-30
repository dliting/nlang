/*-----------------------------------------------------------------------------
	ncomp/intf/Utils.h
	This file defines the utility classes and functions in the nlang compiler.
-----------------------------------------------------------------------------*/
#pragma once
#include "SyntaxNodeConsts.h"
#include <map>

namespace nlang
{

//RnField modifier information utility class.
class NLANG_COMPILER_API NodeFlagInfo
{
public:
	//Get a modifier name by type.
	static const char *NameOf(NodeBits flag);

	//Get several modifier names by a set.
	static const std::string NamesOf(NodeBits flags, 
		const char *szSplitter = ", ");

	static void StaticInit();
private:
	static std::map<NodeBits, std::string> s_FlagNames;
};

}; //namespace nlang
