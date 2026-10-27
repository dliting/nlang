/*---
    ExprResolverNew.cpp — 分配表达式解析（new/new[]；初始化列表见
    ExprResolverInitList.cpp；构造器选择见 ExprResolverNewCtor.cpp）
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
		//Thread used to gate by-name construction here as a synthesized
		//builtin; as a declared class its empty ctor surface says it all
		//(with-args rejects on the no-constructor arm, the zero-arg form
		//is the legal bare-allocation shape). Mutex/Channel stay
		//constructible (their handles own the resource).
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
			//K3 task 4: `Channel<int> c = Channel(n);` — the assign
			//resolver published the declared LHS type; a Channel target
			//binds the invoke as the bounded-capacity ctor call.
			//List/Dict keep the reject: their bare names read as
			//element lists, a different contract.
			if (snInvoke.CalleeName() == kBuiltinChannelTypeName
				&& TryBindChannelCtorInvoke(snInvoke))
				return true;
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

//K3 task 4: bind a bare `Channel(n)` invoke as the bounded-capacity ctor
//call when the assignment target is a Channel<T> instantiation. The hint
//is consumed unconditionally (null outside an assignment's RHS — the
//reject below is unreachable there). Returns true = consumed (bound, or
//rejected with a named diagnostic); false = no usable Channel target.
//Capacity contract: exactly one int argument (builtin invokes stage
//exact arity — an unstaged capacity slot would read evalArea garbage);
//0 = unbounded, n >= 1 = bounded, n < 0 rejected at run time. The
//unbounded default keeps the `{}` init-list form.
bool ExprResolveAccessor::TryBindChannelCtorInvoke(SnInvokeExpr &snInvoke)
{
	auto *pTarget = m_pCtorInvokeTarget;
	m_pCtorInvokeTarget = nullptr;
	auto *pChannelType = pTarget ? pTarget->EvalDataType() : nullptr;
	auto *pChannel = (pChannelType && pChannelType->Kind() == NK_ClassDecl)
		? static_cast<SnClassDecl *>(pChannelType) : nullptr;
	if (!pChannel || !pChannel->IsGenericInstantiation()
		|| pChannel->BaseName() != kBuiltinChannelTypeName)
		return false;
	SnExpression *pCapacityArg = nullptr;
	size_t positionalCount = 0;
	for (auto &arg : snInvoke.Params())
	{
		if (RejectNonPositionalCtorArg(arg))
			continue;
		if (!pCapacityArg)
			pCapacityArg = &arg;
		++positionalCount;
	}
	if (positionalCount != 1)
	{
		m_Env.Log(CLL_Error, snInvoke.Location(),
			"channel constructor takes one capacity argument "
			"(0 = unbounded); use \"{}\" for the unbounded default");
		return true;
	}
	auto *pArgType = pCapacityArg->EvalDataType();
	if (!pArgType || pArgType->Kind() != NK_Int32)
	{
		m_Env.Log(CLL_Error, pCapacityArg->Location(),
			"channel capacity must be an int");
		return true;
	}
	ResolveFieldExprAs(snInvoke, pChannel);
	snInvoke.CtorClass(pChannel);
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