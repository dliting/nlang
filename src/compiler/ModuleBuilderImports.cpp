/*-----------------------------------------------------------------------------
	ModuleBuilderImports.cpp
	The library-source arm of the import pipeline of ModuleBuilder: library
	.n discovery (including a wildcard import's package subtree), prelude
	resolution and the per-TU import gates. Split from ModuleBuilder.cpp
	(2026-09-27 maintainability refactor, zero behavior change); the
	artifact arm (unit files and package members) and the mirror-record
	arm (the fixed point's reading of artifact images' import records)
	split out to ModuleBuilderExternalImports.cpp.
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "BuiltinPrelude.h"
#include "SyntaxTree.h"
#include "TranslationUnit.h"
#include "builder/ModuleRegistry.h"
#include "ModuleBuilderImportCycles.h"
#include "ModuleBuilderImports.h"
#include "ScriptParser.h"
#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

static const char *BAR_STR =
	"--------------------------------------------------------------------";

namespace nlang
{

bool ModuleBuilder::LoadImports()
{
	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
	{
		m_upEnv->Log(CLL_Info, BAR_STR);
		m_upEnv->Log(CLL_Info, "Loading the import modules ...");
	}

	//Library sources were inlined and artifact units found (both at
	//DiscoverLibraryUnits, before registration). The artifact units join
	//the registry first, so every gate below sees the complete module
	//set; their stubs follow once the gates are built.
	MaterializeArtifacts();
	if (!BuildImportGates())
		return false;
	if (!LoadExternalModules())
		return false;

	//v0.12.6 Task 8 (D13): reject cyclic import graphs before codegen
	//(ModuleBuilderImportCycles.cpp).
	if (!RejectImportCycles())
		return false;

	return true;
}

//Per-TU gates (D1: imports are file-scoped — one file's import must
//not leak visibility to other files). Every module the gate may name is
//registered by now: compiled-in units and artifact units alike. PtrList
//is a std::list (no operator[]), so the module index travels with a
//local counter.
bool ModuleBuilder::BuildImportGates()
{
	ModuleRegistry &reg = m_upEnv->Registry();
	std::vector<std::string> gateErrors;
	uint32_t gateModuleIndex = 0;
	for (auto pTransUnit : *m_upTransUnits)
	{
		if (!reg.BuildGate(gateModuleIndex++, pTransUnit->Imports(),
				gateErrors,
				[this](const std::string &ns) {
					return m_upEnv->IsLibraryPackage(ns);
				}))
		{
			for (const auto &error : gateErrors)
				m_upEnv->Log(CLL_Error, "%s", error.c_str());
			return false;
		}
	}
	return true;
}

//Locate a library source for a dotted package name: `a.b.c` is
//<root>/a/b/c.n on the first matching search dir. Returns the file and the
//root it was found under, because the package name is the path RELATIVE TO
//THAT ROOT — a bare stem would make vendor/graphics.n register as `graphics`
//and be unreachable as `import vendor.graphics;`.
std::pair<std::string, std::string> ModuleBuilder::FindLibrarySourceFile(
	const std::string& dottedName) const
{
	const auto matches = FindLibrarySourceMatches(m_upEnv->Params(),
		dottedName);
	if (matches.empty())
		return { };
	return matches.front();
}

//Index the signatures of one library source file (so its package opens
//the same gate as the standard library) and then parse it fully as an
//inline library translation unit. Each absolute path is inlined once.
//packageRoot is the search root the file was found under — the package
//name is the path relative to THAT root (Step 4's RegisterUnits reads it
//back through m_librarySourceRoots).
bool ModuleBuilder::ParseLibraryUnit(const std::string &path,
	const std::string &packageRoot, const char *pSourceText)
{
	std::error_code fsError;
	const std::string absPath =
		std::filesystem::absolute(std::filesystem::path(path), fsError)
			.lexically_normal().string();
	if (!m_inlinedLibraryFiles.insert(absPath).second)
		return false;  //already fully parsed this build
	m_librarySourceRoots[absPath] = packageRoot;

	//Signature index (idempotent): declarations are indexed under the
	//matched-root package, so the package resolves as a library package
	//in gates. Disk sources only — the embedded prelude copy has no file
	//to index, and its gate classification rides the registry entry
	//(IsCompiledInModule) instead.
	if (pSourceText == nullptr)
		m_upEnv->LoadLibrarySource(path, packageRoot);

	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
		m_upEnv->Log(CLL_Info, "Parsing library %s ...", path.c_str());

	TranslationUnit *pUnit = new TranslationUnit(path);
	m_upTransUnits->push_back(pUnit);
	ScriptParser parser(*m_upEnv);
	parser.ParseUnit(*pUnit, m_upEnv->ContainFlags(MBF_ParserDebug),
		pSourceText);
	return true;
}

//HandleLibraryImport's match tail, shared with the prelude resolution.
bool ModuleBuilder::ImportLibraryMatch(
	const std::pair<std::string, std::string> &match)
{
	//A match that IS a project source is already compiled in; inlining it
	//again would register the same package twice.
	std::error_code fsError;
	const std::string absPath =
		std::filesystem::absolute(std::filesystem::path(
			match.first), fsError).lexically_normal().string();
	if (m_projectSourceFiles.count(absPath) > 0)
		return false;
	return ParseLibraryUnit(match.first, match.second);
}

//One prelude package's implicit-import resolution (JLS §7.5.3
//analogue). Resolution order: the first disk match across the
//effective library dirs is the canonical source (editing the stdlib
//copy takes effect on rebuild); with no disk match the embedded copy
//parses as a pseudo-path unit (module path = the package name via the
//stem fallback). Two roots offering the package is the existing
//duplicate-package diagnosis fired from here — neither disk copy may
//hijack a reserved name.
bool ModuleBuilder::EnsurePreludePackageImported(
	const PreludePackage &pkg)
{
	//A project source deriving to the reserved name IS the prelude copy
	//for this build (project-source dedup): that unit registers under
	//the same package path, so inlining any other copy would collide
	//with it. Innermost-wins is the language's shadowing rule; the
	//implicit import respects it instead of guarding the name. Success,
	//not failure — EnsurePreludeImported must still reach the remaining
	//packages.
	if (IsProjectModuleName(pkg.name))
		return true;
	const auto matches = FindLibrarySourceMatches(m_upEnv->Params(),
		pkg.name);
	if (matches.size() > 1)
	{
		m_upEnv->Log(CLL_Error, "Duplicate package '%s': '%s' and '%s'.",
			pkg.name, matches.front().first.c_str(),
			matches.back().first.c_str());
		return false;
	}
	if (!matches.empty())
		return ImportLibraryMatch(matches.front());
	return ParseLibraryUnit(pkg.name, std::string(), pkg.source);
}

//The implicit prelude import: both reserved packages, table order.
bool ModuleBuilder::EnsurePreludeImported()
{
	for (const PreludePackage &pkg : kPreludePackages)
	{
		if (!EnsurePreludePackageImported(pkg))
			return false;
	}
	return true;
}

//One import's library-source handling (DiscoverLibraryUnits' inner loop
//body): a dotted name maps to <root>/a/b/c.n under the matched root (a
//bare stem is the same rule at one segment). More than one root offering
//the same package is the duplicate-package error — the reserved-name
//table's replacement — naming both source paths instead of silently
//binding the import to whichever root searched first. A name no source
//answers for goes to the artifact arm, unless a project source derives
//to it: that unit registers under the name, and a package member of the
//same name (an earlier build's output left on the search path, say)
//must not answer for it. True when a TU was added (one fixed-point
//driver; the artifact arm adds no TU but grows the pending set, the
//other driver). discoverArtifact=false runs the source arm alone — the
//mirror-record resolver calls the artifact arm itself to read its
//outcome.
bool ModuleBuilder::HandleLibraryImport(const std::string &dottedName,
	bool discoverArtifact)
{
	//The reserved prelude packages never resolve through the disk scan
	//alone: their registration belongs to the implicit import (embedded
	//fallback included). An explicit `import lang;` — whichever TU
	//order processed it — dedups against that registration here.
	for (const PreludePackage &pkg : kPreludePackages)
	{
		if (dottedName == pkg.name)
			return EnsurePreludePackageImported(pkg);
	}
	const auto matches = FindLibrarySourceMatches(m_upEnv->Params(),
		dottedName);
	if (matches.size() > 1)
	{
		std::string message = "Duplicate package '" + dottedName
			+ "': '" + matches.front().first + "' and '"
			+ matches.back().first + "'.";
		m_upEnv->Log(CLL_Error, "%s", message.c_str());
		return false;
	}
	if (!matches.empty())
		return ImportLibraryMatch(matches.front());
	if (discoverArtifact && !IsProjectModuleName(dottedName))
		DiscoverArtifactUnit(dottedName);
	return false;
}

bool ModuleBuilder::IsProjectModuleName(const std::string& moduleName) const
{
	const std::string &projectDir = m_upEnv->Params().m_sProjectDir;
	for (const auto &sourceFile : m_projectSourceFiles)
	{
		if (DeriveModulePath(sourceFile, projectDir) == moduleName)
			return true;
	}
	return false;
}

//Module path of a file found under a wildcard's package directory:
//<pkg dir>/static/assets.n is the unit `<dottedName>.static.assets`, the
//inverse of FindLibrarySourceMatches' mapping (dots to directory
//separators). Empty when the path relative to the package dir cannot be
//computed.
static std::string LibraryUnitModulePath(const std::string &dottedName,
	const std::filesystem::path &file, const std::filesystem::path &pkgDir)
{
	std::error_code fsError;
	const std::filesystem::path rel =
		std::filesystem::relative(file, pkgDir, fsError);
	if (fsError || rel.empty())
		return { };
	std::string name = dottedName;
	for (const auto &part : rel.parent_path())
	{
		const std::string seg = part.string();
		if (!seg.empty() && seg != ".")
			name += "." + seg;
	}
	return name + "." + file.stem().string();
}

//The disk half of the wildcard union's source arm: every unit under
//<root>/<a/b>/ in each effective library dir, as module paths. The walk
//mirrors the index's own unit rules (SymbolIndex::LoadLibraryDir) - a
//dot directory holds no units, and neither does a file whose stem
//carries a dot - so discovery and the index cannot disagree about what
//a package contains. Each dir's units are sorted before joining the
//list: directory order is filesystem-dependent, and both the
//fixed-point iteration and the duplicate-package diagnosis must not
//inherit it.
static std::vector<std::string> SubtreeUnitModulePaths(
	const BuildParams &params, const std::string &dottedName)
{
	std::string rel = dottedName;
	std::replace(rel.begin(), rel.end(), '.', '/');
	const auto walkOptions =
		std::filesystem::directory_options::skip_permission_denied;
	std::vector<std::string> modulePaths;
	for (const auto &dir : EffectiveLibraryDirs(params))
	{
		const std::filesystem::path pkgDir =
			std::filesystem::path(dir) / rel;
		std::error_code fsError;
		if (!std::filesystem::is_directory(pkgDir, fsError))
			continue;

		std::vector<std::filesystem::path> units;
		for (std::filesystem::recursive_directory_iterator it(pkgDir,
				walkOptions), end; it != end; ++it)
		{
			const std::filesystem::directory_entry &entry = *it;
			if (entry.is_directory())
			{
				if (entry.path().filename().string().rfind(".", 0) == 0)
					it.disable_recursion_pending();
				continue;
			}
			if (!entry.is_regular_file()
				|| entry.path().extension() != ".n")
				continue;
			if (entry.path().stem().string().find('.')
				!= std::string::npos)
				continue;   //a dotted stem stands for a directory: not a unit
			units.push_back(entry.path());
		}
		std::sort(units.begin(), units.end());
		for (const auto &unit : units)
		{
			const std::string modulePath =
				LibraryUnitModulePath(dottedName, unit, pkgDir);
			if (!modulePath.empty())
				modulePaths.push_back(modulePath);
		}
	}
	return modulePaths;
}

//Discovery half of D-29 rule 2's wildcard union (ruling 45): `import
//a.b.*;` inlines <root>/a/b.n - the package itself - plus every unit
//under <root>/a/b/ in each effective library dir. Which candidates
//exist is decided by the files on disk, never by
//BuildEnvironment::IsLibraryPackage: that index loads only the standard
//library directory while third-party packages arrive through -I dirs,
//and a union whose size depended on the search dir would be
//unpredictable. The union's other arm is the package member tables
//(DiscoverMemberTableSubtree, the artifact arm). True when a TU was
//added (drives the iteration).
bool ModuleBuilder::DiscoverLibrarySubtree(const std::string &dottedName)
{
	bool changed = HandleLibraryImport(dottedName);   //the package file itself
	for (const std::string &modulePath :
		SubtreeUnitModulePaths(m_upEnv->Params(), dottedName))
	{
		if (HandleLibraryImport(modulePath))
			changed = true;
	}
	if (DiscoverMemberTableSubtree(dottedName))
		changed = true;
	return changed;
}

//DiscoverLibraryUnits' iteration. The TU half keeps its own scan
//cursor (TUs only ever append); the mirror half starts where the
//previous pass's artifact scan stopped. A pass that adds a TU or
//queues an artifact changes the world, so the loop runs again; a pass
//that changes nothing is the fixed point.
void ModuleBuilder::RunDiscoveryFixedPoint(
	std::set<std::string> &specNames,
	std::vector<std::string> &homelessNames)
{
	size_t scanned = 0;               //TUs already inspected
	size_t scannedArtifacts = 0;      //artifact images already inspected
	std::set<std::string> mirrorNames;  //record names already resolved
	bool changed = true;
	while (changed)
	{
		changed = false;
		//Only inspect the TUs present when this pass began: a library
		//added this pass is picked up on the next pass (the seen counter
		//bounds the scan regardless of list-end iterator stability).
		const size_t total = m_upTransUnits->size();
		size_t seen = 0;
		for (auto it = m_upTransUnits->begin();
			it != m_upTransUnits->end() && seen < total; ++it, ++seen)
		{
			if (seen < scanned)
				continue;
			const TranslationUnit *pUnit = *it;
			for (const ImportSpec &spec : pUnit->Imports())
			{
				if (!spec.wildcard)
				{
					specNames.insert(spec.DottedName());
					if (HandleLibraryImport(spec.DottedName()))
						changed = true;
					continue;
				}
				//D-29 rule 2 (ruling 45): a library wildcard opens the package
				//AND its subtree — the units inside it import only their own
				//dependencies, never their siblings. A project package's
				//wildcard finds nothing here (its sources are project files,
				//compiled in already; ImportLibraryMatch refuses to inline a
				//project source twice), so no project-versus-library case is
				//needed.
				if (DiscoverLibrarySubtree(spec.DottedName()))
					changed = true;
			}
		}
		scanned = total;
		if (ScanMirrorImportRecords(scannedArtifacts, mirrorNames,
				homelessNames))
			changed = true;
	}
}

//Discover every import-reachable library unit. Iterate the imports of
//all known TUs to a fixed point so a library can depend on a library:
//each pass may add library TUs whose imports are scanned on the next
//pass. A non-wildcard import is one candidate (a dotted name maps to
//<root>/a/b/c.n under the matched root; a bare stem is the same rule at
//one segment) and a wildcard is the package plus its subtree
//(DiscoverLibrarySubtree), whether or not the signature index already
//knows the package, so the standard library is inlined exactly like a
//third-party source library. A candidate no source answers for is
//looked up as an artifact instead (HandleLibraryImport's tail). The
//fixed point also reads every queued artifact image's import records:
//a mirror's dependency chain joins the build without any TU naming
//it, and a record name nothing answers for is reported here — no
//import gate would see it.
void ModuleBuilder::DiscoverLibraryUnits()
{
	//Per-run state: a second build starts from an empty queue and a fresh view.
	m_pendingArtifacts.clear();
	m_upEnv->ResetArtifactPackages(MakeArtifactPackageCache());

	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
	{
		m_upEnv->Log(CLL_Info, BAR_STR);
		m_upEnv->Log(CLL_Info, "Discovering library units ...");
	}

	std::set<std::string> specNames;
	std::vector<std::string> homelessNames;
	RunDiscoveryFixedPoint(specNames, homelessNames);
	ReportHomelessMirrorNames(specNames, homelessNames);
}

} //namespace nlang
