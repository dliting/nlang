/*---
    ExprResolverBinding.cpp — 重载候选的绑定距离核算
    从 ExprResolverCast.cpp 抽取（2026-10-12 source-size 拆分，零行为变化）。
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

//Phase 13: distance contribution of a pending function-reference binding
//(bare identifier or receiver-bound member form): 0 when the formal is a
//func type whose signature matches the referenced function exactly, -1
//otherwise. The exact-match-only rule also closes the legacy hole where
//the bare name's RETURN type let it bind approximately to non-func
//formals.
static int FuncRefBindingDistance(const FormalBinding &b)
{
	SnFunction *pRef = nullptr;
	if (IsUnboundFuncRef(*b.pCallerExpr))
		pRef = static_cast<SnFunction*>(
			static_cast<SnIdentifierExpr*>(b.pCallerExpr)->Field());
	else if (IsUnboundMemberFuncRef(*b.pCallerExpr))
		pRef = static_cast<SnFunction*>(
			static_cast<SnIdentifierExpr*>(
				static_cast<SnMemberExpr*>(
					b.pCallerExpr)->Inner())->Field());
	auto *pTgt = b.pFormal->EvalDataType();
	if (!IsFuncTypeDecl(pTgt) || !pRef
		|| !FuncRefMatchesDecl(*pRef, static_cast<SnClassDecl*>(pTgt)))
		return -1;
	return 0;
}

//Null-literal policy against a formal type (the null literal is
//Int32-typed): 0 = no null special case, adjudicate via CalcTypeDistance;
//1 = accept (null binds an array formal as the raw sentinel 0, same bridge
//the assignment grants); -1 = reject (null against a string formal would
//emit OP_Int32_to_str and print "0"). Class/interface formals fall through
//to the distance calculation.
static int NullBindingVerdict(SnExpression &caller, const SnField &formal)
{
	if (!caller.ContainFlags(NF_NullLiteral))
		return 0;
	const NodeKind k = formal.Kind();
	if (k == NK_ArrayTypeToken)
		return 1;
	if (k == NK_String)
		return -1;
	return 0;
}

//0.9.0: the formal shapes a bare brace argument can infer against —
//the init-list target shapes (IsInitListTargetShape, the single
//authority in ExprResolverInitList.cpp); the formal must carry a type.
static bool AdmitsInitListArg(const SnFormalParam &formal)
{
	auto *pType = formal.EvalDataType();
	return pType && IsInitListTargetShape(pType);
}

//Phase 9c: sum of CalcTypeDistance over the bound (positional / named)
//entries. B_Default contributes 0. Returns -1 if any bound entry has
//incompatible types.
int ExprResolveAccessor::ComputeBindingDistance(
	const std::vector<FormalBinding> &bindings) const
{
	int nDistance = 0;
	for (auto &b : bindings)
	{
		if (b.kind == FormalBinding::B_Default)
			continue;
		assert(b.pCallerExpr && b.pFormal);
		if (IsUnboundFuncRef(*b.pCallerExpr)
			|| IsUnboundMemberFuncRef(*b.pCallerExpr))
		{
			if (FuncRefBindingDistance(b) < 0)
				return -1;
			continue;
		}
		//0.9.0 (D11): a deferred brace argument only admits an
		//init-compatible formal — anything else rejects the candidate.
		if (UntypedInitListOf(*b.pCallerExpr))
		{
			if (!AdmitsInitListArg(*b.pFormal))
				return -1;
			continue;
		}
		auto *pSrc = b.pCallerExpr->EvalDataType();
		auto *pTgt = b.pFormal->EvalDataType();
		if (!pSrc || !pTgt)
			return -1;
		//Null-literal policy (array accept / string reject) precedes the
		//generic distance; see NullBindingVerdict.
		const int nNull = NullBindingVerdict(*b.pCallerExpr, *pTgt);
		if (nNull > 0)
			continue;
		if (nNull < 0)
			return -1;
		int n = CalcTypeDistance(*pSrc, *pTgt);
		if (n < 0)
			return -1;
		//Trial scoring stays pure (TryBindArgs' no-logging contract):
		//rejecting a non-matching overload is expected among same-name
		//array overloads; the final verdict's notes name the conversion.
		//Phase 9e: out bindings require the exact same type — the callee
		//writes its slot straight back into the caller's variable; any
		//implicit cast (int→float etc.) would be discarded by writeback.
		if (b.bIsOut && n != 0)
			return -1;
		nDistance += n;
	}
	return nDistance;
}

} //namespace nlang
