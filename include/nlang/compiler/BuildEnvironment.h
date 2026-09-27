/*-----------------------------------------------------------------------------
	ncomp/intf/BuildEnvironment.h
	This file define the interface of a module build context for nlang.
-----------------------------------------------------------------------------*/
#pragma once
#include "TypeDef.h"
#include "Logger.h"
#include "SnTypes.h"
#include "SnExtraTypes.h"
#include "ICodeBackend.h"
#include <nlang/runtime/Module.h>
#include <list>
#include <map>
#include <memory>

//Forward declarations.
#ifdef NLANG_ENABLE_LLVM
namespace llvm
{

class Module;
class LLVMContext;

} //namespace llvm
#endif

namespace nlang
{

struct BuildParams
{
	BuildParams()
	{
		//Add working directory to import directories.
		m_ImportDirs.push_back(".");
	}

	std::list<std::string> m_ImportModules;

	std::list<std::string> m_ImportDirs;
	//The source files.
	std::list<std::string> m_SourceFiles;
	//The output module name.
	std::string m_sOutputModule;
	//The directory for output file.
	std::string m_sOutputDir;
	//The directory for temporary files.
	std::string m_sTempDir;
	//Project root directory (ncc -p mode). Module paths of source
	//files are computed relative to this (utils/helper.n →
	//"utils.helper"); empty in single-file mode (stem only).
	std::string m_sProjectDir;
	//Directory holding the standard library declaration files
	//(stdlib/io.n, ...). Located by the host tool (ncc/nide via
	//langservice::FindStdLibDir) and passed in; empty means no
	//library index is loaded.
	std::string m_sStdLibDir;
};

//Enumerate flags of module building.
enum NModuleBuildFlag: uint8
{
	MBF_ShowBuildingSteps	= 0x01,
	MBF_ShowSyntaxTree		= 0x02,
	MBF_ShowAssemblyCodes	= 0x04,
	//Enable the debug mode of the parser.
	MBF_ParserDebug			= 0x08
};

typedef uint8 ModuleBuildFlagBits;

class Module;
class TranslationUnit;
//Internal builder type (src/compiler/builder/ModuleRegistry.h); opaque
//in this public header.
class ModuleRegistry;
class SnArrayTypeToken;
//Library declaration index (nlang_langservice); opaque here so this
//public header does not depend on the langservice headers.
namespace langservice { class SymbolIndex; }

//The context during building a nlang module.
class NLANG_COMPILER_API BuildEnvironment : public Flagable<ModuleBuildFlagBits>
{
public:
	explicit BuildEnvironment(const BuildParams&, CompileLogger&);

	~BuildEnvironment();

	//Get the build parameters.
	const BuildParams& Params() const
	{
		return m_Params;
	}

	//Compile-time module registry (internal type, opaque here).
	ModuleRegistry& Registry();

	//Index of library declaration files (stdlib/*.n); the authority
	//for function signatures during type checking and codegen.
	langservice::SymbolIndex& LibraryIndex();

	//True when name is a loaded library namespace (standard or third
	//party). Thin wrapper over the library index so internal passes need
	//not depend on the langservice headers.
	bool IsLibraryNamespace(const std::string& name) const;
	//Load one library declaration file into the index (idempotent).
	void LoadLibrarySource(const std::string& path);

	//Get the current module been compiled.
	Module* CurrModule() const
	{
		return m_pCurrModule;
	}

	//Set the current module been compiled.
	void CurrModule(Module &module);

#ifdef NLANG_ENABLE_LLVM
	//Get the current LLVM module been compiled.
	llvm::Module *CurrMetaModule() const
	{
		return m_pCurrMetaModule;
	}

	//Get the LLVM context.
	static llvm::LLVMContext &MetaContext();
#endif

	//Get the code backend.
	ICodeBackend* Backend() const
	{
		return m_upBackend.get();
	}

	//Set the code backend.
	void Backend(ICodeBackend* p)
	{
		m_upBackend.reset(p);
	}

	//0.7.3 B: TU-level array token interning. Single construction
	//entry — element field → array token; the first use mints, later
	//uses share, so token pointer equality is type equality
	//(hash-consing). Lifetime = this environment (one translation
	//unit); tokens are never freed early.
	SnArrayTypeToken* InternArrayTypeToken(SnField *pElemType);

	void Log(CompileLogLevel, const char* szFormat, ...);

	void Log(CompileLogLevel, const ISourceLocation*,
		const char* szFormat, ...);

	void VLog(CompileLogLevel, const ISourceLocation&, const char* szFormat,
		va_list&);

	bool HasError() const
	{
		return m_Logger.Errors() + m_Logger.Fatals() > 0;
	}
private:
	const BuildParams& m_Params;
	CompileLogger& m_Logger;
	Module* m_pCurrModule;
#ifdef NLANG_ENABLE_LLVM
	llvm::Module *m_pCurrMetaModule;
#endif
	std::unique_ptr<ICodeBackend> m_upBackend;
	std::unique_ptr<ModuleRegistry> m_upRegistry;
	std::unique_ptr<langservice::SymbolIndex> m_upLibraryIndex;
	//0.7.3 B: interned array tokens, keyed by element field. Owns the
	//tokens for this environment's lifetime.
	std::map<SnField*, std::unique_ptr<SnArrayTypeToken>> m_InternedArrayTokens;
};

}
