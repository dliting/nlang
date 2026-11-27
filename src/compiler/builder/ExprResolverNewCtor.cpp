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
static std::vector<SnFunction*> CollectCtorCandidates(
	const ModuleRegistry &reg, SnField *pTypeDecl,
	const SnField &accessor, bool &rbInaccessible)
{
	std::vector<SnFunction*> candidates;
	auto admit = [&candidates, &reg, &accessor, &rbInaccessible](
		SnFunction &ctor)
	{
		if (!ctor.HasImplementation())
			return;
		if (!AccessAdmitted(reg, ctor, accessor))
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
	//synthetic creationed singletons (func) stay candidate-free — they carry
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
//The legacy arity texts for the no-AST-candidate shapes (0.9.11 T6):
//builtin/generic classes keep the intrinsic arity check via
//FindCtorParamRange — a call outside its arity window names the expected
//count(s) and stays unbound; a type with no constructor at all rejects
//only when arguments were supplied (the legal bare-allocation shape).
void ExprResolveAccessor::LogLegacyCtorArity(SnField *pTypeDecl,
	const std::vector<SnExpression*> &actuals, const ISourceLocation *pLoc,
	const char *kindWord)
{
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
		return;
	}
	if (argCount != 0)
	{
		m_Env.Log(CLL_Error, pLoc,
			"%s \"%s\" has no constructor; %s() cannot take arguments",
			kindWord, pTypeDecl->Name().c_str(),
			pTypeDecl->Name().c_str());
	}
}

//The overload match plus the two named mismatch outcomes. See the
//declaration in ExprResolver.h for the sharing story (ctor calls and
//the super(...) parent-ctor path).
SnFunction *ExprResolveAccessor::MatchCtorOrLogMismatch(
	SyntaxNode &argsOwner, const std::vector<SnExpression*> &actuals,
	const std::vector<SnFunction*> &candidates,
	const ISourceLocation *pLoc, const char *kindWord,
	const std::string &typeName)
{
	bool bAmbiguous = false;
	auto *pCtor = MatchCtorAgainst(argsOwner, actuals, candidates, bAmbiguous);
	if (pCtor)
		return pCtor;
	if (bAmbiguous)
	{
		m_Env.Log(CLL_Error, pLoc,
			"ambiguous constructor call to %s \"%s\": multiple "
			"constructors match the argument list",
			kindWord, typeName.c_str());
		return nullptr;
	}
	//v0.13.13a D-2(a): a rejected function reference among the actuals
	//names its own anchor instead of the generic text (the scoring sweep
	//stays silent; this is the final verdict's named note).
	if (!MaybeLogCtorFuncRefMismatch(actuals, candidates, pLoc, typeName))
		m_Env.Log(CLL_Error, pLoc,
			"no constructor of %s \"%s\" matches the argument list",
			kindWord, typeName.c_str());
	return nullptr;
}

SnFunction *ExprResolveAccessor::SelectCtorForArgs(SyntaxNode &argsOwner,
	SnField *pTypeDecl,
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
	auto candidates = CollectCtorCandidates(m_Env.Registry(), pTypeDecl,
		*m_pAccessor, bAnyInaccessible);
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
		return MatchCtorOrLogMismatch(argsOwner, actuals, candidates, pLoc,
			kindWord, pTypeDecl->Name());

	//No AST candidates: the intrinsic-arity shapes (builtin/generic
	//classes) keep the legacy arity texts; a type with no constructor
	//at all rejects only when arguments were supplied.
	LogLegacyCtorArity(pTypeDecl, actuals, pLoc, kindWord);
	return nullptr;
}

//The shared ctor-call tail of the two invoke-based call forms (bare
//`Foo(args)` and module-qualified `m.Foo(args)`). ResolveFieldExprAs
//carries the type in both Field() (member chains like Point(1,4).x
//assert on it via IsDataExpr) and EvalDataType (SnClassDecl/SnStructDecl
//is its own type). 0.9.11 D2: the kind decides which ctor slot the
//invoke carries; CtorFunction() records the selected constructor for
//codegen and the frame walkers. The parameters are already resolved in
//both paths, so the overload match reads them directly.
SnFunction *ExprResolveAccessor::BindCtorSelection(SnInvokeExpr &invoke,
	SnField &type)
{
	ResolveFieldExprAs(invoke, &type);
	if (type.Kind() == NK_StructDecl)
		invoke.CtorStruct(static_cast<SnStructDecl*>(&type));
	else
		invoke.CtorClass(static_cast<SnClassDecl*>(&type));
	std::vector<SnExpression*> actuals;
	for (auto &param : invoke.Params())
		actuals.push_back(&param);
	auto *pCtor = SelectCtorForArgs(invoke, &type, actuals,
		invoke.Location());
	invoke.CtorFunction(pCtor);
	//v0.13.13a D-2(a): the selected ctor's formals are the targets —
	//stamp the pending function references and infer the lambdas'
	//expected types (the invoke path's ResolveInvokeWithFunc twin).
	(void)BindDeferredCtorArgs(invoke, pCtor);
	return pCtor;
}

//v0.13.13a D-2(a): the constructor twin of ResolveInvokeWithFunc's
//binding tail — the invoke-based call forms (bare `Foo(args)`, module-
//qualified `m.Foo(args)`). The actuals are the invoke's parameters; the
//deferred list is (re)collected here because BindCtorSelection does not
//hold one. See BindCtorDeferredTail for the shared stamping story.
bool ExprResolveAccessor::BindDeferredCtorArgs(SnInvokeExpr &invoke,
	SnFunction *pCtor)
{
	std::vector<SnExpression*> actuals;
	for (auto &param : invoke.Params())
		actuals.push_back(&param);
	return BindCtorDeferredTail(invoke, actuals,
		CollectDeferredArgs(invoke), pCtor);
}

//v0.13.13a D-2(b): the new-expression twin (`new T(args)`, `T<A>(args)`).
//The class-name child is not an actual; the caller collected the
//deferred list for its ResolveExpressionList skip list and hands it
//over, so the failure sweep claims exactly the skipped shapes.
bool ExprResolveAccessor::BindDeferredCtorArgs(SnNewExpr &newExpr,
	SnFunction *pCtor, const std::vector<SnExpression*> &deferredArgs)
{
	std::vector<SnExpression*> actuals;
	for (auto &arg : newExpr.Args())
	{
		if (&arg == newExpr.ClassName()) continue;  //Args() view includes it
		actuals.push_back(&arg);
	}
	return BindCtorDeferredTail(newExpr, actuals, deferredArgs, pCtor);
}

//The shared binding tail behind both BindDeferredCtorArgs overloads.
//The selection (SelectCtorForArgs → MatchCtorAgainst) already scored
//the pending references and admitted the untyped shapes silently; this
//stamps them — TryBindArgs re-runs the winning candidate's positional
//bindings (mirroring MatchCtorAgainst exactly: same actuals, same
//candidate, so the shape that won the match binds here),
//BindArgFuncRefs gives a reference its bound func type, and
//ResolveDeferredArgs hands a lambda its expected function type. On
//either failure exit — a failed selection or a reference rejected by
//BindArgFuncRefs — the diagnostic is already logged; the deferred
//arguments still get their sweep so they never reach the backend
//unresolved and each answers with its own missing-target diagnostic
//(the sweep is idempotent — resolved arguments drop out).
bool ExprResolveAccessor::BindCtorDeferredTail(SyntaxNode &argsOwner,
	const std::vector<SnExpression*> &actuals,
	const std::vector<SnExpression*> &deferredArgs, SnFunction *pCtor)
{
	std::vector<FormalBinding> bindings;
	if (!pCtor || !TryBindArgs(actuals, *pCtor, bindings))
	{
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return false;
	}
	if (!BindArgFuncRefs(bindings))
	{
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return false;
	}
	return ResolveDeferredArgs(argsOwner, bindings);
}

//v0.13.13a D-2(b): the new-expression twin of BindCtorSelection, fed by
//Access(SnNewExpr&) after the type name and its type arguments resolved.
//The deferred shapes (bare brace argument, lambda) skip their own
//resolution — their target type only exists once the ctor is selected,
//the contract the invoke paths established; the selection then runs on
//the resolved actuals and BindDeferredCtorArgs stamps the pair. A
//failed argument resolution sweeps the deferred shapes for their own
//missing-target diagnostics instead of leaving them silent.
void ExprResolveAccessor::BindNewExprCtorSelection(SnNewExpr &sn,
	SnField &type)
{
	std::vector<SnExpression*> deferredArgs = CollectDeferredArgs(sn);
	if (!ResolveExpressionList(sn.Args(), &deferredArgs))
	{
		AcceptDeferredArgs(*m_pVisitor, deferredArgs);
		return;
	}
	std::vector<SnExpression*> actuals;
	for (auto &arg : sn.Args())
	{
		if (&arg == sn.ClassName()) continue;  //Args() view includes it
		actuals.push_back(&arg);
	}
	auto *pCtor = SelectCtorForArgs(sn, &type, actuals, sn.Location());
	sn.CtorFunction(pCtor);
	(void)BindDeferredCtorArgs(sn, pCtor, deferredArgs);
}

} //namespace nlang
