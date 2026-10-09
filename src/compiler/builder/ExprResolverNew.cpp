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
#include "ModuleRegistry.h"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Count the positional ctor arguments, rejecting the forms constructor
//calls cannot lower: named arguments (args emit positionally without
//FormalBindings) and out arguments (no writeback path in NewExpr).
size_t ExprResolveAccessor::CountPositionalCtorArgs(SnNewExpr &sn)
{
	size_t argCount = 0;
	for (auto &arg : sn.Args())
	{
		if (&arg == sn.ClassName()) continue;  //Args() view includes it
		if (arg.Kind() == NK_NamedArgExpr)
		{
			m_Env.Log(CLL_Error, arg.Location(),
				"named arguments are not supported in constructor calls");
			continue;
		}
		if (arg.Kind() == NK_OutArgExpr)
		{
			m_Env.Log(CLL_Error, arg.Location(),
				"out arguments are not supported in constructor calls.");
			continue;
		}
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

//Round-14: constructor calls emit their args positionally without
//FormalBindings (VmBackend's NewExpr handler), so — like super(...) —
//named arguments cannot bind, defaults declared on ctor params are not
//applied at the call site, and the arity must match exactly. Pre-fix a
//mismatched call compiled clean: missing args read uninitialized
//callParam slots (a defaulted param arrived as garbage), extras were
//silently dropped, named args hit codegen's unhandled-kind internal
//error. Imported class stubs are skipped — their ctor lives only in the
//merged CompiledClass, not in AST members.
void ExprResolveAccessor::CheckNewExprCtorArity(SnNewExpr &sn,
	SnClassDecl *pClassDecl)
{
	if (pClassDecl->IsImported())
		return;
	size_t argCount = CountPositionalCtorArgs(sn);
	size_t ctorArity = 0;
	if (!FindCtorArity(pClassDecl, ctorArity))
	{
		if (argCount != 0)
			m_Env.Log(CLL_Error, sn.Location(),
				"class \"%s\" has no constructor; new %s() cannot take "
				"arguments",
				pClassDecl->Name().c_str(), pClassDecl->Name().c_str());
	}
	else if (argCount != ctorArity)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"constructor of \"%s\" expects %zu argument(s), got %zu",
			pClassDecl->Name().c_str(), ctorArity, argCount);
	}
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

} //namespace nlang