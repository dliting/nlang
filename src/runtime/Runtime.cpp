/*-----------------------------------------------------------------------------
	nlang/impl/Runtime.cpp
	This file define the implementation of the global runtime system.
-----------------------------------------------------------------------------*/
#include "Runtime.h"
#include "RnMisc.h"
#include "Utils.h"
#include <mutex>
#ifdef _DEBUG
#include <crtdbg.h>
#include <stdlib.h>
#endif

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
#ifdef _DEBUG
	//Headless diagnostics policy (same philosophy as NLANG_ASSERT):
	//Debug CRT checks (_ASSERT/_CrtDbgReport, the STL's mutex and
	//iterator validations) open MODAL dialogs by default, which blocks
	//any unattended run — gate sweeps, ctest, CI. Route every CRT
	//report to stderr and drop abort's crash-report dialog so a Debug
	//assert fails the process with the message on stderr instead of
	//waiting for a human at a console. Release builds are unaffected
	//(the debug CRT is not linked there).
	_CrtSetReportMode(_CRT_ASSERT,
		_CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR,
		_CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
	_set_abort_behavior(0, _CALL_REPORTFAULT);
#endif
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