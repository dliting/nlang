/*-----------------------------------------------------------------------------
	nlang/impl/Runtime.cpp
	This file define the implementation of the global runtime system.
-----------------------------------------------------------------------------*/
#include "Runtime.h"
#include "RnMisc.h"
#include "Utils.h"
#include <nlang/compiler/CastInfo.h>

namespace nlang
{

RnNamespace	*Runtime::s_pGlobalNamespace	= nullptr;

void Runtime::StaticInit()
{
	IdString::StaticInit();
	static RnNamespace s_GlobalNamespace(GlobalNamespaceName());
	s_GlobalNamespace.AddFlags(NF_DontDelete | NF_Hidden);
	s_pGlobalNamespace = &s_GlobalNamespace;
	RnBuiltinDataType::StaticInit();
	TypeCastInfo::StaticInit();
}

const IdString &Runtime::GlobalNamespaceName()
{
	static IdString s(GLOBAL_NAMESPACE_NAME);
	return s;
}

void Runtime::StaticFini()
{
}

} //namespace nlang