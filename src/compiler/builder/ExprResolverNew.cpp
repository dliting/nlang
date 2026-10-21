/*---
    ExprResolverNew.cpp — 分配表达式解析（new/new[]；初始化列表见
    ExprResolverInitList.cpp）
    从 ExprResolver.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include "ModuleRegistry.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Shared per-argument reject of the two ctor call forms: named arguments
//(args emit positionally without FormalBindings) and out arguments (no
//writeback path in the ctor lowering). True = rejected (diagnostic
//logged).
static bool RejectNonPositionalCtorArg(BuildEnvironment &env,
	SnExpression &arg)
{
	if (arg.Kind() == NK_NamedArgExpr)
	{
		env.Log(CLL_Error, arg.Location(),
			"named arguments are not supported in constructor calls");
		return true;
	}
	if (arg.Kind() == NK_OutArgExpr)
	{
		env.Log(CLL_Error, arg.Location(),
			"out arguments are not supported in constructor calls.");
		return true;
	}
	return false;
}

//0.9.11 T6 (D10): the intrinsic ctor arity table of the builtin and
//generic classes — their ctors are stubs registered by
//VmBackend::RegisterBuiltinClasses (arity excludes `this`) and live only
//in the CompiledClass, so a call against them is arity-checked, never
//overload-matched; requiredArity and totalArity stay equal where
//defaults cannot exist. User classes and structs answer false: their
//ctors are AST candidates for MatchCtorAgainst. False = no constructor.
bool ExprResolveAccessor::FindCtorParamRange(SnField *pTypeDecl,
	size_t &requiredArity, size_t &totalArity)
{
	requiredArity = 0;
	totalArity = 0;
	if (pTypeDecl->Kind() != NK_ClassDecl)
		return false;  //struct ctors are always AST candidates
	auto *pClassDecl = static_cast<SnClassDecl*>(pTypeDecl);
	if (IsGenericClassDecl(pClassDecl))
		return true;  //List/Dict ctor stubs take only `this`
	if (pClassDecl->IsBuiltinClass())
	{
		//FileStream(this, path, mode) → 2; Exception family
		//(this, message) → 1; Object/ByteStream → 0.
		const auto& clsName = pClassDecl->Name();
		if (clsName == "FileStream") requiredArity = totalArity = 2;
		else if (IsBuiltinExceptionClassName(clsName))
			requiredArity = totalArity = 1;
		return true;
	}
	return false;
}

//0.9.11 T6 (D10): the overload candidate set of one constructible type,
//in declaration order. Structs answer every Ctors() entry; user classes
//every same-name non-static member (a same-name static lives in the
//static namespace, 0.9.10 D2.4). Builtin and generic classes answer the
//empty set — FindCtorParamRange arity-checks calls against them instead.
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
	if (IsGenericClassDecl(pClassDecl) || pClassDecl->IsBuiltinClass())
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
		if (RejectNonPositionalCtorArg(m_Env, *pActual))
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

//Builtin class names: ByteStream, FileStream, Object.
//Phase 9d: Exception hierarchy.
//These don't exist in the AST namespace, so the name won't resolve
//through the normal path. Use the singleton SnClassDecl.
void ExprResolveAccessor::TryResolveBuiltinClassName(
	SnFieldExpr &fieldExpr)
{
	if (fieldExpr.IsResolved())
		return;
	const auto& name = fieldExpr.ToString();
	if (IsBuiltinClassName(name))
		ResolveFieldExprAs(fieldExpr,
			GetBuiltinClassDecl(name, fieldExpr.Location()));
}

//Binds the resolved type declaration to the new-expression. Classes keep
//the Phase 13 func-type rejection (func values come only from function or
//method references, never by-name construction); structs join directly
//(0.9.11 D2). False = the diagnostic was logged, abort resolution.
bool ExprResolveAccessor::BindNewExprType(SnNewExpr &sn, SnField &typeField)
{
	if (typeField.Kind() == NK_ClassDecl)
	{
		auto pClassDecl = static_cast<SnClassDecl*>(&typeField);
		if (IsGenericClassDecl(pClassDecl)
			&& pClassDecl->BaseName() == kBuiltinFuncTypeName)
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"func types cannot be constructed by name; bind a "
				"function or method reference");
			return false;
		}
		sn.ClassDecl(pClassDecl);
	}
	else
	{
		sn.StructDecl(static_cast<SnStructDecl*>(&typeField));
	}
	return true;
}

void ExprResolveAccessor::Access(SnNewExpr &sn)
{
	assert(!sn.IsResolved());

	auto pClassName = sn.ClassName();
	assert(pClassName);
	pClassName->Accept(*m_pVisitor);
	TryResolveBuiltinClassName(*pClassName);

	if (!pClassName->IsResolved())
		return;

	auto pTypeField = pClassName->Field();
	//0.9.11 D2: struct types join classes on the new-expression path (the
	//direct-init `S name(args);` spelling decomposes into one of these).
	if (!pTypeField
		|| (pTypeField->Kind() != NK_ClassDecl
			&& pTypeField->Kind() != NK_StructDecl))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"\"%s\" is not a class or struct type.",
			pClassName->ToString().c_str());
		return;
	}

	if (!BindNewExprType(sn, *pTypeField))
		return;
	sn.EvalDataType(pTypeField);
	sn.AddFlags(NF_Resolved);

	//0.9.11 T6: the arguments resolve before the ctor is selected — the
	//overload match reads their types. ResolveExpressionList skips the
	//already-resolved class-name child, so Args() stays safe here.
	if (!ResolveExpressionList(sn.Args()))
		return;

	std::vector<SnExpression*> actuals;
	for (auto &arg : sn.Args())
	{
		if (&arg == sn.ClassName()) continue;  //Args() view includes it
		actuals.push_back(&arg);
	}
	sn.CtorFunction(SelectCtorForArgs(pTypeField, actuals, sn.Location()));
}

void ExprResolveAccessor::Access(SnNewArrayExpr &sn)
{
	assert(!sn.IsResolved());

	//Resolve element type name.
	auto pElemType = sn.ElementType();
	assert(pElemType);
	if (pElemType->IsArrayType())
	{
		//Resolve nested element type for array-of-arrays (future extension).
		m_Env.Log(CLL_Error, sn.Location(),
			"Multi-dimensional arrays are not supported.");
		return;
	}
	pElemType->Accept(*m_pVisitor);
	if (!pElemType->IsResolved())
		return;

	auto pElemField = pElemType->Field();
	if (!pElemField)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"\"%s\" is not a valid array element type.",
			pElemType->ToString().c_str());
		return;
	}

	//Resolve size expression with direct Accept (SnExpressionList wrapping is broken).
	sn.Size()->Accept(*m_pVisitor);
	if (!sn.Size()->IsResolved())
		return;

	//EvalDataType briefly stores the element type, then BindArrayTypeToken
	//overwrites it with the interned array token (the array-valued
	//property). Codegen's array registration reads the AST element type
	//expression, not this slot.
	sn.EvalDataType(pElemField);
	sn.AddFlags(NF_Resolved);
	BindArrayTypeToken(sn);
}

//0.9.0 (Phase A): resolve a bare name to a class or struct the way
//Access(SnNewExpr&) resolves its type name (builtin table first, then
//the scope chain), but as a silent probe — a miss must not add a second
//diagnostic next to the caller's own failure text. Null = the name does
//not address a constructible type (the caller falls through to its
//generic failure). 0.9.11 D2: structs join classes as targets.
SnField *ExprResolveAccessor::ProbeCtorType(const std::string &name,
	const ISourceLocation *pLoc)
{
	if (IsBuiltinClassName(name))
		return GetBuiltinClassDecl(name, pLoc);
	//0.9.12 D-new-6: a ctor-call position names a *type*, so the scope
	//walk must skip value hits instead of stopping at the first one —
	//inside the class the name is shadowed by its own constructor
	//member, and FindFieldInAncestor would return that member, fail
	//the kind check and answer null (the class could never be built
	//from its own methods). FindTypeNameInAncestor keeps walking past
	//the ctor member to the type binding.
	auto *pField = FindTypeNameInAncestor(name, *m_pContext, *m_pAccessor,
		false);
	if (pField && (pField->Kind() == NK_ClassDecl
		|| pField->Kind() == NK_StructDecl))
	{
		return pField;
	}
	//0.9.0: `P(args)` with `using P = Point;` — the invoke callee sits in
	//expression position, where the alias pre-pass never expands; probe
	//the TU alias table last. Silent on a miss: the caller owns the
	//generic failure text.
	return ProbeAliasType(name, pLoc);
}

//Bare `Foo(args)` without new: bind the invoke as a constructor call.
//Runs ONLY in the invoke-resolution failure branch (functions keep
//priority), with the arguments already resolved. True = consumed —
//resolved as a ctor call, or rejected through the shared selection
//diagnostics; false = the name addresses no class, the caller reports
//its own generic failure.
bool ExprResolveAccessor::TryResolveCtorInvoke(SnInvokeExpr &snInvoke)
{
	auto *pTypeDecl = ProbeCtorType(snInvoke.CalleeName(),
		snInvoke.Location());
	if (!pTypeDecl)
	{
		//0.9.0: List/Dict exist only as typed instantiations — no class
		//declaration a probe could find. Name the fix instead of letting
		//the generic function-not-found text through. The probe runs
		//first: a user class named List is legal and must construct.
		if (IsBuiltinGenericTypeName(snInvoke.CalleeName()))
		{
			m_Env.Log(CLL_Error, snInvoke.Location(),
				"generic class \"%s\" cannot be constructed by bare name; "
				"use [] or {} with a typed declaration",
				snInvoke.CalleeName().c_str());
			return true;
		}
		return false;
	}
	//ResolveFieldExprAs carries the type in both Field() (member chains
	//like Point(1,4).x assert on it via IsDataExpr) and EvalDataType
	//(SnClassDecl/SnStructDecl is its own type). 0.9.11 D2: the kind
	//decides which ctor slot the invoke carries; CtorFunction() records
	//the selected constructor for codegen and the frame walkers. The
	//parameters are already resolved (this runs in the invoke failure
	//branch), so the overload match reads them directly.
	ResolveFieldExprAs(snInvoke, pTypeDecl);
	if (pTypeDecl->Kind() == NK_StructDecl)
		snInvoke.CtorStruct(static_cast<SnStructDecl*>(pTypeDecl));
	else
		snInvoke.CtorClass(static_cast<SnClassDecl*>(pTypeDecl));
	std::vector<SnExpression*> actuals;
	for (auto &param : snInvoke.Params())
		actuals.push_back(&param);
	snInvoke.CtorFunction(SelectCtorForArgs(pTypeDecl, actuals,
		snInvoke.Location()));
	return true;
}

//Module-qualified `shapes.Point(args)` without new: the module table has
//no function of the name and the type table does. Member wiring mirrors
//BindQualifiedInvoke (direct field/type slots; the caller's
//FinishConsumedMember adds the resolved flags). 0.9.11 D2: structs join
//classes as module-qualified ctor targets.
bool ExprResolveAccessor::TryResolveQualifiedCtorInvoke(SnMemberExpr &snMember,
	SnInvokeExpr &invoke, const std::string &modulePath)
{
	auto *pType = m_Env.Registry().FindModuleType(modulePath,
		invoke.CalleeName());
	if (!pType
		|| (pType->Kind() != NK_ClassDecl && pType->Kind() != NK_StructDecl))
	{
		//0.9.0: the qualified twin of the bare-name reject. Builtin generics
		//own no module declaration (FindModuleType scans unit-owned AST
		//members only), so without this check `shapes.List()` would fall
		//through to the generic function-not-found text. The type lookup
		//runs first: an imported class named List is legal and must
		//construct. Consuming with the logged error mirrors
		//RejectUnimportedModuleCall — the caller's
		//FinishConsumedMember flags the member and the error keeps
		//codegen out.
		if (IsBuiltinGenericTypeName(invoke.CalleeName()))
		{
			m_Env.Log(CLL_Error, invoke.Location(),
				"generic class \"%s\" cannot be constructed by bare name; "
				"use [] or {} with a typed declaration",
				invoke.CalleeName().c_str());
			return true;
		}
		return false;
	}
	ResolveFieldExprAs(invoke, pType);
	if (pType->Kind() == NK_StructDecl)
		invoke.CtorStruct(static_cast<SnStructDecl*>(pType));
	else
		invoke.CtorClass(static_cast<SnClassDecl*>(pType));
	std::vector<SnExpression*> actuals;
	for (auto &param : invoke.Params())
		actuals.push_back(&param);
	auto *pCtor = SelectCtorForArgs(pType, actuals, invoke.Location());
	invoke.CtorFunction(pCtor);
	//0.9.12 D-new-7: Field() answers "what was called" — bind the selected
	//constructor so the invoke-receiver classifier sees a value-producing
	//call here, not a type name (ReceiverNamesType treats a member whose
	//Field() is a type declaration as a type name, which mis-routes
	//`lib.Calc().add(5)` into the type-name receiver reject). EvalDataType
	//answers "what it produces" and stays the type: the access context
	//switch and codegen read that channel. On failed selection (the
	//diagnostic is already logged) keep the type binding — SnFieldExpr::
	//IsDataExpr dereferences Field() unconditionally.
	snMember.m_pField = pCtor ? pCtor : pType;
	snMember.EvalDataType(pType);
	return true;
}

} //namespace nlang