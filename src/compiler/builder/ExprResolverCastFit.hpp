/*-----------------------------------------------------------------------------
	ncomp/builder/ExprResolverCastFit.hpp
	Constant-fit and lossy-conversion predicates of the 0.7.5 cast rules
(spec §2.2), shared by RejectIncompatibleCast (the fit gate that can
promote an explicit narrowing of an in-range constant to implicit) and
FixupExprType (the precision-loss warning). Pure functions over the
expression and the registry — no resolver state.
-----------------------------------------------------------------------------*/
#pragma once
#include <nlang/runtime/PrimitiveTypes.h>

namespace nlang
{

class SnExpression;

//Verdict of TryConstantFit for one (constant, target-row) pair.
enum ConstantFitVerdict
{
	CF_NotApplicable,  // not a constant, or source domain cannot fit this target
	CF_Fits,           // in-range: the caller may treat the cast as implicit
	CF_OutOfRange      // domain-matching constant outside the target's range
};

struct ConstantFitOutcome
{
	ConstantFitVerdict verdict = CF_NotApplicable;
	char constantText[32] = {0};  //rendered constant (OutOfRange diagnostic)
};

//Java/C# constant-fit: a constant whose value fits the narrowing target
//converts implicitly. Only literals and negated literals count (negative
//ints carry the sign inside the token; negated ulong is not a candidate);
//folded expressions stay out of scope for v1. char/bool targets are
//excluded via the numeric-domain gate — spec keeps char↔numeric
//explicit-only (`char c = 97` is an error; write 'a').
ConstantFitOutcome TryConstantFit(SnExpression& srcExpr,
	const ScalarPrimInfo& tgtRow);

//Lossy implicit pairs: int/uint/long/ulong → float; long/ulong →
//double. 32-bit integers → double are exact (52-bit mantissa covers
//every int32/uint32 value), float → double is exact.
bool IsLossyImplicitPair(NodeKind srcKind, NodeKind dstKind);

//Exemption for the lossy warning: a constant operand whose value
//survives the conversion exactly (float f = 5 is silent; float f =
//16777217 warns). (int64)f for an out-of-int64-range float saturates to
//INT64_MIN on the target — deterministic, and the != verdict correctly
//reports lossy.
bool ConstantExactlyRepresentable(SnExpression* e, NodeKind dstKind);

} //namespace nlang
