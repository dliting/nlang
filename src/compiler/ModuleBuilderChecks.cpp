/*-----------------------------------------------------------------------------
	ModuleBuilderChecks.cpp
	Post-resolution validation (circular struct/class checks, interface
	implementation, method-contract (override-signature/interface)
	checks) of ModuleBuilder.
	Split from ModuleBuilder.cpp (2026-09-27 maintainability refactor,
	zero behavior change).
-----------------------------------------------------------------------------*/

#include "ModuleBuilder.h"
#include "SyntaxTree.h"
#include "builder/ExprResolver.h"
#include <nlang/compiler/SnMisc.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
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
//function-parent members. Shared by the circular-inheritance, the
//interface-implementation and the method-contract checks.
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

//==================== method-contract checking (P1) ====================
//
//Virtual and interface dispatch are NAME-BASED: the call site lays out
//arguments per the statically bound signature, the runtime picks the
//first same-name method from the runtime class (VmExecutorOpsCalls
//FindMethodByName). Any same-name method whose signature differs from
//the one a call site bound to therefore reads arguments bit-wrong with
//no diagnostic. These checks reject every such shape at the declaration
//site.

namespace {

bool TypeExprsIdentical(const SnFieldExpr *pA, const SnFieldExpr *pB)
{
	if (pA == pB)
		return true;
	if (!pA || !pB)
		return false;
	return pA->Field() == pB->Field()
		&& pA->IsArrayType() == pB->IsArrayType();
}

//Full method-signature identity: param count, per-param type identity
//and out flag, return identity. Access levels and default values are
//deliberately NOT compared (spec D5/D6/D8).
bool SignaturesIdentical(const SnFunction &a, const SnFunction &b)
{
	//A static method can never claim an instance dispatch slot (spec
	//§2.1 static rule): both the class-side bijection and the interface
	//winner comparison route a same-signature static method to the
	//mismatch report through this check.
	if (a.ContainFlags(NF_Static) != b.ContainFlags(NF_Static))
		return false;
	if (a.Params().size() != b.Params().size())
		return false;
	if (!TypeExprsIdentical(a.ReturnType(), b.ReturnType()))
		return false;
	//Index-compare via range-for collection: the ChildFieldList iterator
	//pairing idiom elsewhere only ever walks one list at a time.
	std::vector<const SnFormalParam*> pa, pb;
	for (auto &p : a.Params()) pa.push_back(&p);
	for (auto &p : b.Params()) pb.push_back(&p);
	for (size_t i = 0; i < pa.size(); ++i)
	{
		if (!TypeExprsIdentical(pa[i]->Type(), pb[i]->Type()))
			return false;
		if (pa[i]->ContainFlags(NF_Out) != pb[i]->ContainFlags(NF_Out))
			return false;
	}
	return true;
}

std::string FormatSignature(const std::string &name, const SnFunction &f)
{
	std::string sig;
	//A static mismatch must not render identical to the instance
	//signature it is compared against.
	if (f.ContainFlags(NF_Static))
		sig += "static ";
	sig += name + "(";
	bool first = true;
	for (auto &param : f.Params())
	{
		if (!first)
			sig += ", ";
		first = false;
		if (param.ContainFlags(NF_Out))
			sig += "out ";
		sig += (param.Type() && param.Type()->Field())
			? param.Type()->Field()->ToString() : "?";
	}
	sig += "): ";
	sig += (f.ReturnType() && f.ReturnType()->Field())
		? f.ReturnType()->Field()->ToString() : "void";
	return sig;
}

//Interfaces reachable along the inheritance chain (the dispatch domain
//walks ancestors' implements lists too — ExprResolverFuncRef), deduped
//by pointer. Chain walks stop at imported stubs (empty) and builtin
//base classes (Object stub, ByteStream/FileStream — native-binding
//domain, not AST-comparable).
//Every chain walk below guards against inheritance cycles with a seen
//set: CheckClassCircularInheritance reports the cycle but does not stop
//the build, so a cyclic tree reaches these checks and a bare
//SuperClass() walk would loop forever.
void CollectChainImplements(const SnClassDecl &cls,
	std::vector<const SnInterfaceDecl*> &out)
{
	std::set<const SnClassDecl*> seen;
	for (auto *pChain = &cls; pChain && !seen.count(pChain);
		pChain = pChain->SuperClass())
	{
		seen.insert(pChain);
		if (pChain->ContainFlags(NF_Imported) || pChain->IsBuiltinClass())
			break;
		for (auto *pIface : pChain->ImplementsList())
			if (std::find(out.begin(), out.end(), pIface) == out.end())
				out.push_back(pIface);
	}
}

//First same-name method in dispatch order: the class's own members in
//declaration order, then up the superclass chain (FindMethodByName's
//order — VmExecutorOpsCalls).
SnFunction* FindDispatchWinner(SnClassDecl &cls, const std::string &name)
{
	std::set<SnClassDecl*> seen;
	for (auto *pChain = &cls; pChain && !seen.count(pChain);
		pChain = pChain->SuperClass())
	{
		seen.insert(pChain);
		if (pChain->ContainFlags(NF_Imported) || pChain->IsBuiltinClass())
			break;
		for (auto &member : pChain->Members())
			if (member.Kind() == NK_Function && member.Name() == name)
				return static_cast<SnFunction*>(&member);
	}
	return nullptr;
}

//Canonical Object-protocol signatures (spec D4): the Object builtin is
//an empty stub with no AST and no chain presence (a user class without
//a declared base has SuperClass()==nullptr, not Object), so the
//protocol is a hardcoded table. Identity compares by POINTER against
//the builtin singletons — the resolver binds "Object"/int/string type
//names to those same nodes, and name rendering would wrongly unify
//equals(SomeUserClass) with equals(Object) (both render "class ...").
struct ProtocolEntry
{
	const char *methodName;
	size_t paramCount;        //0 or 1
	NodeKind returnKind;      //NK_Int32 or NK_String
	const char *rendered;     //for diagnostics
};

const ProtocolEntry* FindProtocolEntry(const std::string &name)
{
	//rendered must agree with the row's paramCount/returnKind fields
	//(two sources of truth for the same signature).
	static const ProtocolEntry kTable[] = {
		{ "equals", 1, NK_Int32, "equals(Object): Int32" },
		{ "getHashCode", 0, NK_Int32, "getHashCode(): Int32" },
		{ "toString", 0, NK_String, "toString(): String" },
	};
	for (auto &e : kTable)
		if (name == e.methodName)
			return &e;
	return nullptr;
}

bool MatchesProtocolEntry(const SnFunction &f, const ProtocolEntry &e,
	SnField *pObjectClass, SnField *pInt, SnField *pString)
{
	//A static method cannot claim the protocol's dispatch slot either.
	if (f.ContainFlags(NF_Static))
		return false;
	if (f.Params().size() != e.paramCount)
		return false;
	SnField *pRetExpected =
		(e.returnKind == NK_String) ? pString : pInt;
	auto *pRet = f.ReturnType() ? f.ReturnType()->Field() : nullptr;
	if (pRet != pRetExpected)
		return false;
	if (e.paramCount == 1)
	{
		for (auto &param : f.Params())
		{
			auto *pType = param.Type() ? param.Type()->Field() : nullptr;
			if (pType != pObjectClass || param.IsArrayType()
				|| param.ContainFlags(NF_Out))
				return false;
		}
	}
	return true;
}

} //namespace

//Step 1 — class-side bijection (spec §2.1): for every name the class
//itself declares as a method, the set of its own same-name methods must
//be in bijection with the contract set (ancestor same-name virtual
//methods + same-name methods of every interface along the chain).
//Processing is topmost-first per chain; once a name is rejected at some
//class, all subclasses skip that name (error recovery — a mid-chain
//mismatch must not cascade onto a correct grandchild override).
static void CheckOverrideSignatures(SnClassDecl &cls,
	BuildEnvironment &env,
	std::set<std::string> &rejectedNames)
{
	std::vector<const SnInterfaceDecl*> ifaces;
	CollectChainImplements(cls, ifaces);

	//Group own methods by name (names sorted by std::map; each
	//per-name vector keeps declaration order).
	std::map<std::string, std::vector<SnFunction*>> own;
	for (auto &member : cls.Members())
		if (member.Kind() == NK_Function)
			own[member.Name()].push_back(static_cast<SnFunction*>(&member));

	for (auto &kv : own)
	{
		const std::string &name = kv.first;
		auto &funcs = kv.second;
		if (rejectedNames.count(name))
			continue;

		//Contract set. Ancestor members iterate through the non-const
		//Members() overload (cls is non-const); cycle-guarded like the
		//other chain walks.
		std::vector<const SnFunction*> contract;
		std::vector<std::string> owners;
		std::set<SnClassDecl*> seen;
		for (auto *pSuper = cls.SuperClass(); pSuper && !seen.count(pSuper);
			pSuper = pSuper->SuperClass())
		{
			seen.insert(pSuper);
			if (pSuper->ContainFlags(NF_Imported) || pSuper->IsBuiltinClass())
				break;
			for (auto &member : pSuper->Members())
				if (member.Kind() == NK_Function && member.Name() == name
					&& member.ContainFlags(NF_Virtual))
				{
					contract.push_back(static_cast<SnFunction*>(&member));
					owners.push_back(pSuper->Name());
				}
		}
		//Interface members iterate through the CONST Members() overload
		//(ifaces holds const pointers) — cast to const SnFunction.
		for (auto *pIface : ifaces)
			for (auto &member : pIface->Members())
				if (member.Kind() == NK_Function && member.Name() == name)
				{
					contract.push_back(static_cast<const SnFunction*>(&member));
					owners.push_back(pIface->Name());
				}
		//The contract set is a set of SIGNATURES (spec §2.1), not a set
		//of declaration entries: a multi-level override chain puts the
		//same signature in the contract at every level (implicit
		//virtual propagation NF_Virtual-marks each override), and
		//several interfaces may require the same signature — entries
		//must not multiply the bijection. Dedup by signature, keeping
		//the first declaration as the diagnostic anchor (e2e sentinel:
		//virtual_deep_dispatch.n, a 4-level same-signature chain, must
		//stay green).
		std::vector<const SnFunction*> deduped;
		std::vector<std::string> dedupedOwners;
		for (size_t i = 0; i < contract.size(); ++i)
		{
			bool duplicate = false;
			for (auto *pKept : deduped)
				if (SignaturesIdentical(*pKept, *contract[i]))
				{
					duplicate = true;
					break;
				}
			if (!duplicate)
			{
				deduped.push_back(contract[i]);
				dedupedOwners.push_back(owners[i]);
			}
		}
		contract.swap(deduped);
		owners.swap(dedupedOwners);

		//Object protocol (spec D4): these three names carry the implicit
		//Object contract in every user class. Check against every own
		//same-name method; a mismatch rejects the whole name (subclasses
		//skip it — same recovery as the bijection below).
		const ProtocolEntry *pProtocol = FindProtocolEntry(name);
		if (pProtocol)
		{
			//Location is only the fallback error anchor for the lookup
			//itself; the class declaration site is the natural context.
			auto *pObjectClass =
				GetBuiltinClassDecl("Object", cls.Location());
			auto *pInt = SnBuiltinDataType::InstanceOf(NK_Int32);
			auto *pString = SnBuiltinDataType::InstanceOf(NK_String);
			bool protocolRejected = false;
			for (auto *pOwn : funcs)
				if (!MatchesProtocolEntry(*pOwn, *pProtocol,
						pObjectClass, pInt, pString))
				{
					env.Log(CLL_Error, pOwn->Location(),
						"The method \"%s\" claims the virtual dispatch "
						"slot of \"Object.%s\" with a different "
						"signature \"%s\". Align the signature to "
						"override, or rename the method.",
						pOwn->Name().c_str(), pProtocol->rendered,
						FormatSignature(name, *pOwn).c_str());
					protocolRejected = true;
				}
			if (protocolRejected)
			{
				rejectedNames.insert(name);
				continue;
			}
		}

		if (contract.empty())
			continue;

		//Bijection: each own method matches a distinct contract member,
		//each contract member is matched. Pick the diagnostic anchor for
		//a mismatching own method: the first contract entry with the
		//same param count (the signature the author most likely aimed
		//at), else entry 0. Static own methods never match (see
		//SignaturesIdentical) and land in the mismatch report.
		std::vector<bool> taken(contract.size(), false);
		bool nameRejected = false;
		for (auto *pOwn : funcs)
		{
			size_t hit = contract.size();
			for (size_t i = 0; i < contract.size(); ++i)
				if (!taken[i] && SignaturesIdentical(*pOwn, *contract[i]))
				{
					hit = i;
					break;
				}
			if (hit < contract.size())
			{
				taken[hit] = true;
				continue;
			}
			//Mismatch: report against the nearest-anchor contract entry.
			size_t anchor = 0;
			for (size_t i = 0; i < contract.size(); ++i)
				if (contract[i]->Params().size() == pOwn->Params().size())
				{
					anchor = i;
					break;
				}
			env.Log(CLL_Error, pOwn->Location(),
				"The method \"%s\" claims the virtual dispatch slot of "
				"\"%s.%s\" with a different signature \"%s\". Align the "
				"signature to override, or rename the method.",
				pOwn->Name().c_str(),
				owners[anchor].c_str(),
				FormatSignature(name, *contract[anchor]).c_str(),
				FormatSignature(name, *pOwn).c_str());
			env.Log(CLL_More, contract[anchor]->Location(),
				"See also the declaration of \"%s.%s\".",
				owners[anchor].c_str(),
				FormatSignature(name, *contract[anchor]).c_str());
			nameRejected = true;
		}
		//Uncovered contract signatures (rare: multi-signature contract
		//with a partial own set) — report at the class.
		for (size_t i = 0; i < contract.size(); ++i)
			if (!taken[i])
			{
				env.Log(CLL_Error, cls.Location(),
					"The class \"%s\" does not implement the method "
					"\"%s\" required by its inheritance contract.",
					cls.Name().c_str(),
					FormatSignature(name, *contract[i]).c_str());
				nameRejected = true;
			}
		if (nameRejected)
			rejectedNames.insert(name);
	}
}

//Step 2 — interface completeness (upgrade of the former name-only
//CheckInterfaceImplementation): every method of every interface along
//the chain must be implemented by the FIRST same-name method in
//dispatch order, with a fully identical signature.
static void CheckInterfaceContracts(SnClassDecl &cls,
	BuildEnvironment &env,
	const std::set<std::string> &rejectedNames)
{
	std::vector<const SnInterfaceDecl*> ifaces;
	CollectChainImplements(cls, ifaces);
	for (auto *pIface : ifaces)
		for (auto &member : pIface->Members())
		{
			if (member.Kind() != NK_Function)
				continue;
			if (rejectedNames.count(member.Name()))
				continue;
			//const iterator chain (see the contract collection above).
			auto &required = static_cast<const SnFunction&>(member);
			auto *pWinner = FindDispatchWinner(cls, required.Name());
			if (!pWinner)
			{
				env.Log(CLL_Error, cls.Location(),
					"The class \"%s\" does not implement the method "
					"\"%s\" required by interface \"%s\".",
					cls.Name().c_str(),
					FormatSignature(required.Name(), required).c_str(),
					pIface->Name().c_str());
				continue;
			}
			if (SignaturesIdentical(*pWinner, required))
				continue;
			env.Log(CLL_Error, pWinner->Location(),
				"The class \"%s\" does not implement the method \"%s\" "
				"required by interface \"%s\" (found \"%s\" with a "
				"different signature).",
				cls.Name().c_str(),
				FormatSignature(required.Name(), required).c_str(),
				pIface->Name().c_str(),
				FormatSignature(required.Name(), *pWinner).c_str());
			env.Log(CLL_More, required.Location(),
				"See also the declaration of \"%s.%s\".",
				pIface->Name().c_str(),
				FormatSignature(required.Name(), required).c_str());
		}
}

void ModuleBuilder::CheckMethodContracts()
{
	SnNamespace &root = TreeRoot();
	std::vector<SnClassDecl*> classes;
	CollectClasses(root, classes);

	//Topmost-first chain processing with inherited rejected-name sets:
	//each class is processed once (after its superclass, if any), and a
	//name rejected higher up is skipped below it. The processed mark is
	//set BEFORE recursing into the superclass — a reported-but-ongoing
	//inheritance cycle (see the chain-walk guards) must not recurse
	//forever here either; pre-order marking leaves the DAG behavior
	//unchanged (parents still process before children).
	std::map<SnClassDecl*, std::set<std::string>> rejectedByClass;
	std::set<SnClassDecl*> processed;
	std::function<void(SnClassDecl*)> process = [&](SnClassDecl *pCls)
	{
		if (!pCls || processed.count(pCls)
			|| pCls->ContainFlags(NF_Imported) || pCls->IsBuiltinClass())
			return;
		processed.insert(pCls);
		process(pCls->SuperClass());
		std::set<std::string> rejected =
			pCls->SuperClass() ? rejectedByClass[pCls->SuperClass()]
				: std::set<std::string>();
		CheckOverrideSignatures(*pCls, *m_upEnv, rejected);
		CheckInterfaceContracts(*pCls, *m_upEnv, rejected);
		rejectedByClass[pCls] = std::move(rejected);
	};
	for (auto *pClass : classes)
		process(pClass);
}

} //namespace nlang
