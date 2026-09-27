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
#include "builder/StatementResolver.h"
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
	InitSyntaxTree();

	//Parse sources first so TranslationUnit.m_Imports is populated; the
	//list of imports to load comes from source, not from CLI.
	ParseTransUnits();
	if (m_upEnv->HasError())
		return false;

	if (!RegisterUnits())
		return false;

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

//Phase 9c cross-module: built-in types must be available BEFORE parser
//runs (parser resolves "int" / "float" / "string" to SnBuiltinDataType
//singletons constructed by BuildFromRuntime). Clear first to drop any
//stale state from a prior Build() on the same SyntaxTree singleton.
void ModuleBuilder::InitSyntaxTree()
{
	TheAST().Clear();
	TheAST().BuildFromRuntime(*m_upEnv);
}

//Register every TU's module path (owner tagging happens in
//MergeTransUnits; import gates in LoadImports — both later).
//Reset first: a second Build() on the same builder must not append
//to the previous run's entries (whose owner tags point into its
//destroyed AST).
bool ModuleBuilder::RegisterUnits()
{
	ModuleRegistry& reg = m_upEnv->Registry();
	reg.Reset();
	//The detached import stubs are owned HERE (the registry only holds
	//raw pointers), so they are dropped beside the registry reset.
	m_upDetachedImportStubs.clear();
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
	return true;
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
	{
		vmBackend->SetImportedModules(std::move(m_loadedImports));
		//Inject the library declaration index so codegen reads stdlib
		//signatures (param/return types) from the .n declarations rather
		//than from the runtime table.
		vmBackend->SetLibraryIndex(&m_upEnv->LibraryIndex());
	}
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
	//TU order == module index (ModuleRegistry registration contract).
	uint32_t moduleIndex = 0;
	for (auto pTransUnit : *m_upTransUnits)
	{
		assert(pTransUnit->Root());
		//Tag this unit's top-level members (and namespace members,
		//recursively — namespaces can span TUs) BEFORE the merge moves
		//them: after MergeFrom the unit boundary is gone.
		TagUnitMembers(*pTransUnit->Root(), moduleIndex);
		root.MergeFrom(*pTransUnit->Root(), *m_upEnv);
		//Drop the owner tags of nodes still left in the unit: a
		//namespace declared by several TUs merges member-wise, so the
		//losing NS shell stays in the unit root and dies with it. The
		//owner table is pointer-keyed — keeping the dead shell's entry
		//would let a later allocation reusing that address (e.g. a
		//GetBuiltinClassDecl SnClassDecl created during resolution)
		//silently inherit a stale owner. Only shells (and members left
		//by error paths) remain here, so the sweep is cheap.
		EraseUnitOwners(*pTransUnit->Root());
		pTransUnit->ClearRoot();
		++moduleIndex;
	}
}

//Tag the direct members of a (unit-root or nested) namespace with the
//owning module index; nested namespaces recurse.
void ModuleBuilder::TagUnitMembers(SnNamespace &ns, uint32_t moduleIndex)
{
	for (auto &member : ns.Members())
	{
		m_upEnv->Registry().TagOwner(member, moduleIndex);
		if (member.Kind() == NK_Namespace)
			TagUnitMembers(static_cast<SnNamespace &>(member),
				moduleIndex);
	}
}

//Mirror image of TagUnitMembers: erase the owner tags of everything
//still hanging under a (unit-root or nested) namespace — the nodes
//that did not survive the merge and are about to die with the unit
//root (see MergeTransUnits).
void ModuleBuilder::EraseUnitOwners(SnNamespace &ns)
{
	for (auto &member : ns.Members())
	{
		m_upEnv->Registry().EraseOwner(member);
		if (member.Kind() == NK_Namespace)
			EraseUnitOwners(static_cast<SnNamespace &>(member));
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

} //namespace nlang
