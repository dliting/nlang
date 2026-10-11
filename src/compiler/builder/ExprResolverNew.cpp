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

//Count the positional ctor arguments of a new expression.
size_t ExprResolveAccessor::CountPositionalCtorArgs(SnNewExpr &sn)
{
	size_t argCount = 0;
	for (auto &arg : sn.Args())
	{
		if (&arg == sn.ClassName()) continue;  //Args() view includes it
		if (RejectNonPositionalCtorArg(m_Env, arg))
			continue;
		++argCount;
	}
	return argCount;
}

//Ctor-invoke twin: SnInvokeExpr::Params() has no class-name child.
size_t ExprResolveAccessor::CountPositionalCtorArgs(SnInvokeExpr &sn)
{
	size_t argCount = 0;
	for (auto &arg : sn.Params())
	{
		if (RejectNonPositionalCtorArg(m_Env, arg))
			continue;
		++argCount;
	}
	return argCount;
}

//Expected ctor arity. User classes look up the ctor SnFunction; the
//rest are intrinsic ctor stubs registered by VmBackend::RegisterBuiltin
//Classes (arity excludes `this`). False = the class has no constructor.
bool ExprResolveAccessor::FindCtorArity(SnClassDecl *pClassDecl,
	size_t &ctorArity)
{
	ctorArity = 0;
	if (IsGenericClassDecl(pClassDecl))
		return true;  //List/Dict ctor stubs take only `this`
	if (pClassDecl->IsBuiltinClass())
	{
		//FileStream(this, path, mode) → 2; Exception family
		//(this, message) → 1; Object/ByteStream → 0.
		const auto& clsName = pClassDecl->Name();
		if (clsName == "FileStream") ctorArity = 2;
		else if (IsBuiltinExceptionClassName(clsName)) ctorArity = 1;
		return true;
	}
	for (auto& member : pClassDecl->Members())
	{
		if (member.Kind() == NK_Function
			&& member.Name() == pClassDecl->Name())
		{
			ctorArity = static_cast<SnFunction&>(member).Params().size();
			return true;
		}
	}
	return false;
}

//Constructor-call arity check, shared by new Foo(args) and the 0.9.0
//ctor-invoke forms. Round-14: constructor calls emit their args
//positionally without FormalBindings (VmBackend's NewExpr handler), so —
//like super(...) — named arguments cannot bind, defaults declared on
//ctor params are not applied at the call site, and the arity must match
//exactly. Pre-fix a mismatched call compiled clean: missing args read
//uninitialized callParam slots (a defaulted param arrived as garbage),
//extras were silently dropped, named args hit codegen's unhandled-kind
//internal error. Imported class stubs are skipped — their ctor lives
//only in the merged CompiledClass, not in AST members.
void ExprResolveAccessor::CheckCtorArgCount(const ISourceLocation *pLoc,
	SnClassDecl *pClassDecl, size_t argCount)
{
	if (pClassDecl->IsImported())
		return;
	size_t ctorArity = 0;
	if (!FindCtorArity(pClassDecl, ctorArity))
	{
		if (argCount != 0)
			m_Env.Log(CLL_Error, pLoc,
				"class \"%s\" has no constructor; new %s() cannot take "
				"arguments",
				pClassDecl->Name().c_str(), pClassDecl->Name().c_str());
	}
	else if (argCount != ctorArity)
	{
		m_Env.Log(CLL_Error, pLoc,
			"constructor of \"%s\" expects %zu argument(s), got %zu",
			pClassDecl->Name().c_str(), ctorArity, argCount);
	}
}

void ExprResolveAccessor::CheckNewExprCtorArity(SnNewExpr &sn,
	SnClassDecl *pClassDecl)
{
	CheckCtorArgCount(sn.Location(), pClassDecl,
		CountPositionalCtorArgs(sn));
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

void ExprResolveAccessor::Access(SnNewExpr &sn)
{
	assert(!sn.IsResolved());

	auto pClassName = sn.ClassName();
	assert(pClassName);
	pClassName->Accept(*m_pVisitor);
	TryResolveBuiltinClassName(*pClassName);

	if (!pClassName->IsResolved())
		return;

	auto pClassField = pClassName->Field();
	if (!pClassField || pClassField->Kind() != NK_ClassDecl)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"\"%s\" is not a class type.", pClassName->ToString().c_str());
		return;
	}

	auto pClassDecl = static_cast<SnClassDecl*>(pClassField);
	//Phase 13: func types are structural — values come only from function
	//or method references, so there is no by-name construction.
	if (IsGenericClassDecl(pClassDecl)
		&& pClassDecl->BaseName() == kBuiltinFuncTypeName)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"func types cannot be constructed by name; bind a function "
			"or method reference");
		return;
	}
	sn.ClassDecl(pClassDecl);
	sn.EvalDataType(pClassDecl);
	sn.AddFlags(NF_Resolved);

	CheckNewExprCtorArity(sn, pClassDecl);

	if (!ResolveExpressionList(sn.Args()))
		return;
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

//0.9.0 (Phase A): resolve a bare name to a class the way
//Access(SnNewExpr&) resolves its class name (builtin table first, then
//the scope chain), but as a silent probe — a miss must not add a second
//diagnostic next to the caller's own failure text. Null = the name does
//not address a class (the caller falls through to its generic failure).
SnClassDecl *ExprResolveAccessor::ProbeCtorClass(const std::string &name,
	const ISourceLocation *pLoc)
{
	if (IsBuiltinClassName(name))
		return GetBuiltinClassDecl(name, pLoc);
	auto *pField = FindFieldInAncestor(name, *m_pContext, *m_pAccessor,
		Flags());
	if (pField && pField->Kind() == NK_ClassDecl)
		return static_cast<SnClassDecl *>(pField);
	//0.9.0: `P(args)` with `using P = Point;` — the invoke callee sits in
	//expression position, where the alias pre-pass never expands; probe
	//the TU alias table last. Silent on a miss: the caller owns the
	//generic failure text.
	return ProbeAliasClass(name, pLoc);
}

//Bare `Foo(args)` without new: bind the invoke as a constructor call.
//Runs ONLY in the invoke-resolution failure branch (functions keep
//priority), with the arguments already resolved. True = consumed —
//resolved as a ctor call, or rejected through the shared arity check;
//false = the name addresses no class, the caller reports its own
//generic failure.
bool ExprResolveAccessor::TryResolveCtorInvoke(SnInvokeExpr &snInvoke)
{
	auto *pClassDecl = ProbeCtorClass(snInvoke.CalleeName(),
		snInvoke.Location());
	if (!pClassDecl)
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
	//ResolveFieldExprAs carries the class in both Field() (member chains
	//like Point(1,4).x assert on it via IsDataExpr) and EvalDataType
	//(SnClassDecl is its own type).
	ResolveFieldExprAs(snInvoke, pClassDecl);
	snInvoke.CtorClass(pClassDecl);
	CheckCtorArgCount(snInvoke.Location(), pClassDecl,
		CountPositionalCtorArgs(snInvoke));
	return true;
}

//Module-qualified `shapes.Point(args)` without new: the module table has
//no function of the name and the type table does. Member wiring mirrors
//BindQualifiedInvoke (direct field/type slots; the caller's
//FinishConsumedMember adds the resolved flags).
bool ExprResolveAccessor::TryResolveQualifiedCtorInvoke(SnMemberExpr &snMember,
	SnInvokeExpr &invoke, const std::string &modulePath)
{
	auto *pType = m_Env.Registry().FindModuleType(modulePath,
		invoke.CalleeName());
	if (!pType || pType->Kind() != NK_ClassDecl)
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
	auto *pClassDecl = static_cast<SnClassDecl *>(pType);
	ResolveFieldExprAs(invoke, pClassDecl);
	invoke.CtorClass(pClassDecl);
	snMember.m_pField = pClassDecl;
	snMember.EvalDataType(pClassDecl);
	CheckCtorArgCount(invoke.Location(), pClassDecl,
		CountPositionalCtorArgs(invoke));
	return true;
}

} //namespace nlang