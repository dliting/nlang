/*-----------------------------------------------------------------------------
	nlang/compiler/BuildEnvironment.cpp
	This file define the implementation of a compile environment for nlang.
-----------------------------------------------------------------------------*/

#include "BuildEnvironment.h"
#include "SnExtraTypes.h"
#include "builder/ModuleRegistry.h"
#include <cstdarg>

#ifdef NLANG_ENABLE_LLVM
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#endif

namespace nlang
{

BuildEnvironment::BuildEnvironment(const BuildParams& params,
	CompileLogger& logger):
	m_Params(params), m_Logger(logger), m_pCurrModule(nullptr),
	m_upRegistry(std::make_unique<ModuleRegistry>())
#ifdef NLANG_ENABLE_LLVM
	, m_pCurrMetaModule(nullptr)
#endif
{
	ClearFlags();
}

BuildEnvironment::~BuildEnvironment()
{
	// m_upBackend is now unique_ptr - auto-deleted
#ifdef NLANG_ENABLE_LLVM
	delete m_pCurrMetaModule;
#endif
}

ModuleRegistry& BuildEnvironment::Registry()
{
	return *m_upRegistry;
}

void BuildEnvironment::Log(CompileLogLevel level, const char* szFormat, ...)
{
	va_list ap;
	va_start(ap, szFormat);
	m_Logger.VLog(level, nullptr, szFormat, ap);
	va_end(ap);
}

void BuildEnvironment::Log(CompileLogLevel level, const ISourceLocation* pLoc,
	const char* szFormat, ...)
{
	va_list ap;
	va_start(ap, szFormat);
	m_Logger.VLog(level, pLoc, szFormat, ap);
	va_end(ap);
}

void BuildEnvironment::VLog(CompileLogLevel level, const ISourceLocation& loc,
	const char* szFormat, va_list& ap)
{
	m_Logger.VLog(level, &loc, szFormat, ap);
}

void BuildEnvironment::CurrModule(Module &module)
{
	m_pCurrModule = &module;
#ifdef NLANG_ENABLE_LLVM
	auto sName = m_pCurrModule->Name().ToString();
	m_pCurrMetaModule = new llvm::Module(sName, MetaContext());
#endif
}

#ifdef NLANG_ENABLE_LLVM
llvm::LLVMContext & BuildEnvironment::MetaContext()
{
	static llvm::LLVMContext s_Context;
	return s_Context;
}
#endif

} //namespace nlang
