/*-----------------------------------------------------------------------------
	nlang/impl/Runtime.cpp
	This file define the implementation of the global runtime system.
-----------------------------------------------------------------------------*/
#include "Runtime.h"
#include "RnMisc.h"
#include "Utils.h"
#include <mutex>

namespace nlang
{

RnNamespace	*Runtime::s_pGlobalNamespace	= nullptr;

//0.7.5: TypeCastInfo::StaticInit() moved OUT of here to the compiler
//hosts (ncc main, in-process test hosts). The old call violated the
//module boundary — nlang_runtime referencing a nlang_compiler symbol —
//and only linked because vm-only tools (nvm/ndb) never pulled
//Runtime.obj. nvm's OP_Prim_to_str needs the Rn singletons at run
//time, so nvm now calls Runtime::StaticInit() too; that pull would
//fail to resolve the compiler symbol.
void Runtime::StaticInit()
{
	//K2 slice 0: embedding hosts may trigger the first call from several
	//threads at once; call_once keeps the whole table set single-shot.
	//The nested StaticInits are individually idempotent (IdString now
	//guards itself) but not thread-safe on their own.
	static std::once_flag s_initOnce;
	std::call_once(s_initOnce, [] {
		IdString::StaticInit();
		static RnNamespace s_GlobalNamespace(GlobalNamespaceName());
		s_GlobalNamespace.AddFlags(NF_DontDelete | NF_Hidden);
		s_pGlobalNamespace = &s_GlobalNamespace;
		RnBuiltinDataType::StaticInit();
	});
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