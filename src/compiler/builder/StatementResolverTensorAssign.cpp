/*---
    StatementResolverTensorAssign.cpp — v0.14.2: the fancy-index
    assignment lowering (a[indices] = values on numeric arrays).
    Split from StatementResolverAssign.cpp (the source-size guard):
    the lowering is one self-contained accessor method — the
    statement-node twin of the binary lowering in
    ExprResolverArrayOps.cpp (children release, the invoke adopts,
    the statement holds the body; the failure path unwinds through
    re-adoption, never a leak).
---*/
#include "StatementResolver.h"
#include "SnArrayTypeToken.h"
#include "SyntaxTree.h"
#include "ModuleRegistry.h"
#include <nlang/runtime/NodeContainers.h>
#include <nlang/runtime/BuiltinGenericNames.h>

namespace nlang
{

//The element row a subscript-store writes into: either spelling of
//the base's array type (the interned token or the Array<E>
//instantiation — the Array<E> arm feeds the bare-brace store's
//inferred target too), or the container's value slot for List/Dict.
static SnField* SubscriptStoreElemType(SnExpression *pBase)
{
	if (!pBase)
		return nullptr;
	auto* pBaseType = pBase->EvalDataType();
	if (!pBaseType)
		return nullptr;
	if (SnField* pElemRow = ArrayElementRowOf(pBaseType))
		return pElemRow;
	if (pBaseType->Kind() == NK_ClassDecl)
	{
		auto* pGen = static_cast<SnClassDecl*>(pBaseType);
		auto typeArgs = GetGenericTypeArgs(pGen);
		if (pGen->BaseName() == kBuiltinDictTypeName
			&& typeArgs.size() > 1)
			return typeArgs[1];
		if (pGen->BaseName() == kBuiltinListTypeName
			&& !typeArgs.empty())
			return typeArgs[0];
	}
	return nullptr;
}

//The operand-resolve prologue of the subscript-store statement: the
//base and the indices, then the value (a bare brace stored value
//takes the base's element type as its target — the 0.9.0 rule; the
//base is resolved by then, so the element type is derivable). True =
//a sub-resolve logged a diagnostic (the caller stops).
bool StatementResolveAccessor::ResolveSubscriptAssignOperands(
	SnSubscriptAssignStmt &sn)
{
	if (sn.Array() && !sn.Array()->IsResolved())
		m_ExprResolver.Resolve(*sn.Array(), *sn.Array()->Parent(), *m_pCurrType, ERF_None);
	if (ResolveStoreIndices(sn))
		return true;
	if (auto *pInitList = sn.Value()
		? UntypedInitListOf(*sn.Value()) : nullptr)
		pInitList->InferredTarget(SubscriptStoreElemType(sn.Array()));
	if (sn.Value() && !sn.Value()->IsResolved())
		m_ExprResolver.Resolve(*sn.Value(), *sn.Value()->Parent(), *m_pCurrType, ERF_None);
	return false;
}

//The fancy-index assignment lowering. Shape gate: a single array-
//valued int index on a numeric array base (the rank-1 fancy form; the
//rank gate rides the native's named check). The three operands detach
//into a resolved tensor.scatterInto invoke (the binary-lowering
//pattern: the children release, the invoke adopts, the statement
//holds the body). True = consumed.
//The shape gate: a single array-valued int index on a numeric array
//base (the rank-1 fancy form; the rank gate rides the native's named
//check). False = not a fancy-assignment shape (the plain store paths
//carry the statement).
bool FancyAssignShape(SnSubscriptAssignStmt &sn)
{
	if (sn.Indices().size() != 1)
		return false;
	auto *pIdxRow = ArrayElementRowOf(sn.Index()->EvalDataType());
	if (!pIdxRow || pIdxRow->Kind() != NK_Int32)
		return false;
	if (!sn.Array()->IsArrayValued())
		return false;
	auto *pBaseRow = ArrayElementRowOf(sn.Array()->EvalDataType());
	if (!pBaseRow)
		return false;
	const NodeKind baseKind = pBaseRow->Kind();
	return baseKind == NK_Int32 || baseKind == NK_Long
		|| baseKind == NK_Float || baseKind == NK_Double;
}

bool StatementResolveAccessor::TryTensorScatterInto(
	SnSubscriptAssignStmt &sn)
{
	if (!FancyAssignShape(sn))
		return false;
	//The values-arm spelling: an array-valued value takes the array
	//form, a scalar the scalar form (the compile-time shape selects
	//the overload family — getArrayView raises on a scalar, so the
	//native side cannot probe).
	auto *pValRow = sn.Value()
		? ArrayElementRowOf(sn.Value()->EvalDataType()) : nullptr;
	const char *fnName = pValRow ? "scatterInto" : "scatterIntoS";
	auto candidates = m_Env.Registry().ModuleFunctions("tensor", fnName);
	if (candidates.empty())
		return false;   //no tensor import: the plain store path's
		                //type diagnostic is the honest failure.
	//Detach the three children (ownership moves into the invoke's
	//params — the binary-lowering pattern; the raw members stay valid
	//pointers into the plan-owned nodes for ToString/unwind).
	UniquePtrList<SnExpression> params;
	params.Add(sn.Array());
	params.Add(sn.Index());
	params.Add(sn.Value());
	{
		auto &children = sn.Children();
		while (children.begin() != children.end())
		{
			auto iChild = children.begin();
			sn.RemoveChild(iChild);
		}
	}
	auto pPlan = new SnInvokeExpr(
		new std::string(fnName), std::move(params), *sn.Location());
	if (!m_ExprResolver.Resolve(*pPlan, sn, *m_pCurrType, ERF_None))
	{
		//The invoke's diagnostic is logged; unwind the operands back
		//to the statement (the binary's UnwindTensorCallPlan shape:
		//erase releases the params without destroying, the statement
		//re-adopts them into its child list).
		while (pPlan->Params().begin() != pPlan->Params().end())
			pPlan->Params().erase(pPlan->Params().begin());
		delete pPlan;
		sn.AddChild(sn.Array());
		sn.AddChild(sn.Index());
		sn.AddChild(sn.Value());
		return false;
	}
	sn.TensorBody(std::unique_ptr<SnInvokeExpr>(pPlan));
	return true;
}

} //namespace nlang
