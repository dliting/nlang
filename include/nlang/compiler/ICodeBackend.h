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
class SnNamespace;
class BuildEnvironment;

//The interface of a code generation backend.
struct NLANG_COMPILER_API ICodeBackend
{
	virtual ~ICodeBackend() = default;

	//Called when a new module is created.
	virtual void OnModuleCreate(Module& module) = 0;

	//Generate the type fields of the AST.
	virtual void GenerateTypes(SnNamespace& root) = 0;

	//Generate the data fields of the AST.
	virtual void GenerateData(SnNamespace& root) = 0;

	//Generate the statement codes of the AST.
	virtual void GenerateStatements(SnNamespace& root) = 0;

	//Save the generated module to the output.
	virtual bool SaveModule(BuildEnvironment& env) = 0;
};

} //namespace nlang
