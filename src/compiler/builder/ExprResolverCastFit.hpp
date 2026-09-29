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

//A constant literal (or its negation) flattened to a value — shared by
//the constant-fit gate below and the switch/enum label consumers
//(numeric literals are unsigned since the 0.7.5 sign retirement, so
//every negative constant arrives as OP_Neg over a literal).
struct ConstLiteralValue
{
    NodeKind kind;   // NK_Int32 / NK_Long / NK_ULong / NK_Double / NK_Float
    int64    i;      // valid for NK_Int32 / NK_Long
    uint64   u;      // valid for NK_ULong
    double   d;      // valid for NK_Double / NK_Float
};

//True when e is a literal or OP_Neg over a literal; fills v with the
//kind and value. Negated ulong is not a candidate, except the 2^63
//special case which folds to long INT64_MIN (keeping
//`-9223372036854775808` legal — the lexer produces it unsigned-only).
//The NK_Float value rides v.d at float precision (exact widening).
bool TryGetConstantLiteral(SnExpression* e, ConstLiteralValue& v);

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
//converts implicitly. Only literals and negated literals count (signs
//are unary operators since 0.7.5, so every negative constant is the
//OP_Neg form; negated ulong is not a candidate); folded expressions
//stay out of scope for v1. char/bool targets are excluded via the
//numeric-domain gate — spec keeps char↔numeric explicit-only
//(`char c = 97` is an error; write 'a').
ConstantFitOutcome TryConstantFit(SnExpression& srcExpr,
	const ScalarPrimInfo& tgtRow);

//Range predicate shared by TryConstantFit and the switch-label range
//gate (enum member labels carry a bare int32 value, not an expression
//TryConstantFit can walk). Signed rows span [-2^(8w-1), 2^(8w-1)-1],
//unsigned rows [0, 2^8w-1], w = slotWidth from the registry row.
bool IntFitsRow(int64 v, const ScalarPrimInfo& p);

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
