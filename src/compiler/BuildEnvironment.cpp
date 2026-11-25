/*-----------------------------------------------------------------------------
	nlang/compiler/BuildEnvironment.cpp
	This file define the implementation of a compile environment for nlang.
-----------------------------------------------------------------------------*/

#include "BuildEnvironment.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SnExtraTypes.h"
#include "builder/ModuleRegistry.h"
#include "ModuleBuilderImports.h"
#include <nlang/langservice/SymbolIndex.h>
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <fstream>
#include <vector>

#ifdef NLANG_ENABLE_LLVM
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <nlang/runtime/StackDump.h>
#endif

namespace nlang
{

BuildEnvironment::BuildEnvironment(const BuildParams& params,
	CompileLogger& logger):
	m_Params(params), m_Logger(logger), m_pCurrModule(nullptr),
	m_upRegistry(std::make_unique<ModuleRegistry>()),
	m_upLibraryIndex(std::make_unique<langservice::SymbolIndex>())
#ifdef NLANG_ENABLE_LLVM
	, m_pCurrMetaModule(nullptr)
#endif
{
	ClearFlags();
	if (!m_Params.m_sStdLibDir.empty())
		m_upLibraryIndex->LoadLibraryDir(m_Params.m_sStdLibDir);
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

langservice::SymbolIndex& BuildEnvironment::LibraryIndex()
{
	return *m_upLibraryIndex;
}

//Definitions of the seam declarations in ModuleBuilderImports.h. They
//live here, not in the import TUs, so referencing them pulls no builder
//machinery: a consumer linking only the environment (the langservice
//tests, IDE indexer) stays linkable without the loader libraries, and
//BuildEnvironment::HasLibrarySource shares the discovery rule by calling
//the same functions the import walk uses.

//Effective library search directories: explicit/configured import dirs
//first, then the stdlib dir, so the standard library is discoverable even
//when a direct BuildParams caller did not list it (the tool layer normally
//prepends it through BuildLibrarySearchPath).
std::vector<std::string> EffectiveLibraryDirs(const BuildParams &params)
{
	std::vector<std::string> dirs;
	dirs.assign(params.m_ImportDirs.begin(), params.m_ImportDirs.end());
	if (!params.m_sStdLibDir.empty()
		&& std::find(dirs.begin(), dirs.end(), params.m_sStdLibDir)
			== dirs.end())
		dirs.push_back(params.m_sStdLibDir);
	return dirs;
}

//Every (file, root) match across the effective library dirs: the
//duplicate-package diagnosis must see the second root too, so the scan
//does not stop at the first hit.
std::vector<std::pair<std::string, std::string>>
FindLibrarySourceMatches(const BuildParams &params,
	const std::string& dottedName)
{
	std::string rel = dottedName;
	std::replace(rel.begin(), rel.end(), '.', '/');
	std::vector<std::pair<std::string, std::string>> matches;
	for (const auto &dir : EffectiveLibraryDirs(params))
	{
		const std::string path = dir + "/" + rel + ".n";
		std::ifstream test(path, std::ios::binary);
		if (test.good())
			matches.push_back({ path, dir });
	}
	return matches;
}

bool BuildEnvironment::IsLibraryPackage(const std::string& name) const
{
	return m_upLibraryIndex && m_upLibraryIndex->HasPackage(name);
}

bool BuildEnvironment::HasLibrarySource(const std::string& name) const
{
	const auto iFound = m_LibrarySourceProbes.find(name);
	if (iFound != m_LibrarySourceProbes.end())
		return iFound->second;
	//Existence only: the resolver asks knowability per chain segment, so
	//a legal deep chain (web.form.Form.post) probes each prefix once per
	//build instead of once per use.
	const bool present =
		!FindLibrarySourceMatches(m_Params, name).empty();
	m_LibrarySourceProbes.emplace(name, present);
	return present;
}

//Knowability through the installed probe - a consumer that ran no
//import pipeline holds no pair, so nothing is known.
bool BuildEnvironment::ArtifactKnowability(
	const std::string& name) const
{
	if (!m_spArtifactPackages || !m_fnArtifactKnowability)
		return false;
	return m_fnArtifactKnowability(*this, *m_spArtifactPackages, name);
}

void BuildEnvironment::LoadLibrarySource(const std::string& path,
	const std::string& packageRoot)
{
	//The package is the path relative to the matched root — the same rule
	//LoadLibraryDir applies — not the root itself: a lazily inlined
	//third-party source must index under the package name its consumers
	//spell (vendor/graphics.n under R is "vendor.graphics").
	m_upLibraryIndex->LoadFileOnce(path,
		langservice::SymbolIndex::PackageUnderRoot(path, packageRoot));
}

SnArrayTypeToken* BuildEnvironment::InternArrayTypeToken(SnField *pElemType,
	uint8_t rank)
{
	NLANG_ASSERT(pElemType);
	auto key = std::make_pair(pElemType, rank);
	auto iFound = m_InternedArrayTokens.find(key);
	if (iFound != m_InternedArrayTokens.end())
		return iFound->second.get();
	//First use creates the token, later lookups share it — the pointer
	//stays a stable type identity. Direct new (not make_unique): the
	//token constructor is private and friendship does not extend into
	//make_unique's instantiation.
	std::unique_ptr<SnArrayTypeToken> upToken(
		new SnArrayTypeToken(pElemType, ScriptLocation(), rank));
	auto *pToken = upToken.get();
	m_InternedArrayTokens.emplace(key, std::move(upToken));
	return pToken;
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
