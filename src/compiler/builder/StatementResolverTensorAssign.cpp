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

//The slice-assign classification gate: at least one SLICE item in the
//index list, the base a numeric-kind tensor array (FancyAssignShape's
//slice form). The container sugar and the plain element stores fall
//through to their own paths.
static bool SliceAssignShape(SnSubscriptAssignStmt &sn)
{
	bool hasSlice = false;
	for (auto *pIdx : sn.Indices())
	{
		if (pIdx && pIdx->Kind() == NK_SliceItemExpr)
			hasSlice = true;
	}
	if (!hasSlice)
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

//The read half of the slice-assign lowering: detach the operands,
//resolve `base[items]` as a plain read (yielding the slice VIEW —
//the read path's normalization, gates and residency are the single
//source of the view's shape, strides and writability), then detach
//the view and hand it over. False = the read's diagnostic fired (the
//operands rolled back; the caller consumes the failure).
bool StatementResolveAccessor::ResolveSliceAssignView(
	SnSubscriptAssignStmt &sn, SnExpression *&pView)
{
	//The values-arm spelling: an array-valued value takes the array
	//form, a scalar the scalar form (the compile-time shape selects
	//the overload family — the same gate the fancy assignment uses).
	auto *pValRow = sn.Value()
		? ArrayElementRowOf(sn.Value()->EvalDataType()) : nullptr;
	const char *fnName = pValRow ? "assignInto" : "assignIntoS";
	if (m_Env.Registry().ModuleFunctions("tensor", fnName).empty())
		return false;   //no tensor import: the plain store path's
		                //type diagnostic is the honest failure.
	//The operands detach FIRST (AddChild asserts a null parent — the
	//subscript's construction below adopts the base and the indices,
	//so they must leave the statement's child list before it exists).
	while (sn.Children().begin() != sn.Children().end())
		sn.RemoveChild(sn.Children().begin());
	UniquePtrList<SnExpression> subIndices;
	for (auto *pIdx : sn.Indices())
		subIndices.Add(pIdx);
	auto pSub = new SnSubscriptExpr(sn.Array(), std::move(subIndices),
		*sn.Location());
	sn.AddChild(pSub);
	if (!m_ExprResolver.Resolve(*pSub, sn, *m_pCurrType, ERF_None))
	{
		//The read's diagnostic is logged; the subscript's children roll
		//back into the statement and the node retires. The friendship of
		//SnSubscriptExpr gates the Node-level child transfer.
		while (pSub->Children().begin() != pSub->Children().end())
		{
			Node *pChild = &*pSub->Children().begin();
			pSub->RemoveChild(pSub->Children().begin());
			sn.AddChild(pChild);
		}
		sn.RemoveChild(pSub);
		delete pSub;
		return false;
	}
	sn.RemoveChild(pSub);
	pView = pSub;
	return true;
}

//The slice-assign lowering body: the read half yields the slice view,
//the write half binds tensor.assignInto(view, value). True = the
//statement consumed (the tensor body bound); false = a diagnostic
//fired and the operands rolled back into the statement.
bool StatementResolveAccessor::LowerTensorSliceAssign(
	SnSubscriptAssignStmt &sn)
{
	//The values-arm spelling selects the overload family (the same
	//gate the fancy assignment uses) — checked before any detachment.
	auto *pValRow = sn.Value()
		? ArrayElementRowOf(sn.Value()->EvalDataType()) : nullptr;
	if (m_Env.Registry().ModuleFunctions("tensor",
		pValRow ? "assignInto" : "assignIntoS").empty())
		return false;   //no tensor import: the plain store path's
		                //type diagnostic is the honest failure.
	SnExpression *pView = nullptr;
	if (!ResolveSliceAssignView(sn, pView))
	{
		//The read's diagnostic IS the statement's failure — the plain
		//store paths must not run on the rolled-back fragments (the
		//compile stops at the logged diagnostics; codegen is
		//unreachable).
		return true;
	}
	//The write half: tensor.assignInto(<subscript read>, value) — the
	//view detaches into the invoke's params (the ownership moves; the
	//raw pointer stays valid for ToString/unwind).
	SnExpression *pValue = sn.Value();
	auto *pValRow2 = pValue
		? ArrayElementRowOf(pValue->EvalDataType()) : nullptr;
	UniquePtrList<SnExpression> params;
	params.Add(pView);
	params.Add(pValue);
	auto pPlan = new SnInvokeExpr(
		new std::string(pValRow2 ? "assignInto" : "assignIntoS"),
		std::move(params), *sn.Location());
	if (!m_ExprResolver.Resolve(*pPlan, sn, *m_pCurrType, ERF_None))
	{
		//The invoke's diagnostic is logged; the operands roll back two
		//levels into the statement (the view first, so its children keep
		//their order under the statement's). The params release through
		//the public list API (the FancyAssign rollback shape).
		while (pPlan->Params().begin() != pPlan->Params().end())
			pPlan->Params().erase(pPlan->Params().begin());
		delete pPlan;
		sn.AddChild(pView);
		return true;
	}
	sn.TensorBody(std::unique_ptr<SnInvokeExpr>(pPlan));
	return true;
}

bool StatementResolveAccessor::TryTensorSliceAssign(
	SnSubscriptAssignStmt &sn)
{
	if (!SliceAssignShape(sn))
		return false;
	//The values-arm spelling selects the overload family (the same
	//gate the fancy assignment uses) — checked before any detachment.
	auto *pValRow = sn.Value()
		? ArrayElementRowOf(sn.Value()->EvalDataType()) : nullptr;
	const char *fnName = pValRow ? "assignInto" : "assignIntoS";
	if (m_Env.Registry().ModuleFunctions("tensor", fnName).empty())
	{
		//No tensor import: the plain store paths have no slice-item arm
		//— the honest rejection names the statement (the classification
		//consumed it; the plain paths would reach codegen with the
		//slice-item nodes).
		m_Env.Log(CLL_Error, sn.Location(),
			"slice assignment requires the tensor package.");
		return true;
	}
	if (!LowerTensorSliceAssign(sn))
	{
		//The lowering's diagnostic IS the statement's failure — the
		//plain store paths must not run on the rolled-back fragments
		//(the compile stops at the logged diagnostics; codegen is
		//unreachable).
		return true;
	}
	return true;
}

} //namespace nlang
