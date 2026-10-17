/*---
ModuleBuilderMethodContracts.cpp - method-contract (override-
signature/interface) checking of ModuleBuilder: the class-side
override-signature bijection (Step 1) and the interface completeness
walk (Step 2). Split from ModuleBuilderChecks.cpp (2026-10-17
maintainability refactor, zero behavior change); the Object protocol
table lives in ModuleBuilderObjectProtocol.cpp.
---*/

#include "ModuleBuilder.h"
#include "ModuleBuilderChecks.h"
#include "SyntaxTree.h"
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nlang
{

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

//Contract set of one own-method name: the ancestor same-name virtual
//methods (cycle-guarded like the other chain walks) plus the same-name
//methods of every interface along the chain. Ancestor members iterate
//through the non-const Members() overload (cls is non-const).
void CollectContractSet(SnClassDecl &cls,
	const std::vector<const SnInterfaceDecl*> &ifaces,
	const std::string &name,
	std::vector<const SnFunction*> &contract,
	std::vector<std::string> &owners)
{
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
}

//The contract set is a set of SIGNATURES (spec §2.1), not a set of
//declaration entries: a multi-level override chain puts the same
//signature in the contract at every level (implicit virtual
//propagation NF_Virtual-marks each override), and several interfaces
//may require the same signature — entries must not multiply the
//bijection. Dedup by signature, keeping the first declaration as the
//diagnostic anchor (e2e sentinel: virtual_deep_dispatch.n, a 4-level
//same-signature chain, must stay green).
void DedupContractSignatures(std::vector<const SnFunction*> &contract,
	std::vector<std::string> &owners)
{
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
}

//Bijection: each own method matches a distinct contract member, each
//contract member is matched. Pick the diagnostic anchor for a
//mismatching own method: the first contract entry with the same param
//count (the signature the author most likely aimed at), else entry 0.
//Static own methods never match (see SignaturesIdentical) and land in
//the mismatch report.
bool MatchOwnMethodsToContract(const std::string &name,
	const std::vector<SnFunction*> &funcs,
	const std::vector<const SnFunction*> &contract,
	const std::vector<std::string> &owners,
	std::vector<bool> &taken,
	BuildEnvironment &env)
{
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
	return nameRejected;
}

//Uncovered contract signatures (rare: multi-signature contract with a
//partial own set) — report at the class.
bool ReportUnmatchedContractSignatures(SnClassDecl &cls,
	const std::string &name,
	const std::vector<const SnFunction*> &contract,
	const std::vector<bool> &taken,
	BuildEnvironment &env)
{
	bool nameRejected = false;
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
	return nameRejected;
}

//0.9.7 spec §2.1: within one class a name that owns a virtual method
//owns exactly one dispatch slot — runtime dispatch is name-keyed
//(FindMethodByName returns the first same-name declaration), so a
//mixed-signature family misdispatches silently. Own members must be
//signature-identical to the first declaration (the dispatch winner).
//Pairs that differ only in virtuality or in default values stay
//silent here (the field-conflict check already rejected them); pairs
//that differ in out flags or staticness additionally report here on
//top of that earlier error.
bool CheckVirtualNameUniformity(SnClassDecl &cls,
	const std::string &name,
	const std::vector<SnFunction*> &funcs,
	BuildEnvironment &env)
{
	bool anyVirtual = false;
	for (auto *pOwn : funcs)
		if (pOwn->ContainFlags(NF_Virtual))
		{
			anyVirtual = true;
			break;
		}
	if (!anyVirtual)
		return false;
	SnFunction *pWinner = funcs[0];
	bool rejected = false;
	for (size_t i = 1; i < funcs.size(); ++i)
	{
		if (SignaturesIdentical(*pWinner, *funcs[i]))
			continue;
		env.Log(CLL_Error, funcs[i]->Location(),
			"The method \"%s\" claims the virtual dispatch slot of "
			"\"%s.%s\" with a different signature \"%s\". Virtual "
			"dispatch is name-based — rename the overloads, or make "
			"the family non-virtual.",
			name.c_str(), cls.Name().c_str(),
			FormatSignature(name, *pWinner).c_str(),
			FormatSignature(name, *funcs[i]).c_str());
		env.Log(CLL_More, pWinner->Location(),
			"See also the declaration of \"%s.%s\".",
			cls.Name().c_str(),
			FormatSignature(name, *pWinner).c_str());
		rejected = true;
	}
	return rejected;
}

} //namespace

//Render a method signature for diagnostics; shared with the Object-
//protocol TU (declaration in ModuleBuilderChecks.h).
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

//Per-name body of CheckOverrideSignatures: same-class uniformity, the
//Object protocol, then the ancestor/interface bijection. Every rejection
//records the name so subclasses skip it (topmost-first error recovery).
static void CheckMethodNameContracts(SnClassDecl &cls,
	const std::vector<const SnInterfaceDecl*> &ifaces,
	const std::string &name,
	const std::vector<SnFunction*> &funcs,
	BuildEnvironment &env,
	std::set<std::string> &rejectedNames)
{
	if (rejectedNames.count(name))
		return;

	//0.9.7: same-class uniformity fires before the ancestor/
	//interface bijection — it is the more local violation, and
	//rejecting the name here stops the contract pass from double-
	//reporting the same shape.
	if (CheckVirtualNameUniformity(cls, name, funcs, env))
	{
		rejectedNames.insert(name);
		return;
	}

	//Contract set, deduped by signature (anchor = first declaration).
	std::vector<const SnFunction*> contract;
	std::vector<std::string> owners;
	CollectContractSet(cls, ifaces, name, contract, owners);
	DedupContractSignatures(contract, owners);

	//Object protocol (spec D4): a mismatch rejects the whole name.
	if (CheckObjectProtocol(cls, name, funcs, env))
	{
		rejectedNames.insert(name);
		return;
	}

	if (contract.empty())
		return;

	std::vector<bool> taken(contract.size(), false);
	bool nameRejected = MatchOwnMethodsToContract(name, funcs, contract, owners, taken, env);
	if (ReportUnmatchedContractSignatures(cls, name, contract, taken, env))
		nameRejected = true;
	if (nameRejected)
		rejectedNames.insert(name);
}

//Step 1 — class-side bijection (spec §2.1): for every name the class
//itself declares as a method, the set of its own same-name methods must
//be in bijection with the contract set (ancestor same-name virtual
//methods + same-name methods of every interface along the chain).
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
		CheckMethodNameContracts(cls, ifaces, kv.first, kv.second, env,
			rejectedNames);
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
