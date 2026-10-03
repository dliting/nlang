/*-----------------------------------------------------------------------------
	nlang/compiler/ICodeBackend.h
	This file defines the interface of a code generation backend for nlang.
	The default backend is the custom bytecode VM (VmBackend, Phase 2).
	An optional LLVM backend (LlvmBackend) is available when
	NLANG_ENABLE_LLVM is enabled.
-----------------------------------------------------------------------------*/
#pragma once
#include "Config.h"

namespace nlang
{

class Module;

//The interface of a code generation backend. Phase 6 narrowed it to the
//module-creation hook: code generation itself is backend-specific
//(VmBackend::BuildUnitImages drives the per-unit pipeline directly), so
//the old GenerateTypes/GenerateData/GenerateStatements/SaveModule steps
//are gone with the merged single-module path.
struct NLANG_COMPILER_API ICodeBackend
{
	virtual ~ICodeBackend() = default;

	//Called when a new module is created.
	virtual void OnModuleCreate(Module& module) = 0;
};

} //namespace nlang
