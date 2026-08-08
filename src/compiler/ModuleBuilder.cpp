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
#include <nlang/compiler/SnMisc.h>
#include "VmBackend.h"
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

	//Bail out before MergeTransUnits if any parse failed — on a top-level
	//syntax error the CompileUnit rule never reduces, so
	//TranslationUnit::Root() stays null and MergeFrom would deref null.
	if (m_upEnv->HasError())
		return false;

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
