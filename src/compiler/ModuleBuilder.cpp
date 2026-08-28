/*-----------------------------------------------------------------------------
	nlang/compiler/ModuleBuilder.cpp
	This file implements the module builder of nlang.
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "TranslationUnit.h"
#include "ScriptParser.h"
#include "SyntaxTree.h"
#include "builder/ExprResolver.h"
#include "builder/ModuleRegistry.h"
#include "builder/ImportedNodeBuilder.hpp"
#include "builder/CompiledModuleNodeBuilder.hpp"
#include "builder/DuplicateFieldChecker.hpp"
#include "builder/AliasExpander.hpp"
#include "builder/StatementResolver.hpp"
#include <nlang/runtime/Runtime.h>
#include <nlang/runtime/Module.h>
#include <nlang/compiler/SnMisc.h>
#include "VmBackend.h"
#include "ModuleLoader.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <functional>

static const char *BAR_STR =
	"--------------------------------------------------------------------";

namespace nlang
{

void DumpField(std::ostream &os, const SnField &sn, size_t nIndent)
{
	os << std::setw(nIndent) << ' ' <<
		(sn.MetaName().empty() ? sn.Name() : sn.MetaName()) << '\n';
	if (dynamic_cast<const SnFunctionParentField*>(&sn) != nullptr)
	{
		auto &ct = static_cast<const SnFunctionParentField&>(sn);
		for (auto &member : ct.Members())
			DumpField(os, member, nIndent + 4);
	}
}

ModuleBuilder::ModuleBuilder(const BuildParams &params,
	CompileLogger &logger):
	m_upEnv(std::make_unique<BuildEnvironment>(params, logger)),
	m_upTransUnits(std::make_unique<PtrList<TranslationUnit>>())
{
}

ModuleBuilder::~ModuleBuilder()
{
	for (auto pUnit : *m_upTransUnits)
		delete pUnit;
	// m_upTransUnits and m_upEnv are now unique_ptr - auto-deleted
	// Clear the AST to avoid static destruction order issues.
	TheAST().Clear();
}

const ModuleRegistry& ModuleBuilder::Registry() const
{
	return m_upEnv->Registry();
}

bool ModuleBuilder::Build()
{
	if (!CreateModule())
		return false;

	//Phase 9c cross-module: built-in types must be available BEFORE parser
	//runs (parser resolves "int" / "float" / "string" to SnBuiltinDataType
	//singletons constructed by BuildFromRuntime). Clear first to drop any
	//stale state from a prior Build() on the same SyntaxTree singleton.
	TheAST().Clear();
	TheAST().BuildFromRuntime(*m_upEnv);

	//Parse sources first so TranslationUnit.m_Imports is populated; the
	//list of imports to load comes from source, not from CLI.
	ParseTransUnits();
	if (m_upEnv->HasError())
		return false;

	//Register every TU's module path (owner tagging happens in
	//MergeTransUnits; import gates in LoadImports — both later).
	//Reset first: a second Build() on the same builder must not append
	//to the previous run's entries (whose owner tags point into its
	//destroyed AST).
	ModuleRegistry& reg = m_upEnv->Registry();
	reg.Reset();
	std::vector<std::string> regErrors;
	uint32_t moduleIndex = 0;
	for (auto pTransUnit : *m_upTransUnits)
	{
		if (!reg.RegisterUnit(moduleIndex++, *pTransUnit,
				m_upEnv->Params().m_sProjectDir, regErrors))
		{
			for (const auto& error : regErrors)
				m_upEnv->Log(CLL_Error, "%s", error.c_str());
			return false;
		}
	}

	//Phase 13: type alias pre-pass — must run before the units are merged
	//(alias scope is the translation unit; the merge clears unit roots).
	ExpandTypeAliases();
	if (m_upEnv->HasError())
		return false;

	//Load .nmod imports and merge stubs into root namespace.
	if (!LoadImports())
		return false;

	//Merge user TU roots (post-import) into the AST root and run all
	//subsequent resolution passes.
	MergeTransUnits();
	ResolveUsingLists();
	ResolveDataTypes();
	CheckDuplicateFields();
	ResolveDataValues();
	ResolveStatements();
	if (m_upEnv->HasError())
		return false;

	//Phase 13: sweep function references that are still pending after
	//every consumer ran — they never met an expected Func type.
	SweepPendingFuncRefs();
	if (m_upEnv->HasError())
		return false;

	if (!GenerateCodes())
		return false;

	return SaveModule();
}

bool ModuleBuilder::CreateModule()
{
	const std::string &sModuleName = m_upEnv->Params().m_sOutputModule;
	ModuleManager &mm = ModuleManager::Instance();
	Module *pModule = mm.Create(sModuleName);
	if (!pModule)
		return false;
	m_upEnv->CurrModule(*pModule);

	// Set up default VmBackend if none configured
	if (!m_upEnv->Backend()) {
		ICodeBackend* backend = new VmBackend();
		m_upEnv->Backend(backend);
		backend->OnModuleCreate(*pModule);
	}
	return true;
}

bool ModuleBuilder::LoadImports()
{
	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
	{
		m_upEnv->Log(CLL_Info, BAR_STR);
		m_upEnv->Log(CLL_Info, "Loading the import modules ...");
	}

	//Per-TU gates (D1: imports are file-scoped — one file's import must
	//not leak visibility to other files). Builtin / project names land
	//in the gate; single-segment non-project names are external .nmod
	//candidates collected below. PtrList is a std::list (no operator[]),
	//so the module index travels with a local counter.
	ModuleRegistry &reg = m_upEnv->Registry();
	std::vector<std::string> externalNames;
	std::vector<std::string> gateErrors;
	uint32_t gateModuleIndex = 0;
	for (auto pTransUnit : *m_upTransUnits)
	{
		if (!reg.BuildGate(gateModuleIndex++, pTransUnit->Imports(),
				externalNames, gateErrors))
		{
			for (const auto &error : gateErrors)
				m_upEnv->Log(CLL_Error, "%s", error.c_str());
			return false;
		}
	}

	//Load the external .nmod candidates once each (LOADING is global;
	//VISIBILITY stays per-TU via the gates built above).
	for (const auto &name : externalNames)
	{
		std::string path = FindModuleFile(name);
		if (path.empty())
		{
			const std::string notFound = ModuleNotFoundText(name);
			m_upEnv->Log(CLL_Error, "%s", notFound.c_str());
			return false;
		}

		CompiledModule cm;
		try
		{
			cm = ModuleLoader::Load(path);
		}
		catch (const std::exception &e)
		{
			m_upEnv->Log(CLL_Error, "Failed to load module '%s': %s",
				name.c_str(), e.what());
			return false;
		}

		//srcModIdx = current length of m_loadedImports (before push), which
		//matches the index this module will occupy after the push below.
		uint32_t srcModIdx = static_cast<uint32_t>(m_loadedImports.size());

		CompiledModuleNodeBuilder builder(TheAST(), srcModIdx, name);
		try
		{
			builder.BuildFromCompiledModule(cm);
		}
		catch (const std::exception &e)
		{
			m_upEnv->Log(CLL_Error, "%s", e.what());
			return false;
		}

		//Register stub→source-index entries into VmBackend side-table.
		//Defer if backend isn't ready yet — but in current flow, CreateModule
		//runs before LoadImports and sets up the backend, so it's available.
		if (auto *backend = m_upEnv->Backend())
		{
			if (auto *vmBackend = dynamic_cast<VmBackend*>(backend))
			{
				for (const auto &entry : builder.ImportedFunctions())
					vmBackend->RegisterImportedFunctionStub(entry.stub,
						srcModIdx, entry.srcFuncIdx);
			}
		}

		//Import visibility (C1): the module joins the registry as an
		//EXTERNAL entry owning every free-function stub it contributed,
		//tagged at the same point the stubs join the root — before
		//MergeTransUnits tags the TU members. Class methods are not free
		//functions (they stay inside their class stub), so this table is
		//the whole qualified-call surface for v1.
		uint32_t extIdx = reg.AddExternalModule(name);
		std::vector<SnFunction*> stubs;
		stubs.reserve(builder.ImportedFunctions().size());
		for (const auto &entry : builder.ImportedFunctions())
		{
			stubs.push_back(entry.stub);
			reg.TagOwner(*entry.stub, extIdx);
		}
		reg.SetExternalStubs(extIdx, std::move(stubs));

		//Detached stubs (names already in root) joined the tables above
		//but not the root — keep them alive for the duration of the build.
		for (auto &upStub : builder.TakeDetachedStubs())
			m_upDetachedImportStubs.push_back(std::move(upStub));

		if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
			m_upEnv->Log(CLL_Info, "Loaded module '%s' from %s",
				name.c_str(), path.c_str());

		m_loadedImports.push_back(std::move(cm));
	}

	return true;
}

std::string ModuleBuilder::FindModuleFile(const std::string &name) const
{
	for (const auto &dir : m_upEnv->Params().m_ImportDirs)
	{
		std::string path = dir + "/" + name + ".nmod";
		std::ifstream test(path, std::ios::binary);
		if (test.good())
			return path;
	}
	return std::string();
}

bool ModuleBuilder::GenerateCodes()
{
	ICodeBackend* backend = m_upEnv->Backend();
	if (!backend) {
		m_upEnv->Log(CLL_Fatal, "No code backend configured.");
		return false;
	}
	//Phase 9c cross-module: transfer imported CompiledModules to backend
	//before GenerateStatements runs. The backend owns them from here.
	if (auto *vmBackend = dynamic_cast<VmBackend*>(backend))
		vmBackend->SetImportedModules(std::move(m_loadedImports));
	backend->GenerateTypes(TreeRoot());
	backend->GenerateData(TreeRoot());
	backend->GenerateStatements(TreeRoot());
	return !m_upEnv->HasError();
}

void ModuleBuilder::ParseTransUnits()
{
	if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
	{
		m_upEnv->Log(CLL_Info, BAR_STR);
		m_upEnv->Log(CLL_Info, "Parsing the source files ...");
	}

	ScriptParser parser(*m_upEnv);
	for (auto sFilePath : m_upEnv->Params().m_SourceFiles)
	{
		if (m_upEnv->ContainFlags(MBF_ShowBuildingSteps))
			m_upEnv->Log(CLL_Info, "Parsing %s ...", sFilePath.c_str());

		TranslationUnit *pTransUnit = new TranslationUnit(sFilePath);
		m_upTransUnits->push_back(pTransUnit);
		parser.ParseUnit(*pTransUnit, m_upEnv->ContainFlags(MBF_ParserDebug));
	}
}

void ModuleBuilder::ExpandTypeAliases()
{
	//Per unit: clash check first (needs the unit root and the built-in
	//members of the tree root), then registration + use-site expansion.
	//Errors abort Build() right after this step.
	DuplicateFieldChecker checker(*m_upEnv);
	AliasExpander expander(*m_upEnv);
	for (auto pTransUnit : *m_upTransUnits)
	{
		checker.CheckUnitAliases(*pTransUnit, TreeRoot());
		expander.ProcessUnit(*pTransUnit);
	}
}

void ModuleBuilder::MergeTransUnits()
{
	SnNamespace &root = TreeRoot();
	for (auto pTransUnit : *m_upTransUnits)
	{
		assert(pTransUnit->Root());
		root.MergeFrom(*pTransUnit->Root(), *m_upEnv);
		pTransUnit->ClearRoot();
	}
}

void ModuleBuilder::ResolveUsingLists()
{
	SnNamespace &root = TreeRoot();
	ExprResolver resolver(*m_upEnv);
	for (auto pTransUnit : *m_upTransUnits)
	{
		auto pUsings = pTransUnit->Usings();
		assert(pUsings);
		for (auto pUsing : *pUsings)
		{
			//Phase 13: alias-form usings are consumed by the alias
			//pre-pass (ExpandTypeAliases); their path is the alias NAME,
			//not a namespace path, and must not be resolved here.
			if (pUsing->IsAlias())
				continue;
			assert(!pUsing->IsResolved());
			SnFieldExpr *pPath = pUsing->Path();
			assert(pPath);
			if (!resolver.Resolve(*pPath, root, root, ERF_IgnoreUsings))
			{
				pUsing->AddFlags(NF_Invalid);
				continue;
			}

			pUsing->m_pNamespace = static_cast<SnNamespace*>(pPath->Field());
			pUsing->AddFlags(NF_Resolved);
		}
	}
}

void ModuleBuilder::ResolveDataTypes()
{
	SnNamespace &root = TreeRoot();
	ExprResolver resolver(*m_upEnv);
	resolver.ResolveDataTypes(root, root);
}

void ModuleBuilder::CheckDuplicateFields()
{
	DuplicateFieldChecker checker(*m_upEnv);
	checker.Check(TreeRoot());
}

void ModuleBuilder::ResolveDataValues()
{
	//TODO: support non-simple literal values.
}

void ModuleBuilder::ResolveStatements()
{
	StatementResolver resolver(*m_upEnv);
	resolver.Resolve(TreeRoot());

	CheckStructCircularRefs();
	CheckClassCircularInheritance();
	CheckInterfaceImplementation();
}

//Phase 13: sweep function references still pending after statement
//resolution. A pending reference resolved to a function declaration but
//never met a consumer supplying an expected Func type (e.g. an argument
//position of a call that failed to bind). Left alone it would reach
//codegen as a bare identifier with no codegen binding.
void ModuleBuilder::SweepPendingFuncRefs()
{
	std::function<void(SyntaxNode&)> sweep = [&](SyntaxNode &node) {
		for (auto &child : node.Children())
		{
			//Children() yields Node&; every node below the tree root is
			//a SyntaxNode in practice.
			auto &synChild = static_cast<SyntaxNode&>(child);
			if (IsPendingFuncRef(synChild))
			{
				m_upEnv->Log(CLL_Error, synChild.Location(),
					"function reference \"%s\" requires an expected "
					"function type.",
					synChild.ToString().c_str());
			}
			sweep(synChild);
		}
	};
	sweep(TreeRoot());
}

void ModuleBuilder::CheckStructCircularRefs()
{
	SnNamespace &root = TreeRoot();
	//Collect all SnStructDecl nodes.
	std::vector<SnStructDecl*> structs;
	std::function<void(SnNamespace&)> collect = [&](SnNamespace &ns) {
		for (auto &member : ns.Members()) {
			if (member.Kind() == NK_StructDecl)
				structs.push_back(static_cast<SnStructDecl*>(&member));
			else if (CanBeFuncParent(member.Kind())) {
				for (auto &child : static_cast<SnFunctionParentField&>(member).Members()) {
					if (child.Kind() == NK_StructDecl)
						structs.push_back(static_cast<SnStructDecl*>(&child));
				}
			}
		}
	};
	collect(root);

	//Build dependency sets: for each struct, which other structs do its
	//fields reference?
	std::unordered_map<SnStructDecl*, std::vector<SnStructDecl*>> deps;
	for (auto *sd : structs) {
		for (auto &field : sd->Members()) {
			auto *fieldType = field.EvalDataType();
			if (fieldType && fieldType->Kind() == NK_StructDecl) {
				deps[sd].push_back(static_cast<SnStructDecl*>(fieldType));
			}
		}
	}

	//DFS cycle detection.
	std::unordered_set<SnStructDecl*> visited;
	std::unordered_set<SnStructDecl*> inStack;
	std::function<bool(SnStructDecl*, std::vector<SnStructDecl*>&)> dfs =
		[&](SnStructDecl *node, std::vector<SnStructDecl*> &path) -> bool {
		if (inStack.count(node)) {
			//Found a cycle. Report it.
			path.push_back(node);
			std::string cycle;
			bool found = false;
			for (auto *s : path) {
				if (s == node) found = true;
				if (found) {
					if (!cycle.empty()) cycle += " -> ";
					cycle += s->Name();
				}
			}
			m_upEnv->Log(CLL_Error, "Circular struct reference: %s.",
				cycle.c_str());
			return true;
		}
		if (visited.count(node))
			return false;
		visited.insert(node);
		inStack.insert(node);
		path.push_back(node);
		for (auto *dep : deps[node]) {
			if (dfs(dep, path))
				return true;
		}
		path.pop_back();
		inStack.erase(node);
		return false;
	};

	for (auto *sd : structs) {
		if (!visited.count(sd)) {
			std::vector<SnStructDecl*> path;
			if (dfs(sd, path))
				return;
		}
	}
}

void ModuleBuilder::CheckClassCircularInheritance()
{
	SnNamespace &root = TreeRoot();
	//Collect all SnClassDecl nodes (including nested in function parents).
	std::vector<SnClassDecl*> classes;
	for (auto &member : root.Members()) {
		if (member.Kind() == NK_ClassDecl)
			classes.push_back(static_cast<SnClassDecl*>(&member));
		else if (CanBeFuncParentEx(member.Kind())) {
			for (auto &child : static_cast<SnFunctionParentField&>(member).Members()) {
				if (child.Kind() == NK_ClassDecl)
					classes.push_back(static_cast<SnClassDecl*>(&child));
			}
		}
	}

	//DFS cycle detection on the inheritance chain.
	std::unordered_set<SnClassDecl*> visited;
	std::unordered_set<SnClassDecl*> inStack;
	std::function<bool(SnClassDecl*, std::vector<SnClassDecl*>&)> dfs =
		[&](SnClassDecl *node, std::vector<SnClassDecl*> &path) -> bool {
		if (inStack.count(node)) {
			path.push_back(node);
			std::string cycle;
			bool found = false;
			for (auto *c : path) {
				if (c == node) found = true;
				if (found) {
					if (!cycle.empty()) cycle += " -> ";
					cycle += c->Name();
				}
			}
			m_upEnv->Log(CLL_Error, "Circular class inheritance: %s.",
				cycle.c_str());
			return true;
		}
		if (visited.count(node))
			return false;
		visited.insert(node);
		inStack.insert(node);
		path.push_back(node);
		auto *pSuper = node->SuperClass();
		if (pSuper && dfs(pSuper, path))
			return true;
		path.pop_back();
		inStack.erase(node);
		return false;
	};

	for (auto *cd : classes) {
		if (!visited.count(cd)) {
			std::vector<SnClassDecl*> path;
			if (dfs(cd, path))
				return;
		}
	}
}

//Collect all methods of a class (own + inherited) by name. Used to verify
//that a class satisfies an interface's contract. Stops at the first match.
static bool ClassImplementsMethod(const SnClassDecl &cls,
	const std::string &methodName)
{
	auto *pCur = &cls;
	while (pCur)
	{
		for (auto &member : pCur->Members())
		{
			if (member.Kind() == NK_Function && member.Name() == methodName)
				return true;
		}
		pCur = pCur->SuperClass();
	}
	return false;
}

void ModuleBuilder::CheckInterfaceImplementation()
{
	SnNamespace &root = TreeRoot();
	//Collect all SnClassDecl nodes (including nested in function parents).
	std::vector<SnClassDecl*> classes;
	for (auto &member : root.Members()) {
		if (member.Kind() == NK_ClassDecl)
			classes.push_back(static_cast<SnClassDecl*>(&member));
		else if (CanBeFuncParentEx(member.Kind())) {
			for (auto &child : static_cast<SnFunctionParentField&>(member).Members()) {
				if (child.Kind() == NK_ClassDecl)
					classes.push_back(static_cast<SnClassDecl*>(&child));
			}
		}
	}
	//For each class, verify every declared interface is fully implemented.
	for (auto *pClass : classes)
	{
		for (auto *pIface : pClass->ImplementsList())
		{
			for (auto &member : pIface->Members())
			{
				if (member.Kind() != NK_Function)
					continue;
				if (!ClassImplementsMethod(*pClass, member.Name()))
				{
					m_upEnv->Log(CLL_Error, pClass->Location(),
						"The class \"%s\" does not implement the method "
						"\"%s\" required by interface \"%s\".",
						pClass->Name().c_str(),
						member.Name().c_str(),
						pIface->Name().c_str());
				}
			}
		}
	}
}

void ModuleBuilder::GenerateTypeFields()
{
	ICodeBackend* backend = m_upEnv->Backend();
	if (backend)
		backend->GenerateTypes(TreeRoot());
	std::cout << "Field names after building meta types.\n";
	DumpField(std::cout, TreeRoot(), 0);
}

void ModuleBuilder::GenerateDataFields()
{
	ICodeBackend* backend = m_upEnv->Backend();
	if (backend)
		backend->GenerateData(TreeRoot());
	std::cout << "Field names after building meta data.\n";
	DumpField(std::cout, TreeRoot(), 0);
}

void ModuleBuilder::GenerateStatements()
{
	ICodeBackend* backend = m_upEnv->Backend();
	if (backend)
		backend->GenerateStatements(TreeRoot());
}

bool ModuleBuilder::SaveModule()
{
	ICodeBackend* backend = m_upEnv->Backend();
	if (backend)
		return backend->SaveModule(*m_upEnv);
	return false;
}

} //namespace nlang
