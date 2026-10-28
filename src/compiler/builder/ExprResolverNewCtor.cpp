/*---
    ExprResolverNewCtor.cpp — constructor-call selection (arity table,
    candidate set, overload match) shared by all three ctor call forms
    (new / bare or qualified invoke / direct-init).
    Split out of ExprResolverNew.cpp for the source-size guard
    (2026-10-22 merge of the concurrency line); zero behavior change.
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <vector>

namespace nlang
{

//Shared per-argument reject of the two ctor call forms: named arguments
//(args emit positionally without FormalBindings) and out arguments (no
//writeback path in the ctor lowering). True = rejected (diagnostic
//logged).
bool ExprResolveAccessor::RejectNonPositionalCtorArg(SnExpression &arg)
{
	if (arg.Kind() == NK_NamedArgExpr)
	{
		m_Env.Log(CLL_Error, arg.Location(),
			"named arguments are not supported in constructor calls");
		return true;
	}
	if (arg.Kind() == NK_OutArgExpr)
	{
		m_Env.Log(CLL_Error, arg.Location(),
			"out arguments are not supported in constructor calls.");
		return true;
	}
	return false;
}

//0.9.11 T6 (D10): the intrinsic ctor arity table of the builtin
//classes — their ctors are stubs registered by
//VmBackend::RegisterBuiltinClasses (arity excludes `this`) and live only
//in the CompiledClass, so a call against them is arity-checked, never
//overload-matched; requiredArity and totalArity stay equal where
//defaults cannot exist. User classes and structs answer false: their
//ctors are AST candidates for MatchCtorAgainst. Declared generic
//instances answer false too — their ctor members are AST candidates;
//synthetic func instances carry the builtin marker and arity 0.
//False = no constructor.
bool ExprResolveAccessor::FindCtorParamRange(SnField *pTypeDecl,
	size_t &requiredArity, size_t &totalArity)
{
	requiredArity = 0;
	totalArity = 0;
	if (pTypeDecl->Kind() != NK_ClassDecl)
		return false;  //struct ctors are always AST candidates
	auto *pClassDecl = static_cast<SnClassDecl*>(pTypeDecl);
	if (pClassDecl->IsBuiltinClass())
	{
		//FileStream(this, path, mode) → 2; Object/ByteStream/Thread → 0.
		//Mutex/Semaphore and the Exception family left the synthesized
		//set — declared classes overload-match their ctors like any
		//user class.
		const auto& clsName = pClassDecl->Name();
		if (clsName == "FileStream") requiredArity = totalArity = 2;
		return true;
	}
	return false;
}

//0.9.11 T6 (D10): the overload candidate set of one constructible type,
//in declaration order. Structs answer every Ctors() entry; user classes
//and declared generic instances every same-name non-static member (a
//same-name static lives in the static namespace, 0.9.10 D2.4). Builtin
//classes answer the empty set — FindCtorParamRange arity-checks calls
//against them instead.
//0.9.11 v2.6: imported class/struct stubs carry rebuilt ctor members
//(CreateClassStub/CreateStructStub), so cross-module calls match like
//source peers. A ctor with no implementation (body-less, non-native,
//non-imported — SnFunction::HasImplementation) is skipped: it has
//nothing to run, so codegen would only allocate and silently drop the
//arguments. The filter is the same predicate the backend's
//IsRegisteredCtor uses, keeping the resolver's selection in step with
//codegen — a call that would only match such a ctor now reports a clear
//no-match error instead of binding a ctor that never runs.
//0.9.12 D-new-5: candidates are gated by the caller's access authority
//over each ctor (the same AllowAccess rule fields and methods use).
//rbInaccessible records that at least one implementation-bearing ctor
//was filtered out for access alone, so the caller can tell "no
//constructor" from "no *accessible* constructor" and name the right
//diagnostic.
static std::vector<SnFunction*> CollectCtorCandidates(SnField *pTypeDecl,
	const SnField &accessor, bool &rbInaccessible)
{
	std::vector<SnFunction*> candidates;
	auto admit = [&candidates, &accessor, &rbInaccessible](SnFunction &ctor)
	{
		if (!ctor.HasImplementation())
			return;
		if (!ctor.AllowAccess(accessor))
		{
			rbInaccessible = true;
			return;
		}
		candidates.push_back(&ctor);
	};
	if (pTypeDecl->Kind() == NK_StructDecl)
	{
		for (auto &ctor : static_cast<SnStructDecl*>(pTypeDecl)->Ctors())
			admit(ctor);
		return candidates;
	}
	auto *pClassDecl = static_cast<SnClassDecl*>(pTypeDecl);
	//Phase-2 D5: declared generic instances fall through — their ctor
	//members are real AST members matched by the loop below (erasure
	//naming makes the instance's Name() the ctor member's name). Only
	//synthetic minted singletons (func) stay candidate-free — they carry
	//the builtin marker and arity-gate in FindCtorParamRange.
	if (pClassDecl->IsBuiltinClass())
		return candidates;
	for (auto &member : pClassDecl->Members())
	{
		if (member.Kind() == NK_Function
			&& member.Name() == pClassDecl->Name()
			&& !member.ContainFlags(NF_Static))
		{
			admit(static_cast<SnFunction&>(member));
		}
	}
	return candidates;
}

//0.9.11 T6 (D10): select the constructor a call binds to (all three call
//forms — new, bare/qualified invoke, direct-init — funnel here with their
//arguments already resolved). Constructor calls stage their arguments
//positionally (one FormalBinding per formal — caller args first, then
//the defaults of the omitted trailing formals), so named and out
//arguments stay rejected before matching. User classes and structs run
//the overload match (closest binding wins, a tie is ambiguous); the
//builtin/generic shapes keep the legacy arity texts. Pre-0.9.11 a
//mismatched call compiled clean: missing args read uninitialized
//callParam slots (a defaulted param arrived as garbage), extras were
//silently dropped, named args hit codegen's unhandled-kind internal
//error. Null = the call is not bound: either a diagnostic was logged,
//or the type has no constructor and no arguments were supplied (the
//legal bare-allocation shape).
SnFunction *ExprResolveAccessor::SelectCtorForArgs(SnField *pTypeDecl,
	const std::vector<SnExpression*> &actuals, const ISourceLocation *pLoc)
{
	const char *kindWord = pTypeDecl->Kind() == NK_StructDecl
		? "struct" : "class";

	//One pass over the actuals: a named/out reject is logged once and
	//skips matching (a single error instead of the old cascade behind
	//the match diagnostics).
	bool bRejected = false;
	for (auto *pActual : actuals)
	{
		if (RejectNonPositionalCtorArg(*pActual))
			bRejected = true;
	}
	if (bRejected)
		return nullptr;

	bool bAnyInaccessible = false;
	auto candidates = CollectCtorCandidates(pTypeDecl, *m_pAccessor,
		bAnyInaccessible);
	if (candidates.empty() && bAnyInaccessible)
	{
		//0.9.12 D-new-5: every candidate exists but none is reachable
		//from the caller — an accessibility failure, not a missing or
		//mismatched constructor.
		m_Env.Log(CLL_Error, pLoc,
			"constructor of %s \"%s\" is not accessible",
			kindWord, pTypeDecl->Name().c_str());
		return nullptr;
	}
	if (!candidates.empty())
	{
		bool bAmbiguous = false;
		auto *pCtor = MatchCtorAgainst(actuals, candidates, bAmbiguous);
		if (pCtor)
			return pCtor;
		if (bAmbiguous)
			m_Env.Log(CLL_Error, pLoc,
				"ambiguous constructor call to %s \"%s\": multiple "
				"constructors match the argument list",
				kindWord, pTypeDecl->Name().c_str());
		else
			m_Env.Log(CLL_Error, pLoc,
				"no constructor of %s \"%s\" matches the argument list",
				kindWord, pTypeDecl->Name().c_str());
		return nullptr;
	}

	//No AST candidates: the intrinsic-arity shapes (builtin/generic
	//classes) keep the legacy arity texts; a type with no constructor
	//at all rejects only when arguments were supplied.
	size_t requiredArity = 0;
	size_t totalArity = 0;
	const size_t argCount = actuals.size();
	if (FindCtorParamRange(pTypeDecl, requiredArity, totalArity))
	{
		if (argCount < requiredArity || argCount > totalArity)
		{
			if (requiredArity == totalArity)
			{
				m_Env.Log(CLL_Error, pLoc,
					"constructor of \"%s\" expects %zu argument(s), got %zu",
					pTypeDecl->Name().c_str(), requiredArity, argCount);
			}
			else
			{
				m_Env.Log(CLL_Error, pLoc,
					"constructor of \"%s\" expects between %zu and %zu "
					"argument(s), got %zu",
					pTypeDecl->Name().c_str(), requiredArity, totalArity,
					argCount);
			}
		}
		return nullptr;
	}
	if (argCount != 0)
	{
		m_Env.Log(CLL_Error, pLoc,
			"%s \"%s\" has no constructor; %s() cannot take arguments",
			kindWord, pTypeDecl->Name().c_str(),
			pTypeDecl->Name().c_str());
	}
	return nullptr;
}

} //namespace nlang
