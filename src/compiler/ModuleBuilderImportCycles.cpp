/*-----------------------------------------------------------------------------
	ModuleBuilderImportCycles.cpp
	v0.12.6 Task 8 (D13): compile-time import-cycle rejection, split from
	ModuleBuilderImports.cpp (size guard). A cyclic import graph cannot
	initialize — lazy statics would deadlock (each unit's initializer gate
	can park on a peer whose own initializer parks back). The graph this
	build holds: every compiled unit's own imports (project, library,
	external alike) plus each loaded image's import records. Rejected
	before codegen; the nlink merge itself stays cycle-tolerant for
	images produced before the ban (test_link_cyclic_closure).
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "SyntaxTree.h"
#include "TranslationUnit.h"
#include "builder/ImportCycleCheck.h"
#include "builder/ModuleRegistry.h"
#include "nlang/vm/CompiledModule.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace nlang
{
namespace
{
//One source per key; first wins. Targets sorted + deduped so the walk
//order is deterministic — the same graph reports the same cycle across
//runs.
void AddEdges(std::map<std::string, std::vector<std::string>> &edges,
	const std::string &module, std::vector<std::string> targets)
{
	if (edges.count(module))
		return;
	std::sort(targets.begin(), targets.end());
	targets.erase(std::unique(targets.begin(), targets.end()),
		targets.end());
	edges[module] = std::move(targets);
}

//Wildcard expansion (D5 recursive prefix): `import utils.*` reaches
//utils and every nested registered module without naming them — the
//dependency edge set must contain them all, or a utils.b <-> a cycle
//hides behind the stem.
void ExpandWildcard(const std::vector<std::string> &allPaths,
	const ImportSpec &spec, std::vector<std::string> &out)
{
	const std::string stem = spec.DottedName();
	out.push_back(stem);
	if (!spec.wildcard)
		return;
	const std::string prefix = stem + ".";
	for (const auto &path : allPaths)
		if (path.compare(0, prefix.size(), prefix) == 0)
			out.push_back(path);
}

//Every import record target of a loaded image (empty paths skipped —
//an internal marker, not a module edge).
std::vector<std::string> ImageImportTargets(const CompiledModule &image)
{
	std::vector<std::string> targets;
	auto addTarget = [&](const std::string &path) {
		if (!path.empty())
			targets.push_back(path);
	};
	for (const auto &imp : image.functionImports)
		addTarget(imp.modulePath);
	for (const auto &imp : image.classImports)
		addTarget(imp.modulePath);
	for (const auto &imp : image.structImports)
		addTarget(imp.modulePath);
	for (const auto &imp : image.enumImports)
		addTarget(imp.modulePath);
	for (const auto &imp : image.staticSlotImports)
		addTarget(imp.modulePath);
	return targets;
}
} //namespace

//The dependency edge table: compiled units (project, library alike —
//each keyed by registration order, matching ModulePathOf's fill) and
//loaded .ncu images.
std::map<std::string, std::vector<std::string>>
ModuleBuilder::CollectImportGraphEdges()
{
	ModuleRegistry &reg = m_upEnv->Registry();
	std::vector<std::string> allPaths;
	for (uint32_t i = 0; i < reg.ModuleCount(); ++i)
		allPaths.push_back(reg.ModulePathOf(i));
	std::map<std::string, std::vector<std::string>> edges;
	uint32_t moduleIdx = 0;
	for (auto pTransUnit : *m_upTransUnits)
	{
		std::vector<std::string> unitEdges;
		for (const auto &spec : pTransUnit->Imports())
			ExpandWildcard(allPaths, spec, unitEdges);
		AddEdges(edges, reg.ModulePathOf(moduleIdx++),
			std::move(unitEdges));
	}
	for (const auto &image : m_loadedImports)
		AddEdges(edges, image.modulePath, ImageImportTargets(image));
	return edges;
}

bool ModuleBuilder::RejectImportCycles()
{
	const auto edges = CollectImportGraphEdges();
	ModuleRegistry &reg = m_upEnv->Registry();
	std::vector<std::string> allPaths;
	for (uint32_t i = 0; i < reg.ModuleCount(); ++i)
		allPaths.push_back(reg.ModulePathOf(i));
	uint32_t rootIdx = 0;
	for (auto pTransUnit : *m_upTransUnits)
	{
		//Roots walk in registration order (deterministic first
		//report); each root's own import list is the authoritative
		//adjacency for its node.
		const std::string &root = reg.ModulePathOf(rootIdx++);
		std::vector<std::string> rootEdges;
		for (const auto &spec : pTransUnit->Imports())
			ExpandWildcard(allPaths, spec, rootEdges);
		const std::string cycle = ImportCycleCheck::Check(
			root, rootEdges, edges);
		if (!cycle.empty())
		{
			m_upEnv->Log(CLL_Error,
				"import cycle detected: %s", cycle.c_str());
			return false;
		}
	}
	return true;
}

} //namespace nlang
