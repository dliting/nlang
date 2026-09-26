/*---
    ExprResolverNew.cpp — 分配表达式解析（new/new[]/初始化列表）
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
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

void ExprResolveAccessor::Access(SnNewExpr &sn)
{
	assert(!sn.IsResolved());

	auto pClassName = sn.ClassName();
	assert(pClassName);
	pClassName->Accept(*m_pVisitor);

	//Builtin class names: ByteStream, FileStream, Object.
	//Phase 9d: Exception hierarchy.
	//These don't exist in the AST namespace, so the name won't resolve
	//through the normal path. Use the singleton SnClassDecl.
	if (!pClassName->IsResolved())
	{
		const auto& name = pClassName->ToString();
		if (IsBuiltinClassName(name))
		{
			ResolveFieldExprAs(*pClassName,
				GetBuiltinClassDecl(name, pClassName->Location()));
		}
	}

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
	//Phase 13: Func types are structural — values come only from function
	//or method references, so there is no by-name construction.
	if (IsGenericClassDecl(pClassDecl) && pClassDecl->BaseName() == "Func")
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Func types cannot be constructed by name; bind a function "
			"or method reference");
		return;
	}
	sn.ClassDecl(pClassDecl);
	sn.EvalDataType(pClassDecl);
	sn.AddFlags(NF_Resolved);

	//Round-14: constructor calls emit their args positionally without
	//FormalBindings (VmBackend's NewExpr handler), so — like super(...) —
	//named arguments cannot bind, defaults declared on ctor params are not
	//applied at the call site, and the arity must match exactly. Pre-fix a
	//mismatched call compiled clean: missing args read uninitialized
	//callParam slots (a defaulted param arrived as garbage), extras were
	//silently dropped, named args hit codegen's unhandled-kind internal
	//error. Imported class stubs are skipped — their ctor lives only in the
	//merged CompiledClass, not in AST members.
	if (!pClassDecl->IsImported())
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
		//Expected ctor arity. User classes look up the ctor SnFunction; the
		//rest are intrinsic ctor stubs registered by
		//VmBackend::RegisterBuiltinClasses (arity excludes `this`).
		size_t ctorArity = 0;
		bool hasCtor = false;
		if (IsGenericClassDecl(pClassDecl))
		{
			hasCtor = true;  //List/Dict ctor stubs take only `this`
		}
		else if (pClassDecl->IsBuiltinClass())
		{
			//FileStream(this, path, mode) → 2; Exception family
			//(this, message) → 1; Object/ByteStream → 0.
			hasCtor = true;
			const auto& clsName = pClassDecl->Name();
			if (clsName == "FileStream") ctorArity = 2;
			else if (IsBuiltinExceptionClassName(clsName)) ctorArity = 1;
		}
		else
		{
			for (auto& member : pClassDecl->Members())
			{
				if (member.Kind() == NK_Function
					&& member.Name() == pClassDecl->Name())
				{
					hasCtor = true;
					ctorArity = static_cast<SnFunction&>(member)
						.Params().size();
					break;
				}
			}
		}
		if (!hasCtor)
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

//Phase 8e-6: Collection initializer resolver.
//Two paths:
//- Explicit form `new Type{...}`: ExplicitType() carries the type
//  expression. Resolve it like a normal type, set EvalDataType to its
//  resolved Field.
//- Bare form `[...]`: ExplicitType() is null. The parent AssignStmt
//  resolver must have populated InferredTarget() from the LHS variable's
//  type. Use that directly as EvalDataType.
//Entry values are resolved last via direct Accept (children inherit
//no expected type for now — they resolve via their normal paths).
void ExprResolveAccessor::Access(SnInitListExpr &sn)
{
	assert(!sn.IsResolved());

	SnField *pTargetField = nullptr;
	bool bIsArray = false;

	if (auto *pExplicit = sn.ExplicitType())
	{
		//Resolve the explicit type expression (NameExpr/GenericTypeExpr).
		pExplicit->Accept(*m_pVisitor);
		if (!pExplicit->IsResolved())
			return;
		pTargetField = pExplicit->Field();
		bIsArray = pExplicit->IsArrayType();
	}
	else if (auto *pInferred = sn.InferredTarget())
	{
		//Bare form: parent populated the LHS variable. bIsArray keeps the
		//declaration-side signal (IsArrayType()); the type slot carries
		//the interned token for array variables, peeled to the element
		//below.
		bIsArray = pInferred->IsArrayType();
		pTargetField = pInferred->EvalDataType();
		//0.7.3 B token path: an
		//array-typed LHS carries the interned token; the init-list's
		//per-entry element gate consumes the ELEMENT (the node itself
		//keeps the element contract — codegen's RegisterArrayType reads
		//it directly).
		if (pTargetField
			&& pTargetField->Kind() == NK_ArrayTypeToken)
			pTargetField = static_cast<SnArrayTypeToken*>(
				pTargetField)->ElemTypeOf();
	}

	if (!pTargetField)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Collection initializer requires an explicit type or an LHS "
			"context to infer the target type.");
		return;
	}

	sn.EvalDataType(pTargetField);
	sn.TargetIsArray(bIsArray);
	sn.AddFlags(NF_Resolved);

	//Class init lists lower to `new C()` + per-field stores; the implicit
	//ctor call must not require arguments (spec: class init requires a
	//no-arg constructor, explicit or implicit). Without this check the
	//codegen emits the ctor call anyway and the VM copies garbage/reads
	//past the frame for the missing params.
	if (!bIsArray && pTargetField->Kind() == NK_ClassDecl) {
		auto* pClassDecl = static_cast<SnClassDecl*>(pTargetField);
		if (!pClassDecl->IsBuiltinClass()) {
			//Class initializers are identifier-keyed only: the codegen
			//per-field store dispatches on the key name, and declaration
			//order is meaningless with inherited fields (layout is
			//root-ancestor-first). Value-only entries used to be silently
			//skipped, leaving every field at its ctor default — reject
			//them instead (struct targets keep the ordinal form).
			for (auto& entry : sn.Entries()) {
				if (entry.keyKind != InitEntry::KeyKind::Identifier) {
					m_Env.Log(CLL_Error,
						entry.pValue ? entry.pValue->Location()
							: sn.Location(),
						"class initializer \"new %s{...}\" requires "
						"field:value entries; the positional form is "
						"only valid for struct/array targets",
						pClassDecl->Name().c_str());
					break;
				}
			}
			for (auto& field : pClassDecl->Members()) {
				if (field.Kind() == NK_Function
					&& field.Name() == pClassDecl->Name()) {
					size_t arity = static_cast<SnFunction&>(field)
						.Params().size();
					if (arity > 0) {
						m_Env.Log(CLL_Error, sn.Location(),
							"class initializer \"new %s{...}\" requires a "
							"no-arg constructor; \"%s\" takes %zu "
							"argument(s)",
							pClassDecl->Name().c_str(),
							pClassDecl->Name().c_str(), arity);
					}
					break;
				}
			}
		}
	}

	//Resolve each entry value. Keys (for {...} form) are not expressions
	//and need no resolution.
	for (auto &entry : sn.Entries())
	{
		if (entry.pValue)
			entry.pValue->Accept(*m_pVisitor);
	}

	//Phase 13: init-list entries bind pending function references to the
	//element type — the array element type, or T of a generic container
	//(`List<Func<int,int>> l = [bar];`). Class targets carry no element
	//type and are skipped.
	SnField *pElemType = nullptr;
	if (bIsArray)
	{
		pElemType = pTargetField;
	}
	else if (pTargetField->Kind() == NK_ClassDecl)
	{
		auto elemArgs = GetGenericTypeArgs(
			static_cast<SnClassDecl*>(pTargetField));
		if (!elemArgs.empty())
			pElemType = elemArgs[0];
	}
	if (pElemType)
	{
		for (auto &entry : sn.Entries())
		{
			if (!entry.pValue)
				continue;
			if (IsUnboundFuncRef(*entry.pValue))
			{
				BindFuncRefToExpected(m_Env,
					*static_cast<SnIdentifierExpr*>(entry.pValue),
					pElemType);
			}
			else if (IsUnboundMemberFuncRef(*entry.pValue))
			{
				BindMemberFuncRefToExpected(m_Env,
					*static_cast<SnMemberExpr*>(entry.pValue),
					pElemType);
			}
		}
	}

	//Array-form element-type gate: every entry is an element store, so
	//it takes the same conversion checks as `arr[i] = v` — the cast
	//table adjudicates (0.7.3 B): a mismatched entry is rejected by
	//FixupExprType (named array diagnostic for cross-element array
	//values), and the wrap makes the codegen's OP_StoreElement emit the
	//box/coercion. Container forms (List/Dict) already box through
	//their own per-method plans and are not array-form.
	if (bIsArray && pElemType)
	{
		const auto &entries = sn.Entries();
		for (size_t nIdx = 0; nIdx < entries.size(); ++nIdx)
		{
			auto *pValue = entries[nIdx].pValue;
			if (!pValue || !pValue->IsResolved()
				|| !pValue->EvalDataType())
				continue;
			TypeCastInfo castInfo(pValue->EvalDataType(), pElemType);
			auto iExpr = sn.Children().find(pValue);
			if (iExpr == sn.Children().end())
				continue;
			if (FixupExprType(iExpr, castInfo))
				sn.SetEntryValue(nIdx,
					&static_cast<SnCastExpr &>(*iExpr));
		}
	}
}

} //namespace nlang
