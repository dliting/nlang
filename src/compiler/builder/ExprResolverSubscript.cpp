/*---
    ExprResolverSubscript.cpp — 下标表达式解析（arr[i] / s[i] / 容器
    语法糖 / `Type[n]` 定长创建孪生）
    从 ExprResolverValues.cpp 抽取（2026-10-19 0.9.11 T2，源文件 500 行守卫）。
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
#include <nlang/runtime/Config.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//0.7.5 char bridge: s[i] resolves as a BYTE read (ubyte result) —
//byte semantics stay consistent with length/substring/indexOf; the
//code-point surfaces are string foreach and charAt. 0.7.3 B D3
//rejected the shape outright; the byte read replaces that reject.
//The index must be an integer kind (a char index would silently
//misread as a byte offset — code-point access is foreach/charAt);
//write access stays rejected in StatementResolverAssign (strings
//are immutable). True = consumed (resolved or diagnosed).
bool ExprResolveAccessor::ResolveStringSubscript(SnSubscriptExpr &sn,
	SnField *pBaseType)
{
	if (!pBaseType || pBaseType->Kind() != NK_String)
		return false;
	auto* pIdxType = sn.Index()->IsResolved()
		? sn.Index()->EvalDataType() : nullptr;
	if (pIdxType)
	{
		int pi = ScalarPrimIndexOf(pIdxType->Kind());
		const bool isInteger = pi >= 0
			&& (kScalarPrims[pi].category == PC_SInt
				|| kScalarPrims[pi].category == PC_UInt);
		if (!isInteger)
		{
			m_Env.Log(CLL_Error, sn.Index()->Location(),
				"the string index must be an integer.");
			return true;
		}
	}
	sn.EvalDataType(SnBuiltinDataType::InstanceOf(NK_UByte));
	sn.AddFlags(NF_Resolved);
	return true;
}

//List<T>/Dict<K,V> subscript (li[i] / d[k]): sugar over get().
//The base resolves to a synthetic generic-instantiation class; the
//element type is T (List) or V (Dict). Without this peel the
//subscript keeps the container type and every consumer (assignment,
//member chains, nested subscripts) mis-types it.
//
//0.7.3 B: an array-valued base carries the interned token in
//EvalDataType (NK_ArrayTypeToken, never a ClassDecl), so the Kind()
//check alone distinguishes `List<int>[] a` from `List<int> li` —
//the masquerade-era IsPlainLvalueShape guard here was removed with
//the side channel. Array bases keep the plain element-type
//propagation in the caller, peeling the token.
//True = consumed (resolved as the container's element type).
bool ExprResolveAccessor::TryResolveContainerSubscript(SnSubscriptExpr &sn,
	SnField *pBaseType)
{
	if (!pBaseType || pBaseType->Kind() != NK_ClassDecl)
		return false;
	auto* pClass = static_cast<SnClassDecl*>(pBaseType);
	if (!pClass->IsGenericInstantiation())
		return false;
	const auto& baseName = pClass->BaseName();
	const auto& typeArgs = pClass->GenericTypeArgs();
	SnField* elem = nullptr;
	if (baseName == kBuiltinListTypeName && !typeArgs.empty())
		elem = typeArgs[0];
	else if (baseName == kBuiltinDictTypeName && typeArgs.size() > 1)
	{
		elem = typeArgs[1];
		//0.7.5: the read sugar `d[k]` lowers to get(k), whose boxing
		//plan stages the key with K's tag — wrap the key against K like
		//the store sugar (ApplyContainerStoreCasts) and the method form
		//so the slot holds K's width before the box read. List indexes
		//stay unchecked (a plain int position, runtime-bounds-checked).
		if (sn.Index() && sn.Index()->IsResolved()
			&& sn.Index()->EvalDataType())
		{
			auto keyCast = GetCastInfo(
				sn.Index()->EvalDataType(), typeArgs[0]);
			auto iKey = sn.Children().find(sn.Index());
			if (iKey != sn.Children().end()
				&& FixupExprType(iKey, keyCast))
				sn.m_pIndices[0] = static_cast<SnExpression*>(&*iKey);
		}
	}
	if (!elem)
		return false;
	sn.EvalDataType(elem);
	sn.AddFlags(NF_Resolved);
	BindArrayTypeToken(sn);
	return true;
}

//0.9.0: consume the subscript as a sized array creation (`int[n]` /
//`Foo[n]`, the new-free twin of `new T[n]`) when the base resolved as a
//TYPE (primitive keyword, or a class/struct/enum/interface declaration —
//directly, or through an alias spliced in by ResolveArrayBase). A local
//shadowing the type name binds the VALUE first (FindField order),
//keeping the plain subscript reading. True = consumed. The index child
//doubles as the size expression; the element type is stored first, then
//BindArrayTypeToken mints the array token over it — the same tail as
//Access(SnNewArrayExpr&). An array alias base (`using Ints = int[];`
//then `Ints[2]`) is rejected as a creation head.
//Stage-2: resolve every creation dimension; named/out forms and
//unresolved dimensions consume the expression (true, diagnostics fired).
bool ExprResolveAccessor::ResolveCreationDims(SnSubscriptExpr &sn)
{
	//Rank cap mirrors the grammar's production-point cap: the index
	//list IS the dims list, so the cap is validated where dims are.
	if (sn.Indices().size() > kMaxArrayRank) {
		m_Env.Log(CLL_Error, sn.Location(),
			"array rank %d exceeds the maximum of %d.",
			static_cast<int>(sn.Indices().size()), kMaxArrayRank);
		return false;  //consumed: the cap diagnostic already fired
	}
	for (auto *pIndex : sn.Indices())
	{
		if (pIndex->Kind() != NK_IdentifierExpr
			&& pIndex->Kind() != NK_NameExpr
			&& !pIndex->IsDataExpr())
		{
			m_Env.Log(CLL_Error, pIndex->Location(),
				"array creation dimensions must be plain expressions.");
			return false;
		}
		pIndex->Accept(*m_pVisitor);
		if (!pIndex->IsResolved())
			return false;  //consumed: the dimension diagnostic already fired
	}
	return true;
}

bool ExprResolveAccessor::TrySizedArrayCreation(SnSubscriptExpr &sn)
{
	auto &arrayExpr = *sn.Array();
	if (!arrayExpr.IsResolved())
		return false;
	//Only a TYPE EXPRESSION can head a creation: the bare identifier and
	//the splice products of an alias base (name / array / generic — the
	//complete CloneTypeNode product set; the generic form binds the minted
	//instantiation through Access(SnGenericTypeExpr&)). A value expression whose Field() happens
	//to be type-ish (`ks.get(0)[1]` binds the element token) must keep
	//the plain subscript reading.
	switch (arrayExpr.Kind())
	{
	case NK_IdentifierExpr:
	case NK_NameExpr:
	case NK_ArrayTypeExpr:
	case NK_GenericTypeExpr:
		break;
	default:
		return false;
	}
	auto *pBaseFieldExpr = static_cast<SnFieldExpr *>(&arrayExpr);
	auto *pElemField = pBaseFieldExpr->Field();
	if (!pElemField || !pElemField->IsTypeField())
		return false;
	if (pBaseFieldExpr->IsArrayType())
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"cannot create an array of an array type; declare the element "
			"type once with its rank.");
		return true;
	}
	//Stage-2: the creation twin T[2, 3] carries N dimensions — the index
	//list IS the rank. Named/out forms ride ConcreteParamList and are
	//rejected here.
	if (!ResolveCreationDims(sn))
		return true;
	sn.NewArrayElem(pElemField);
	//Stage-2: the minted token carries the rank — the index count. The
	//BindArrayTypeToken tail interns rank-1 only, so the creation path
	//interns directly with its own rank.
	const uint8_t rank = static_cast<uint8_t>(sn.Indices().size());
	sn.EvalDataType(m_Env.InternArrayTypeToken(pElemField, rank));
	sn.AddFlags(NF_Resolved);
	return true;
}

void ExprResolveAccessor::Access(SnSubscriptExpr &sn)
{
	assert(!sn.IsResolved());

	//Resolve array expression (the 0.9.0 alias splice included).
	if (!ResolveArrayBase(sn))
		return;

	//0.9.0: `Type[size]` creation twin — see TrySizedArrayCreation.
	if (TrySizedArrayCreation(sn))
		return;

	//Resolve every index (rank-1 subscripts hold exactly one).
	for (auto *pIndex : sn.Indices())
	{
		pIndex->Accept(*m_pVisitor);
		if (!pIndex->IsResolved())
			return;
	}

	//Look up arr.length-style access is handled by MemberExpr.
	//For now, the result type of subscript is the element type.
	auto* arrayType = sn.Array()->EvalDataType();
	if (ResolveStringSubscript(sn, arrayType))
		return;
	if (TryResolveContainerSubscript(sn, arrayType))
		return;
	if (arrayType && arrayType->Kind() == NK_ArrayTypeToken)
	{
		//Stage-2: the index count must equal the token rank — the named
		//arity diagnostic replaces the old single-index assumption.
		const auto rank = static_cast<SnArrayTypeToken*>(arrayType)->Rank();
		if (sn.Indices().size() != rank)
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"array subscript expects %u indices, got %u.",
				static_cast<unsigned>(rank),
				static_cast<unsigned>(sn.Indices().size()));
			return;
		}
		//0.7.3 B token path: an
		//array-valued base carries the interned array token — the
		//subscript's own type is its ELEMENT.
		arrayType = static_cast<SnArrayTypeToken*>(
			arrayType)->ElemTypeOf();
		sn.EvalDataType(arrayType);
	}
	sn.AddFlags(NF_Resolved);
	BindArrayTypeToken(sn);
}

} //namespace nlang
