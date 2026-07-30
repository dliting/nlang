/*-----------------------------------------------------------------------------
	nlang/intf/Runtime.h
	This file define the interface of the global runtime system.
-----------------------------------------------------------------------------*/
#pragma once
#include "TypeDef.h"
#include "IdString.h"
#include "RnMisc.h"
#include <cassert>

namespace nlang
{
	
class RnNamespace;

//The global management class of the runtime.
class NLANG_RUNTIME_API Runtime
{
public:

	static void StaticInit();

	static void StaticFini();

	static const IdString &GlobalNamespaceName();

	//Get the global namespace.
	static RnNamespace &GlobalNamespace()
	{
		assert(s_pGlobalNamespace);
		return *s_pGlobalNamespace;
	}
private:
	static RnNamespace *s_pGlobalNamespace;
};

} //namespace