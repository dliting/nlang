/*-----------------------------------------------------------------------------
	ncomp/intf/BuildEnvironment.h
	This file define the interface of a module build context for nlang.
-----------------------------------------------------------------------------*/
#pragma once
#include "TypeDef.h"
#include "Logger.h"
#include "SnTypes.h"
#include "SnExtraTypes.h"
#include <nlang/runtime/Module.h>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>

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

//The bytecode codegen backend (src/compiler/codegen/VmBackend.h, an
//internal header; this public header holds it only behind unique_ptr).
class VmBackend;

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
	//True only for .nproj builds (ncc -p): the mode that also packs
	//the .npkg distribution archive beside the artifact. Distinct
	//from m_sProjectDir, which callers also set as the package root
	//for multi-source single builds (tests).
	bool m_bProjectMode = false;
	//Native library files packed into the .npkg as non-unit members
	//(.nproj <Natives>). Member key is the file name; no import name
	//resolves to them and the run-time loader consumes them via its
	//own extraction seam. Absolute paths, project-file order.
	std::vector<std::string> m_nativeMembers;
	//Directory holding the standard library declaration files
	//(stdlib/io.n, ...). Located by the host tool (ncc/nide via
	//langservice::FindStdLibDir) and passed in; empty means no
	//library index is loaded.
	std::string m_sStdLibDir;
	//0.7.5: suppress precision-loss warnings (--no-warn). Errors are
	//never suppressed — only the lossy-conversion warning family.
	bool m_bNoWarn = false;
	//v0.8.5: strip per-statement debug anchors (ncc --no-debug-info).
	//The artifact keeps sourceFile metadata but loses line numbers:
	//backtraces print (file:?), breakpoints and statement stepping
	//cannot bind (OP_DebugInfo is the debugger's statement checkpoint).
	bool m_bStripDebugInfo = false;
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
//The import pipeline's view of the package artifacts on the search path
//(src/compiler/ModuleBuilderImports.h); opaque here for the same reason.
struct ArtifactPackageCache;
class BuildEnvironment;
//The artifact arm's knowability probe (the import pipeline installs it
//together with the cache it reads): true when an artifact answers for
//an import name on the effective library dirs. The environment layer
//itself never opens a package - the reader lives with the pipeline - so
//link targets that use only the environment stay free of the artifact
//implementation.
using ArtifactKnowabilityProbe = bool (*)(
	const BuildEnvironment &env, ArtifactPackageCache &cache,
	const std::string &name);

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
	bool IsLibraryPackage(const std::string& name) const;
	//True when some effective library dir offers the dotted name as a .n
	//source (existence probe over the same <dir>/<a/b/c>.n mapping the
	//import discovery uses, memoized per build): a third-party package is
	//knowable to diagnostics before any import inlines it.
	bool HasLibrarySource(const std::string& name) const;
	//Load one library declaration file into the index (idempotent).
	//packageRoot: the matched search root the file was found under — the
	//index entry's package is the path relative to THAT root.
	void LoadLibrarySource(const std::string& path,
		const std::string& packageRoot);
	//The artifact discovery's view of the search path's packages (opened
	//member tables and lookups memoized). Null until the import
	//pipeline's discovery phase starts one: a consumer that runs no
	//pipeline holds nothing.
	const std::shared_ptr<ArtifactPackageCache>& ArtifactPackages() const
	{
		return m_spArtifactPackages;
	}
	//Start a fresh view; the discovery phase calls it once per build.
	void ResetArtifactPackages(
		std::shared_ptr<ArtifactPackageCache> spPackages)
	{
		m_spArtifactPackages = std::move(spPackages);
	}

	//True when an artifact answers for the import name on the effective
	//library dirs (the unit file, or a package member listing) -
	//knowability only, nothing is read or adopted. False until the
	//import pipeline installed its probe with the cache: a consumer
	//that ran no pipeline knows no search-path view.
	bool ArtifactKnowability(const std::string& name) const;
	//The probe's install pairs with ResetArtifactPackages above.
	void SetArtifactKnowabilityProbe(ArtifactKnowabilityProbe fnProbe)
	{
		m_fnArtifactKnowability = fnProbe;
	}

	//Get the current module been compiled.
	Module* CurrModule() const
	{
		return m_pCurrModule;
	}

	//Set the current module been compiled.
	void CurrModule(Module &module);

	//D8: take sole ownership of the compiled output module. The
	//ModuleManager::CreateUnregistered factory hands back a bare
	//pointer; the module dies with this environment. One output
	//module per environment -- a second call releases the first.
	void OwnModule(std::unique_ptr<Module> pModule)
	{
		m_upModule = std::move(pModule);
	}

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
	VmBackend* Backend() const
	{
		return m_upBackend.get();
	}

	//Set the code backend. Out of line: reset() deletes the previous
	//backend, so it needs VmBackend's dtor — defined in the internal TU.
	void Backend(VmBackend* p);

	//0.7.3 B: TU-level array token interning. Single construction
	//entry — element field → array token; the first use creates, later
	//uses share, so token pointer equality is type equality
	//(hash-consing). Lifetime = this environment (one translation
	//unit); tokens are never freed early.
	SnArrayTypeToken* InternArrayTypeToken(SnField *pElemType,
		uint8_t rank = 1);

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
	//D8: sole ownership of the output module. Declared before the
	//backend/registry members so it is destroyed last of them -- the
	//module outlives every compile-time consumer, as it did when the
	//global ModuleManager owned it.
	std::unique_ptr<Module> m_upModule;
#ifdef NLANG_ENABLE_LLVM
	llvm::Module *m_pCurrMetaModule;
#endif
	std::unique_ptr<VmBackend> m_upBackend;
	std::unique_ptr<ModuleRegistry> m_upRegistry;
	std::unique_ptr<langservice::SymbolIndex> m_upLibraryIndex;
	//Owned through shared_ptr so the incomplete cache type needs no
	//destructor here; only the import pipeline creates it.
	std::shared_ptr<ArtifactPackageCache> m_spArtifactPackages;
	//The probe reading m_spArtifactPackages above; installed with the
	//cache by the import pipeline (InstallArtifactCache).
	ArtifactKnowabilityProbe m_fnArtifactKnowability = nullptr;
	//Memoized existence probes of HasLibrarySource: mutable so the const
	//probe can cache (a legal deep chain asks each prefix once per build).
	mutable std::unordered_map<std::string, bool> m_LibrarySourceProbes;
	//0.7.3 B: interned array tokens, keyed by element field. Owns the
	//tokens for this environment's lifetime.
	//Key = (element type, rank): T[] and T[,] are distinct identities.
	std::map<std::pair<SnField*, uint8_t>, std::unique_ptr<SnArrayTypeToken>> m_InternedArrayTokens;
};

}
