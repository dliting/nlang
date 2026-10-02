/*---
    ModuleBuilderUnits.cpp — phase 6 per-unit build path（设计：
    docs/dev/phase6_loader_design.md §3/§9）：BuildUnitImages（逐单元
    代码生成）、BuildLinked（nlink 合并后写产物）、WritePackageArtifact
    （项目 .npkg 分发包）。构建流程的其余阶段在 ModuleBuilder.cpp。
---*/
#include "ModuleBuilder.h"
#include "TranslationUnit.h"
#include "VmBackend.h"
#include "NcuLinker.h"
#include <nlang/vm/CompiledModule.h>
#include <nlang/vm/NcuPackage.h>
#include <exception>
#include <filesystem>
#include <sstream>
#include <utility>

namespace nlang
{
namespace
{

//Move the entry-owning unit to the front (in place), keeping the
//relative order of the rest. The linked artifact's header and the
//.npkg entry record both derive from units[0] (BuildLinked/
//WritePackageArtifact below), so a library unit listed first in the
//.nproj must not claim them. An empty entry key (library builds)
//keeps translation-unit order.
void MoveEntryUnitFirst(std::vector<CompiledModule>& units,
	const std::string& entryKey)
{
	if (entryKey.empty())
		return;
	const std::string entryUnit = entryKey.substr(0,
		entryKey.rfind('.'));
	for (size_t i = 1; i < units.size(); ++i)
	{
		if (units[i].modulePath == entryUnit)
		{
			CompiledModule entry = std::move(units[i]);
			units.erase(units.begin() + i);
			units.insert(units.begin(), std::move(entry));
			return;
		}
	}
}

} //namespace

//Phase 6 per-unit build (ModuleBuilder.h contract): the same front end as
//Build(), then VmBackend::GenerateUnits instead of the merged single-pass
//codegen. The external .ncu imports stay un-merged on purpose: they enter
//the caller's link closure as peer images, so codegen never bakes their
//table layout into the unit images.
ModuleBuilder::UnitImages ModuleBuilder::BuildUnitImages()
{
	UnitImages none;
	if (!PrepareUnits() || !ResolveAll())
		return none;
	SweepPendingFuncRefs();
	if (m_upEnv->HasError())
		return none;
	auto* vmBackend = dynamic_cast<VmBackend*>(m_upEnv->Backend());
	if (!vmBackend)
	{
		m_upEnv->Log(CLL_Fatal, "Per-unit builds need the VM backend.");
		return none;
	}
	//Same injections as GenerateCodes minus SetImportedModules: the
	//library index feeds stdlib signature types, the registry feeds
	//table keys and unit paths.
	vmBackend->SetLibraryIndex(&m_upEnv->LibraryIndex());
	vmBackend->SetModuleRegistry(&m_upEnv->Registry(), m_upEnv.get());
	//TU order == module index (ModuleRegistry registration contract).
	std::vector<uint32_t> unitIdxs;
	for (uint32_t i = 0; i < m_upTransUnits->size(); ++i)
		unitIdxs.push_back(i);
	UnitImages out;
	VmBackend::UnitBuildResult built =
		vmBackend->GenerateUnits(TreeRoot(), unitIdxs);
	if (m_upEnv->HasError())
		return none;
	out.units = std::move(built.units);
	out.entryKey = std::move(built.entryKey);
	out.external = std::move(m_loadedImports);
	MoveEntryUnitFirst(out.units, out.entryKey);
	return out;
}

//Phase 6 production build (ModuleBuilder.h contract): per-unit images
//-> NcuLinker::Link (peer merge of the images + the loaded external
//.ncu imports) -> write the linked module as the artifact. The backend
//keeps only merged-mode saving of its own m_compiledModule; the linked
//module goes through WriteModuleArtifact (m_compiledModule is a
//moved-out husk after GenerateUnits).
bool ModuleBuilder::BuildLinked()
{
	UnitImages images = BuildUnitImages();
	if (images.units.empty())
		return false;   //front-end/codegen errors are already logged
	//Project-mode builds also ship the package; single-file builds
	//stop at the .ncu (design section 3).
	const bool project = m_upEnv->Params().m_bProjectMode;
	std::vector<CompiledModule> closure = std::move(images.units);
	for (auto& ext : images.external)
		closure.push_back(std::move(ext));
	CompiledModule linked;
	try
	{
		linked = NcuLinker::Link(std::move(closure), images.entryKey);
	}
	catch (const std::exception& e)
	{
		m_upEnv->Log(CLL_Error, "%s", e.what());
		return false;
	}
	auto* vmBackend = dynamic_cast<VmBackend*>(m_upEnv->Backend());
	if (!vmBackend)
	{
		m_upEnv->Log(CLL_Fatal, "Per-unit builds need the VM backend.");
		return false;
	}
	if (!vmBackend->WriteModuleArtifact(*m_upEnv, linked,
			images.entryKey))
		return false;
	//Single-file builds stop at the .ncu (design section 3); project
	//builds also ship the package.
	return !project || WritePackageArtifact(linked, images.entryKey);
}

//Project-mode builds pack the distribution archive beside the .ncu.
//The member name and the entry record must carry the linked
//artifact's OWN identity: the loader's identity rule refuses a member
//whose header names anything else, and the record's
//<module>.<function> override must name the artifact's real entry —
//both derive from the merged header, and the entry-owning unit leads
//the images (BuildUnitImages).
bool ModuleBuilder::WritePackageArtifact(const CompiledModule& linked,
	const std::string& entryKey)
{
	std::ostringstream image;
	if (!WriteCompiledModule(image, linked, entryKey))
	{
		m_upEnv->Log(CLL_Fatal, "Failed to serialize module '%s'.",
			linked.modulePath.c_str());
		return false;
	}
	NcuPackageWriter packer;
	NcuMember member;
	member.modulePath = linked.modulePath;
	member.bytes = image.str();
	NcuEntryRecord entry;
	entry.modulePath = linked.modulePath;
	//"<module>.<function>" — module paths are non-empty (registration
	//rejects ""), so the dot always exists; the guard keeps a dotless
	//key from silently wrapping to the whole string (npos + 1 == 0).
	const size_t dot = entryKey.rfind('.');
	entry.functionName = dot == std::string::npos
		? entryKey : entryKey.substr(dot + 1);
	//Same path rule as VmBackend::WriteModuleArtifact:
	//<outputDir>/<outputModule>.<ext>; an empty output dir means the
	//current directory.
	std::filesystem::path pkgPath(m_upEnv->Params().m_sOutputModule
		+ NPKG_EXTENSION);
	if (!m_upEnv->Params().m_sOutputDir.empty())
		pkgPath = std::filesystem::path(
			m_upEnv->Params().m_sOutputDir) / pkgPath;
	if (!packer.AddMember(std::move(member)))
	{
		m_upEnv->Log(CLL_Fatal, "Failed to pack module '%s'.",
			linked.modulePath.c_str());
		return false;
	}
	std::string packError;
	if (!packer.Write(pkgPath.string(), m_upEnv->Params().m_sOutputModule,
			entryKey.empty() ? nullptr : &entry, &packError))
	{
		m_upEnv->Log(CLL_Error, "%s", packError.c_str());
		return false;
	}
	m_upEnv->Log(CLL_Info, "Output package file: %s ...",
		pkgPath.string().c_str());
	return true;
}

} //namespace nlang
