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

//Arity of one ctor declaration: requiredArity counts the formals without a
//default expression, totalArity all of them.
static void CountCtorArity(const SnFunction &ctor,
	size_t &requiredArity, size_t &totalArity)
{
	totalArity = ctor.Params().size();
	requiredArity = totalArity;
	for (auto &param : ctor.Params())
	{
		if (param.Value())
			--requiredArity;
	}
}

//Expected ctor arity range. User classes and structs take the same
//declaration the call site binds to (SelectCtorFunction — the first
//declaration-order ctor; T6 swaps in overload matching). The rest are
//intrinsic ctor stubs registered by VmBackend::RegisterBuiltinClasses
//(arity excludes `this`); requiredArity and totalArity stay equal where
//defaults cannot exist (builtins/generics). False = no constructor.
bool ExprResolveAccessor::FindCtorParamRange(SnField *pTypeDecl,
	size_t &requiredArity, size_t &totalArity)
{
	requiredArity = 0;
	totalArity = 0;
	if (auto *pCtor = SelectCtorFunction(pTypeDecl))
	{
		CountCtorArity(*pCtor, requiredArity, totalArity);
		return true;
	}
	if (pTypeDecl->Kind() != NK_ClassDecl)
		return false;  //struct without a constructor
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

//0.9.11 D2: the constructor a call site binds to. Structs take the first
//declaration-order Ctors() entry; user classes the first same-name
//non-static member (a same-name static lives in the static namespace,
//0.9.10 D2.4). Builtin and generic classes answer null — their ctor is an
//intrinsic stub living only in the CompiledClass, reached through
//FindCtorFunction at codegen; imported stubs answer null too (their AST
//carries no ctor member — T5 wires the cross-module path). Until T6
//replaces the first-scan with overload matching, codegen and the frame
//walkers must use this AST-recorded selection, never a fresh scan.
SnFunction *ExprResolveAccessor::SelectCtorFunction(SnField *pTypeDecl)
{
	if (pTypeDecl->Kind() == NK_StructDecl)
	{
		for (auto &ctor : static_cast<SnStructDecl*>(pTypeDecl)->Ctors())
			return &ctor;
		return nullptr;
	}
	auto *pClassDecl = static_cast<SnClassDecl*>(pTypeDecl);
	if (IsGenericClassDecl(pClassDecl) || pClassDecl->IsBuiltinClass())
		return nullptr;
	for (auto &member : pClassDecl->Members())
	{
		if (member.Kind() == NK_Function
			&& member.Name() == pClassDecl->Name()
			&& !member.ContainFlags(NF_Static))
		{
			return static_cast<SnFunction*>(&member);
		}
	}
	return nullptr;
}

//Constructor-call arity check for the ctor-invoke forms (bare and
//module-qualified). Constructor calls stage their arguments positionally
//(one FormalBinding per formal — caller args first, then the defaults of
//the omitted trailing formals), so named and out arguments stay rejected
//and the count is a range: a call may omit any trailing defaulted formal,
//never a required one. Pre-fix a mismatched call compiled clean: missing
//args read uninitialized callParam slots (a defaulted param arrived as
//garbage), extras were silently dropped, named args hit codegen's
//unhandled-kind internal error. Imported type stubs are skipped — their
//ctor lives only in the merged CompiledClass/CompiledStruct, not in AST
//members.
void ExprResolveAccessor::CheckCtorArgCount(const ISourceLocation *pLoc,
	SnField *pTypeDecl, size_t argCount)
{
	if (pTypeDecl->IsImported())
		return;
	const char *kindWord = pTypeDecl->Kind() == NK_StructDecl
		? "struct" : "class";
	size_t requiredArity = 0;
	size_t totalArity = 0;
	if (!FindCtorParamRange(pTypeDecl, requiredArity, totalArity))
	{
		if (argCount != 0)
			m_Env.Log(CLL_Error, pLoc,
				"%s \"%s\" has no constructor; %s() cannot take "
				"arguments",
				kindWord, pTypeDecl->Name().c_str(),
				pTypeDecl->Name().c_str());
	}
	else if (argCount < requiredArity || argCount > totalArity)
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
}

void ExprResolveAccessor::CheckNewExprCtorArity(SnNewExpr &sn,
	SnField *pTypeDecl)
{
	CheckCtorArgCount(sn.Location(), pTypeDecl,
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
	sn.CtorFunction(SelectCtorFunction(pTypeField));
	sn.EvalDataType(pTypeField);
	sn.AddFlags(NF_Resolved);

	CheckNewExprCtorArity(sn, pTypeField);

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
	auto *pField = FindFieldInAncestor(name, *m_pContext, *m_pAccessor,
		Flags());
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
//resolved as a ctor call, or rejected through the shared arity check;
//false = the name addresses no class, the caller reports its own
//generic failure.
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
	//the selected constructor for codegen and the frame walkers.
	ResolveFieldExprAs(snInvoke, pTypeDecl);
	if (pTypeDecl->Kind() == NK_StructDecl)
		snInvoke.CtorStruct(static_cast<SnStructDecl*>(pTypeDecl));
	else
		snInvoke.CtorClass(static_cast<SnClassDecl*>(pTypeDecl));
	snInvoke.CtorFunction(SelectCtorFunction(pTypeDecl));
	CheckCtorArgCount(snInvoke.Location(), pTypeDecl,
		CountPositionalCtorArgs(snInvoke));
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
	invoke.CtorFunction(SelectCtorFunction(pType));
	snMember.m_pField = pType;
	snMember.EvalDataType(pType);
	CheckCtorArgCount(invoke.Location(), pType,
		CountPositionalCtorArgs(invoke));
	return true;
}

} //namespace nlang