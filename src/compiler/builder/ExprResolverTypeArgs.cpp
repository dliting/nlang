/*---
    ExprResolverTypeArgs.cpp — 泛型类型实参的形状门（ArrayTypeDepth、
    RejectInvalidTypeArg）。从 ExprResolverTypes.cpp 拆出（2026-10-27
    尺寸守卫触发的拆分，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"

namespace nlang
{

//Depth of the array-type chain (int[] = 1, int[][] = 2, …). Depth >= 2
//is a jagged declaration form: the VM has no multi-dimensional layout
//(pre-token, the element masquerade additionally degraded such
//declarations twice silently), so it is rejected at the declaration
//site (array redesign B, spec §5.5).
int ArrayTypeDepth(const SnFieldExpr* pType)
{
	int depth = 0;
	for (const auto* pCur = pType; pCur
		&& pCur->Kind() == NK_ArrayTypeExpr; ++depth)
		pCur = static_cast<const SnArrayTypeExpr*>(pCur)->ElementType();
	return depth;
}

//Per-argument reject gate of the generic type-argument loop:
//void and out are func-only features (void only in the first / return
//slot, out only on parameter slots), and a jagged argument has no VM
//layout (the same gate as declaration sites, spec §5.5). True =
//rejected with the named diagnostic.
bool RejectInvalidTypeArg(BuildEnvironment &env, SnFieldExpr *pTA,
	const std::string &baseName, bool bFirstArg)
{
	//Phase 13: void and out are func-only type-argument features.
	//void may only occupy func's first (return) type slot; out may
	//only mark func parameter slots (any position after the first).
	bool isVoidArg = pTA->Field()->Kind() == NK_Void;
	bool isOutArg = pTA->ContainFlags(NF_Out);
	if (isVoidArg && !(baseName == kBuiltinFuncTypeName && bFirstArg))
	{
		env.Log(CLL_Error, pTA->Location(),
			"void is only allowed as the return slot of func<...>.");
		return true;
	}
	if (isOutArg && !(baseName == kBuiltinFuncTypeName && !bFirstArg))
	{
		env.Log(CLL_Error, pTA->Location(),
			"out is only allowed on func<...> parameters.");
		return true;
	}
	//Array redesign B: a jagged type argument has no VM layout — the
	//same gate as declaration sites (spec §5.5), enforced here because
	//generic type-arg position is a distinct declaration form.
	if (ArrayTypeDepth(pTA) >= 2)
	{
		env.Log(CLL_Error, pTA->Location(),
			"jagged arrays (T[][]) are not supported.");
		return true;
	}
	return false;
}

} //namespace nlang
