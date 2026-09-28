/*---
    ExprResolverMemberContainer.cpp — 内建泛型容器方法阶段（List<T> / Dict<K,V>）：元数校验、实参绑定与装箱、返回类型。
    从 ExprResolver.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
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

//Phase 8e-3 / 8e-4: built-in generic List<T> / Dict<K,V> method name sets.
static bool IsGenericContainerMethod(const std::string &baseName,
	const std::string &name)
{
	if (baseName == "List") {
		return (name == "add" || name == "get" || name == "set"
			|| name == "length" || name == "removeAt" || name == "indexOf"
			|| name == "contains" || name == "clear"
				|| name == "toString");
	} else if (baseName == "Dict") {
		return (name == "set" || name == "get"
			|| name == "containsKey" || name == "remove"
			|| name == "clear" || name == "count"
			|| name == "keys" || name == "toString");
	} else if (baseName == "Func") {
		//Phase 13: function handles expose toString only.
		return (name == "toString");
	}
	return false;
}

//Round-13: validate the argument count against the VM intrinsic
//stubs (VmBackend's addMethod tables). A mismatch previously
//slipped to codegen — extra args were silently ignored, missing
//args read uninitialized callParam slots. Logs on mismatch;
//returns true when the count is valid.
bool ExprResolveAccessor::CheckContainerMethodArity(SnInvokeExpr &invoke,
	const std::string &baseName, const std::string &name)
{
	static const std::map<std::string, size_t> kListMethodArities = {
		{"add", 1}, {"get", 1}, {"set", 2}, {"length", 0},
		{"removeAt", 1}, {"indexOf", 1}, {"contains", 1},
		{"clear", 0}, {"toString", 0},
	};
	static const std::map<std::string, size_t> kDictMethodArities = {
		{"set", 2}, {"get", 1}, {"containsKey", 1}, {"remove", 1},
		{"clear", 0}, {"count", 0}, {"keys", 0}, {"toString", 0},
	};
	static const std::map<std::string, size_t> kFuncMethodArities = {
		{"toString", 0},
	};
	const auto& arities = (baseName == "List")
		? kListMethodArities
		: (baseName == "Dict") ? kDictMethodArities
			: kFuncMethodArities;
	auto arityIt = arities.find(name);
	if (arityIt != arities.end() && ArgCountOf(invoke) != arityIt->second)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"Built-in method '%s' called with the wrong number of arguments.",
			name.c_str());
		return false;
	}
	return true;
}

//Phase 13 (D11 site 6, review round-1 F3): value positions by (base,
//method) — List add/indexOf/contains arg 0 → T; List set arg 1 → T
//(arg 0 is the int index); Dict get/containsKey/remove arg 0 → K;
//Dict set arg 1 → V (arg 0 is the key). Also flags the STORE value
//positions (List add/set value, Dict set value) which admit through
//the cast table — the same choke point as subscript stores (0.7.3 B
//T11): without this gate a mismatched value compiled and stored raw
//bits (an int into List<float> read back as a denormal, an array
//handle into List<int> into a primitive-traced slot). Read positions
//(get/indexOf/contains/containsKey/remove) probe by equality and
//stay ungated.
static int ContainerElemSlotFor(const std::string &baseName,
	const std::string &name, size_t &valArg)
{
	int elemSlot = -1;
	valArg = 0;
	if (baseName == "List"
		&& (name == "add" || name == "indexOf"
			|| name == "contains"))
		elemSlot = 0;
	else if (baseName == "List" && name == "set")
	{
		elemSlot = 0;
		valArg = 1;
	}
	else if (baseName == "Dict"
		&& (name == "get" || name == "containsKey"
			|| name == "remove"))
		elemSlot = 0;
	else if (baseName == "Dict" && name == "set")
	{
		elemSlot = 1;
		valArg = 1;
	}
	return elemSlot;
}

//Phase 13 (D11 site 6, review round-1 F3): built-in container
//methods dispatch by name — no overload set is ever scored, so a
//pending function reference in an argument never meets a formal
//and was rejected. Bind it here against the container's type
//argument at the value position.
bool ExprResolveAccessor::BindContainerMethodArgs(SnInvokeExpr &invoke,
	SnClassDecl *pGenClass, const std::string &baseName,
	const std::string &name)
{
	auto typeArgs = GetGenericTypeArgs(pGenClass);
	size_t valArg = 0;
	const int elemSlot = ContainerElemSlotFor(baseName, name, valArg);
	const bool isStoreValue = (baseName == "List"
			&& (name == "add" || name == "set"))
		|| (baseName == "Dict" && name == "set");
	if (elemSlot >= 0
		&& typeArgs.size() > static_cast<size_t>(elemSlot)
		&& typeArgs[elemSlot])
	{
		SnExpression* pWrapValue = nullptr;
		SnExpression* pWrapKey = nullptr;
		if (!BindContainerArgPositions(invoke, typeArgs, baseName, name,
			elemSlot, valArg, isStoreValue, pWrapValue, pWrapKey))
			return false;
		WrapContainerStoreArgs(invoke, typeArgs, elemSlot, pWrapKey,
			pWrapValue);
	}
	return true;
}

//One pass over the arguments: bind pending function references at the
//value position (in-loop — binds never touch the child list) and collect
//the store-value / Dict-key wrap candidates.
//Deferred-wrap discipline: the wraps are applied AFTER the loop, not here —
//FixupExprType frees the arg's list cell (Params() aliases Children()),
//and wrapping inside the range-for leaves its saved iterator dangling
//(heap-use-after-free on ++; manifests intermittently as SEGV, an endless
//loop, or a lucky pass). Dict.set also gates its KEY argument (arg 0
//against K): the key is stored when absent, and an ungated mismatched key
//corrupted the key-slot invariant (DictKeysEqual compares by the declared
//kind); read positions stay ungated per the read/write split.
bool ExprResolveAccessor::BindContainerArgPositions(SnInvokeExpr &invoke,
	const std::vector<SnField*> &typeArgs, const std::string &baseName,
	const std::string &name, int elemSlot, size_t valArg,
	bool isStoreValue, SnExpression *&rpWrapValue,
	SnExpression *&rpWrapKey)
{
	size_t argIdx = 0;
	for (auto &arg : invoke.Params())
	{
		SnExpression *pValue = (arg.Kind() == NK_NamedArgExpr)
			? static_cast<SnNamedArgExpr&>(arg).Inner() : &arg;
		if (argIdx == valArg)
		{
			if (IsUnboundFuncRef(*pValue))
			{
				if (!BindFuncRefToExpected(m_Env,
					*static_cast<SnIdentifierExpr*>(pValue),
					typeArgs[elemSlot]))
					return false;
			}
			else if (IsUnboundMemberFuncRef(*pValue))
			{
				if (!BindMemberFuncRefToExpected(m_Env,
					*static_cast<SnMemberExpr*>(pValue),
					typeArgs[elemSlot]))
					return false;
			}
			else if (isStoreValue && pValue->IsResolved()
				&& pValue->EvalDataType())
			{
				rpWrapValue = pValue;
			}
		}
		else if (baseName == "Dict" && name == "set"
			&& argIdx == 0 && typeArgs[0]
			&& pValue->IsResolved()
			&& pValue->EvalDataType())
		{
			rpWrapKey = pValue;
		}
		++argIdx;
	}
	return true;
}

//Deferred wraps (see BindContainerArgPositions): the element type-arg is
//the interned token for array elements, so the cast table sees full type
//identity — Same/Box/Auto wrap transparently under codegen's per-method
//boxing plan, None rejects.
void ExprResolveAccessor::WrapContainerStoreArgs(SnInvokeExpr &invoke,
	const std::vector<SnField*> &typeArgs, int elemSlot,
	SnExpression *pWrapKey, SnExpression *pWrapValue)
{
	if (pWrapKey)
	{
		auto keyCast = GetCastInfo(
			pWrapKey->EvalDataType(), typeArgs[0]);
		auto iKey = invoke.Children().find(pWrapKey);
		FixupExprType(iKey, keyCast);
	}
	if (pWrapValue)
	{
		auto castInfo = GetCastInfo(
			pWrapValue->EvalDataType(),
			typeArgs[elemSlot]);
		auto iArg = invoke.Children().find(pWrapValue);
		FixupExprType(iArg, castInfo);
	}
}

//Return-type computation for the builtin container methods:
// - List Add/Set/RemoveAt/Clear, Dict Set/Clear: void (no EvalDataType)
// - List Contains, Dict ContainsKey: bool (0.7.5 membership predicates)
// - List Length/IndexOf, Dict Remove/Count: int
// - List Get: T (typeArgs[0]); Dict Get: V (typeArgs[1])
//All elements at runtime are heap idxs (boxed primitives or class refs);
//VmBackend emits OP_Box/OP_Unbox around primitive-typed call sites.
SnField *ExprResolveAccessor::ComputeContainerMethodResult(
	SnMemberExpr &snMember, SnInvokeExpr &invoke,
	const std::vector<SnField*> &typeArgs, const std::string &baseName,
	const std::string &name)
{
	SnField* pResultField = nullptr;
	if (baseName == "List" && name == "get") {
		//Return type = T (typeArgs[0]).
		if (!typeArgs.empty() && typeArgs[0]) {
			snMember.EvalDataType(typeArgs[0]);
			pResultField = typeArgs[0];
		}
	} else if (baseName == "Dict" && name == "get") {
		//Return type = V (typeArgs[1]).
		if (typeArgs.size() > 1 && typeArgs[1]) {
			snMember.EvalDataType(typeArgs[1]);
			pResultField = typeArgs[1];
		}
	} else if ((baseName == "List" && name == "contains")
		|| (baseName == "Dict" && name == "containsKey")
	) {
		//0.7.5: membership predicates return bool (the intrinsics
		//already wrote 0/1 into the int32 carrier slot).
		auto* pBool = SnBuiltinDataType::InstanceOf(NK_Bool);
		snMember.EvalDataType(pBool);
		pResultField = pBool;
	} else if (
		(baseName == "List"
			&& (name == "length" || name == "indexOf"))
		|| (baseName == "Dict"
			&& (name == "remove" || name == "count"))
	) {
		auto* pInt = SnBuiltinDataType::InstanceOf(NK_Int32);
		snMember.EvalDataType(pInt);
		pResultField = pInt;
	} else if (baseName == "Dict" && name == "keys") {
		//Phase 8e-5: Dict.Keys() returns List<K> (K = typeArgs[0]),
		//synthesized so foreach lowering and codegen's boxing plan see
		//the right element type. 0.7.3 B: an array-typed K flows as the
		//interned token — pointer identity keeps List<int[]> distinct.
		if (!typeArgs.empty() && typeArgs[0]) {
			std::vector<SnField*> listArgs{ typeArgs[0] };
			auto* pListClass = GetGenericClassDecl("List", listArgs,
				{}, invoke.Location());
			if (pListClass) {
				//SnClassDecl IS-A SnField, so it can serve as EvalDataType.
				snMember.EvalDataType(pListClass);
				pResultField = pListClass;
			}
		}
	} else if (name == "toString") {
		//Phase 9b-pre: List/Dict toString() returns string.
		auto* pStr = SnBuiltinDataType::InstanceOf(NK_String);
		snMember.EvalDataType(pStr);
		pResultField = pStr;
	}
	return pResultField;
}

//Phase 8e-3 / 8e-4: built-in generic List<T> / Dict<K,V> methods.
//Synthetic generic SnClassDecl carries no real method members; dispatch
//by name here.
bool ExprResolveAccessor::TryResolveGenericContainerMethod(
	SnMemberExpr &snMember, SnFieldExpr *pInnerExpr,
	SyntaxNode *pSavedContext)
{
	if (!(m_pContext && m_pContext->Kind() == NK_ClassDecl
		&& IsGenericClassDecl(static_cast<SnClassDecl*>(m_pContext))
		&& pInnerExpr->Kind() == NK_InvokeExpr))
		return false;
	auto* pGenClass = static_cast<SnClassDecl*>(m_pContext);
	auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	const auto& name = invoke.CalleeName();
	const auto& baseName = pGenClass->BaseName();
	if (!IsGenericContainerMethod(baseName, name))
		return false;
	//Round-12/14: by-name dispatch cannot bind name = value args; out
	//args cannot write back.
	if (RejectNamedOrOutArguments(invoke))
	{
		m_pContext = pSavedContext;
		return true;
	}
	if (!CheckContainerMethodArity(invoke, baseName, name))
	{
		m_pContext = pSavedContext;
		return true;
	}
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	pInnerExpr->AddFlags(NF_Resolved);
	auto typeArgs = GetGenericTypeArgs(pGenClass);
	if (!BindContainerMethodArgs(invoke, pGenClass, baseName, name))
		return true;  //bind failed — logged; context already restored
	SnField* pResultField = ComputeContainerMethodResult(snMember, invoke,
		typeArgs, baseName, name);
	// Set m_pField directly (not via ResolveFieldExprAs
	// which would overwrite EvalDataType with SnType).
	// Needed so IsDataExpr() doesn't crash when chained
	// (e.g. lst.Get(0).length()).
	if (pResultField)
		snMember.m_pField = pResultField;
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}

} //namespace nlang
