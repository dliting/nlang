/*---
    NcuLoader.cpp — nloader 实现（设计：docs/dev/phase6_loader_design.md
    §4 步骤 1-2、§6）。闭包发现与装载全部急切完成；问题逐条收集、
    一次抛出。定位优先级：入口包自身成员表＞各搜索目录（目录内先
    `<路径>.ncu` 文件、再按文件名序扫 .npkg 成员表）。
---*/
#include "NcuLoader.h"
#include "ModuleLoader.h"
#include "nlang/vm/NcuPackage.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace nlang
{
namespace
{

namespace fs = std::filesystem;

//The one-shot report every failure path funnels into (design section 6).
[[noreturn]] void ThrowReport(const std::vector<std::string>& problems)
{
	std::string report = "nloader failed:";
	for (const auto& problem : problems)
		report += "\n  " + problem;
	throw std::runtime_error(std::move(report));
}

//One closure walk. Owns the loaded units, the entry package (its members
//outrank the search path — siblings ship together), the opened-package
//cache and the collected problem lines.
class ClosureWalk
{
public:
	explicit ClosureWalk(std::vector<std::string> searchDirs)
		: m_searchDirs(std::move(searchDirs)) {}

	//Entry artifact: .npkg (entry record member, else first unit member)
	//or .ncu (direct load). Returns the effective entry key.
	std::string LoadEntry(const std::string& artifact);

	//The .npkg half of LoadEntry: open the package, pick the entry
	//member (the entry record's, else the first unit member - format
	//1.1 native members never answer), load it, and keep the package
	//as the walk's entry package (its members outrank the search path).
	//Throws on failure. Returns the effective entry key.
	std::string LoadPackageEntry(const std::string& artifact);

	//Walks every loaded unit's import tables (units grow while walking).
	void DiscoverClosure();

	[[nodiscard]] std::vector<CompiledModule> TakeUnits()
	{ return std::move(m_units); }
	[[nodiscard]] const std::vector<std::string>& Problems() const
	{ return m_problems; }

private:
	std::vector<std::string> m_searchDirs;
	std::vector<CompiledModule> m_units;
	//Loaded OR already-reported targets — either way one resolution each.
	std::set<std::string> m_resolvedPaths;
	std::vector<std::string> m_problems;
	//Set only when the entry artifact is a package.
	std::unique_ptr<NcuPackageReader> m_entryPackage;
	//Open-package cache by file path.
	std::map<std::string, std::shared_ptr<NcuPackageReader>> m_pkgCache;
	//Archives that failed to open while probing for some target.
	std::set<std::string> m_unreadablePkgs;

	void ResolveTarget(const std::string& target);
	//The not-found problem line: searched dirs, plus any archives that
	//could not be opened while scanning them (they might have held it).
	void NoteTargetNotFound(const std::string& target);
	void LoadUnitFile(const std::string& path, const std::string& target);
	//serializedKeyOut receives the member's own serialized entry key (the
	//entry artifact's key when there is no entry record).
	void LoadPackageMember(const NcuPackageReader& pkg,
		const std::string& target, std::string* serializedKeyOut);
	//nullptr = no archive in dir holds the member.
	const NcuPackageReader* FindPackageWithMember(const std::string& dir,
		const std::string& target);
	const NcuPackageReader* OpenPackage(const fs::path& path);
	void AcceptUnit(CompiledModule unit, const std::string& target,
		const std::string& where);
};

std::string ClosureWalk::LoadEntry(const std::string& artifact)
{
	std::string entryKey;
	if (fs::path(artifact).extension() == NPKG_EXTENSION)
	{
		entryKey = LoadPackageEntry(artifact);
	}
	else
	{
		try
		{
			m_units.push_back(ModuleLoader::Load(artifact, &entryKey));
		}
		catch (const std::exception& e)
		{
			throw std::runtime_error("nloader failed:\n  '" + artifact
				+ "': " + e.what());
		}
	}
	m_resolvedPaths.insert(m_units.front().modulePath);
	return entryKey;
}

std::string ClosureWalk::LoadPackageEntry(const std::string& artifact)
{
	auto pkg = std::make_unique<NcuPackageReader>();
	std::string error;
	if (!pkg->Open(artifact, &error))
		throw std::runtime_error("nloader failed:\n  " + error);
	std::string memberPath;
	const NcuEntryRecord* rec = pkg->EntryRecord();
	if (rec)
		memberPath = rec->modulePath;
	else
	{
		//Format 1.1: the fallback is the first UNIT member - a
		//native library member can sort ahead of every unit, and
		//its file name is never a module path.
		const auto& paths = pkg->MemberPaths();
		const auto& kinds = pkg->MemberKinds();
		for (size_t i = 0; i < paths.size(); ++i)
		{
			if (kinds[i] == NcuMemberKind::Unit)
			{
				memberPath = paths[i];
				break;
			}
		}
		if (memberPath.empty())
			throw std::runtime_error("nloader failed:\n  package '"
				+ artifact + "' has no unit members");
	}
	std::string entryKey;
	LoadPackageMember(*pkg, memberPath, &entryKey);
	if (m_units.empty())
		ThrowReport(m_problems);   //entry member failed — nowhere to walk from
	//The entry record overrides the member's serialized key (the
	//.nproj names the entry unit; design section 3).
	if (rec)
		entryKey = rec->modulePath + "." + rec->functionName;
	m_entryPackage = std::move(pkg);
	return entryKey;
}

//Module paths are dot-separated identifiers (registry convention). A path
//with a separator, a drive prefix, or an empty/'.'/'..' segment cannot come
//from codegen — those bytes are corrupt, and fed to the filesystem probes
//below they would also escape the declared search roots.
bool IsValidModulePath(const std::string& path)
{
	if (path.empty() || path.find_first_of("/\\:") != std::string::npos)
		return false;
	size_t start = 0;
	for (;;)
	{
		const size_t dot = path.find('.', start);
		const std::string segment = path.substr(start,
			(dot == std::string::npos ? path.size() : dot) - start);
		if (segment.empty() || segment == "." || segment == "..")
			return false;
		if (dot == std::string::npos)
			return true;
		start = dot + 1;
	}
}

void ClosureWalk::DiscoverClosure()
{
	for (size_t i = 0; i < m_units.size(); ++i)
	{
		//Snapshot the owner name and index (never hold a reference):
		//ResolveTarget below can append to m_units, reallocating it.
		const std::string ownerPath = m_units[i].modulePath;
		std::vector<std::string> targets;
		const auto collect = [&targets](const auto& imports)
		{
			for (const auto& imp : imports)
				if (std::find(targets.begin(), targets.end(),
						imp.modulePath) == targets.end())
					targets.push_back(imp.modulePath);
		};
		collect(m_units[i].functionImports);
		collect(m_units[i].classImports);
		collect(m_units[i].structImports);
		collect(m_units[i].enumImports);
		collect(m_units[i].staticSlotImports);
		for (const auto& target : targets)
		{
			if (!IsValidModulePath(target))
			{
				m_problems.push_back("unit '" + ownerPath
					+ "' has an import slot with a malformed module path '"
					+ target + "' (corrupt image)");
				continue;
			}
			ResolveTarget(target);
		}
	}
}

//Loads (or reports) one target module. The identity rule: whatever loads
//for target X must carry modulePath X — a file or member naming anything
//else is a mislabeled artifact and is refused, not silently accepted.
void ClosureWalk::ResolveTarget(const std::string& target)
{
	if (!m_resolvedPaths.insert(target).second)
		return;   //already loaded or already reported
	//Format 1.1: a native library member answers no import target, so
	//the entry package's own table matches units only.
	if (m_entryPackage && m_entryPackage->HasUnitMember(target))
	{
		LoadPackageMember(*m_entryPackage, target, nullptr);
		return;
	}
	for (const auto& dir : m_searchDirs)
	{
		std::error_code ec;
		const fs::path file = fs::path(dir) / (target + NCU_EXTENSION);
		if (fs::is_regular_file(file, ec))
		{
			LoadUnitFile(file.string(), target);
			return;
		}
		if (const NcuPackageReader* pkg = FindPackageWithMember(dir, target))
		{
			LoadPackageMember(*pkg, target, nullptr);
			return;
		}
	}
	NoteTargetNotFound(target);
}

void ClosureWalk::NoteTargetNotFound(const std::string& target)
{
	std::string note = "module '" + target + "' not found (";
	if (m_searchDirs.empty())
		note += "no search directories";
	else
	{
		note += "searched: ";
		for (size_t i = 0; i < m_searchDirs.size(); ++i)
			note += (i ? ", " : "") + m_searchDirs[i];
	}
	note += ")";
	if (!m_unreadablePkgs.empty())
	{
		note += "; unreadable packages: ";
		bool first = true;
		for (const auto& pkg : m_unreadablePkgs)
		{
			note += (first ? "" : ", ") + pkg;
			first = false;
		}
	}
	m_problems.push_back(std::move(note));
}

void ClosureWalk::LoadUnitFile(const std::string& path,
	const std::string& target)
{
	try
	{
		AcceptUnit(ModuleLoader::Load(path), target, "'" + path + "'");
	}
	catch (const std::exception& e)
	{
		m_problems.push_back("'" + path + "': " + e.what());
	}
}

void ClosureWalk::LoadPackageMember(const NcuPackageReader& pkg,
	const std::string& target, std::string* serializedKeyOut)
{
	std::string bytes;
	std::string error;
	if (!pkg.ExtractMember(target, &bytes, &error))
	{
		m_problems.push_back(std::move(error));
		return;
	}
	const std::string where = "package '" + pkg.PackageName()
		+ "' member '" + target + "'";
	try
	{
		AcceptUnit(ModuleLoader::LoadFromBytes(where, bytes,
			serializedKeyOut), target, where);
	}
	catch (const std::exception& e)
	{
		m_problems.push_back(where + ": " + e.what());
	}
}

const NcuPackageReader* ClosureWalk::FindPackageWithMember(
	const std::string& dir, const std::string& target)
{
	std::error_code ec;
	std::vector<fs::path> archives;
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
		it.increment(ec))
	{
		if (it->is_regular_file(ec)
			&& it->path().extension() == NPKG_EXTENSION)
			archives.push_back(it->path());
	}
	//Filename order keeps member lookups deterministic across platforms.
	std::sort(archives.begin(), archives.end());
	for (const auto& path : archives)
	{
		const NcuPackageReader* pkg = OpenPackage(path);
		if (!pkg)
			continue;
		//Format 1.1: only a UNIT member answers an import target - a
		//native library's file-name key is never a module path.
		if (pkg->HasUnitMember(target))
			return pkg;
	}
	return nullptr;
}

const NcuPackageReader* ClosureWalk::OpenPackage(const fs::path& path)
{
	const std::string key = path.string();
	const auto it = m_pkgCache.find(key);
	if (it != m_pkgCache.end())
		return it->second.get();
	auto reader = std::make_unique<NcuPackageReader>();
	std::string error;
	if (reader->Open(key, &error))
	{
		const NcuPackageReader* raw = reader.get();
		m_pkgCache[key] = std::move(reader);
		return raw;
	}
	m_unreadablePkgs.insert(key + " (" + error + ")");
	return nullptr;
}

//Shared tail of both load paths: identity check, then accept into units.
void ClosureWalk::AcceptUnit(CompiledModule unit, const std::string& target,
	const std::string& where)
{
	if (unit.modulePath != target)
	{
		m_problems.push_back("unit found for module '" + target + "' ("
			+ where + ") names module '" + unit.modulePath
			+ "' in its header");
		return;
	}
	m_units.push_back(std::move(unit));
}

} //namespace

NcuLoader::Result NcuLoader::LoadClosure(const std::string& entryArtifact,
	const Options& options)
{
	ClosureWalk walk(options.searchDirs);
	Result out;
	out.entryKey = walk.LoadEntry(entryArtifact);
	walk.DiscoverClosure();
	if (!walk.Problems().empty())
		ThrowReport(walk.Problems());
	out.units = walk.TakeUnits();
	return out;
}

} //namespace nlang
