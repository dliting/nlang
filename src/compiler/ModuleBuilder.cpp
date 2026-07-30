/*-----------------------------------------------------------------------------
	nlang/compiler/ModuleBuilder.cpp
	This file implements the module builder of nlang.
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "TranslationUnit.h"
#include "ScriptParser.h"
#include "SyntaxTree.h"
#include "builder/ExprResolver.h"
#include "builder/ImportedNodeBuilder.hpp"
#include "builder/DuplicateFieldChecker.hpp"
#include "builder/StatementResolver.hpp"
#include <nlang/runtime/Runtime.h>
#include <nlang/runtime/Module.h>
#include "VmBackend.h"
#include <iomanip>
#include <iostream>

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

bool ModuleBuilder::Build()
{
	if (!CreateModule())
		return false;

	if (!LoadImports())
		return false;

	if (!ParseSources())
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

	ModuleManager &mm = ModuleManager::Instance();
	mm.LoadPath(m_upEnv->Params().m_ImportDirs);
	for (auto sModuleName : m_upEnv->Params().m_ImportModules)
		if (!mm.Load(sModuleName))
			return false;

	TheAST().Clear();
	TheAST().BuildFromRuntime(*m_upEnv);

	return true;
}

bool ModuleBuilder::ParseSources()
{
	ParseTransUnits();

	MergeTransUnits();

	ResolveUsingLists();

	ResolveDataTypes();

	CheckDuplicateFields();

	ResolveDataValues();

	ResolveStatements();

	return !m_upEnv->HasError();
}

bool ModuleBuilder::GenerateCodes()
{
	ICodeBackend* backend = m_upEnv->Backend();
	if (!backend) {
		m_upEnv->Log(CLL_Fatal, "No code backend configured.");
		return false;
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
			assert(!pUsing->IsResolved());
			SnNameExpr *pPath = pUsing->Path();
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
