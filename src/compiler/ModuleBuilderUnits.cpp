/*---
    ModuleBuilderUnits.cpp — phase 6 per-unit build path（设计：
    docs/dev/phase6_loader_design.md §3/§9）：BuildUnitImages（逐单元
    代码生成，库单元排除在产物外）、BuildArtifacts（单文件写入口单元
    .ncu / 项目模式打包逐单元 .npkg）、WritePackageArtifact（项目
    .npkg 分发包）。构建流程的其余阶段在 ModuleBuilder.cpp。
---*/
#include "ModuleBuilder.h"
#include "TranslationUnit.h"
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include <nlang/vm/CompiledModule.h>
#include <nlang/vm/NcuPackage.h>
#include <filesystem>
#include <sstream>
#include <utility>

namespace nlang
{
namespace
{

//The entryKey "<module>.<function>" split, in one place: the module
//path is everything before the LAST dot, the function name the tail.
//Module paths are non-empty (registration rejects ""), so the dot
//always exists on a real key — the npos guard only keeps a corrupt
//dotless key from wrapping to the whole string (npos + 1 == 0).
struct EntryKeyParts
{
	std::string unit;
	std::string function;
};
EntryKeyParts SplitEntryKey(const std::string& entryKey)
{
	EntryKeyParts parts;
	const size_t dot = entryKey.rfind('.');
	parts.unit = dot == std::string::npos
		? entryKey : entryKey.substr(0, dot);
	parts.function = dot == std::string::npos
		? entryKey : entryKey.substr(dot + 1);
	return parts;
}

//Move the entry-owning unit to the front (in place), keeping the
//relative order of the rest. UnitImages consumers (tests and tooling)
//rely on the entry unit leading the images regardless of the .nproj
//source order. An empty entry key (library builds) keeps
//translation-unit order.
void MoveEntryUnitFirst(std::vector<CompiledModule>& units,
	const std::string& entryKey)
{
	if (entryKey.empty())
		return;
	const std::string entryUnit = SplitEntryKey(entryKey).unit;
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

//Entry record + package write (WritePackageArtifact's tail): the record
//carries the pre-split entry parts; the path rule matches
//VmBackend::WriteModuleArtifact — <outputDir>/<outputModule>.npkg, an
//empty output dir meaning the current directory.
bool WritePackageFile(BuildEnvironment& env, NcuPackageWriter& packer,
	const EntryKeyParts& entry)
{
	NcuEntryRecord record;
	record.modulePath = entry.unit;
	record.functionName = entry.function;
	std::filesystem::path pkgPath(env.Params().m_sOutputModule
		+ NPKG_EXTENSION);
	if (!env.Params().m_sOutputDir.empty())
		pkgPath = std::filesystem::path(
			env.Params().m_sOutputDir) / pkgPath;
	std::string packError;
	if (!packer.Write(pkgPath.string(), env.Params().m_sOutputModule,
			entry.function.empty() && entry.unit.empty()
				? nullptr : &record, &packError))
	{
		env.Log(CLL_Error, "%s", packError.c_str());
		return false;
	}
	env.Log(CLL_Info, "Output package file: %s ...",
		pkgPath.string().c_str());
	return true;
}

} //namespace

//Phase 6 per-unit build (ModuleBuilder.h contract): the same front end as
//Build(), then VmBackend::GenerateUnits instead of the merged single-pass
//codegen. Library translation units (stdlib or third-party sources found
//on the search path) are EXCLUDED from codegen — their sources stay
//inlined for signature resolution, but their code ships in their own
//packages and joins the closure at load time (design section 3). The
//external .ncu imports stay un-merged on purpose: they enter the
//caller's link closure as peer images, so codegen never bakes their
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
	//TU order == module index (ModuleRegistry registration contract);
	//library TUs join that index space but not the unit set.
	std::vector<uint32_t> unitIdxs;
	for (uint32_t i = 0; i < m_upTransUnits->size(); ++i)
	{
		if (!m_upEnv->Registry().IsLibraryModule(i))
			unitIdxs.push_back(i);
	}
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

//Phase 6 production build (ModuleBuilder.h contract): write the unit
//images AS the artifacts — linking is the loader's job at run time
//(design section 3). Single-file mode writes the entry unit image as
//the .ncu (its import slots ride along); project mode packs every
//unit image as a member of the .npkg distribution archive. The loaded
//external .ncu imports are deliberately NOT written: the consumer's
//import slots name them, and the run-time search path locates them.
bool ModuleBuilder::BuildArtifacts()
{
	UnitImages images = BuildUnitImages();
	if (images.units.empty())
		return false;   //front-end/codegen errors are already logged
	auto* vmBackend = dynamic_cast<VmBackend*>(m_upEnv->Backend());
	if (!vmBackend)
	{
		m_upEnv->Log(CLL_Fatal, "Per-unit builds need the VM backend.");
		return false;
	}
	if (!m_upEnv->Params().m_bProjectMode)
	{
		//One source file = one non-library TU by construction; a second
		//image would mean the input contract broke upstream, and a bare
		//false here would leave "Compilation failed." with no reason.
		if (images.units.size() != 1)
		{
			m_upEnv->Log(CLL_Fatal,
				"Single-file build produced %u unit images (expected 1).",
				static_cast<unsigned>(images.units.size()));
			return false;
		}
		return vmBackend->WriteModuleArtifact(*m_upEnv,
			images.units[0], images.entryKey);
	}
	return WritePackageArtifact(images.units, images.entryKey);
}

//Project-mode builds pack the distribution archive: one member per
//unit image, the member name being the unit's own module path (the
//loader's identity rule refuses a member whose header names anything
//else — each image IS its own header). The entry record names the
//entry-owning unit and function (the "<module>.<function>" override
//the loader applies); libraries (no entry) pack without a record.
bool ModuleBuilder::WritePackageArtifact(
	const std::vector<CompiledModule>& units,
	const std::string& entryKey) const
{
	const EntryKeyParts entry = SplitEntryKey(entryKey);
	NcuPackageWriter packer;
	for (const auto& unit : units)
	{
		std::ostringstream image;
		if (!WriteCompiledModule(image, unit,
				unit.modulePath == entry.unit ? entryKey : std::string()))
		{
			m_upEnv->Log(CLL_Fatal, "Failed to serialize module '%s'.",
				unit.modulePath.c_str());
			return false;
		}
		NcuMember member;
		member.modulePath = unit.modulePath;
		member.bytes = image.str();
		if (!packer.AddMember(std::move(member)))
		{
			m_upEnv->Log(CLL_Fatal, "Failed to pack module '%s'.",
				unit.modulePath.c_str());
			return false;
		}
	}
	return WritePackageFile(*m_upEnv, packer, entry);
}

} //namespace nlang
