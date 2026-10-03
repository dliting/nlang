/*-----------------------------------------------------------------------------
	ModuleBuilderChecks.cpp
	Post-resolution validation (circular struct/class checks, interface
	implementation) of ModuleBuilder.
	Split from ModuleBuilder.cpp (2026-09-27 maintainability refactor,
	zero behavior change).
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "SyntaxTree.h"
#include <nlang/compiler/SnMisc.h>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nlang
{

//Collect the struct declarations of the root namespace and of its
//function-parent members (two-level scan, matching the historical
//collect lambda).
static void CollectStructs(SnNamespace &ns,
	std::vector<SnStructDecl*> &structs)
{
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
}

//Collect all class declarations directly under the root or nested in
//function-parent members. Shared by the circular-inheritance and the
//interface-implementation checks.
static void CollectClasses(SnNamespace &root,
	std::vector<SnClassDecl*> &classes)
{
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
}

//Shared cycle reporter of the two circular-reference checks below:
//renders the tail of a DFS path starting at the repeated node
//("A -> B -> A").
template <typename T>
static std::string FormatCycleText(const std::vector<T*> &path, T *pRepeat)
{
	std::string cycle;
	bool found = false;
	for (auto *p : path) {
		if (p == pRepeat) found = true;
		if (found) {
			if (!cycle.empty()) cycle += " -> ";
			cycle += p->Name();
		}
	}
	return cycle;
}

//Dependency edges between structs: for each struct, the other structs
//its fields reference (value-typed struct fields only).
static std::unordered_map<SnStructDecl*, std::vector<SnStructDecl*>>
	CollectStructDeps(const std::vector<SnStructDecl*> &structs)
{
	std::unordered_map<SnStructDecl*, std::vector<SnStructDecl*>> deps;
	for (auto *sd : structs) {
		for (auto &field : sd->Members()) {
			auto *fieldType = field.EvalDataType();
			if (fieldType && fieldType->Kind() == NK_StructDecl) {
				deps[sd].push_back(static_cast<SnStructDecl*>(fieldType));
			}
		}
	}
	return deps;
}

void ModuleBuilder::CheckStructCircularRefs()
{
	SnNamespace &root = TreeRoot();
	//Collect all SnStructDecl nodes.
	std::vector<SnStructDecl*> structs;
	CollectStructs(root, structs);

	//Dependency sets: which other structs does each struct reference?
	auto deps = CollectStructDeps(structs);

	//DFS cycle detection.
	std::unordered_set<SnStructDecl*> visited;
	std::unordered_set<SnStructDecl*> inStack;
	std::function<bool(SnStructDecl*, std::vector<SnStructDecl*>&)> dfs =
		[&](SnStructDecl *node, std::vector<SnStructDecl*> &path) -> bool {
		if (inStack.count(node)) {
			//Found a cycle. Report it.
			path.push_back(node);
			m_upEnv->Log(CLL_Error, "Circular struct reference: %s.",
				FormatCycleText(path, node).c_str());
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
	CollectClasses(root, classes);

	//DFS cycle detection on the inheritance chain.
	std::unordered_set<SnClassDecl*> visited;
	std::unordered_set<SnClassDecl*> inStack;
	std::function<bool(SnClassDecl*, std::vector<SnClassDecl*>&)> dfs =
		[&](SnClassDecl *node, std::vector<SnClassDecl*> &path) -> bool {
		if (inStack.count(node)) {
			path.push_back(node);
			m_upEnv->Log(CLL_Error, "Circular class inheritance: %s.",
				FormatCycleText(path, node).c_str());
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
	CollectClasses(root, classes);
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

} //namespace nlang
