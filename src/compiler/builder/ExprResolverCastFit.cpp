/*---
    ExprResolverCastFit.cpp — 常量适配与精度损失判定（0.7.5 cast 规则，spec §2.2）
    从 ExprResolverCast.cpp 抽取（2026-09-29 可维护性重构，零行为变化）：
    纯函数，无 resolver 状态，供 RejectIncompatibleCast 的常量适配门
    与 FixupExprType 的精度损失警告共享。
---*/
#include "ExprResolverCastFit.hpp"
#include <nlang/compiler/SnExpressions.h>
#include <nlang/runtime/NodeConsts.h>
#include <nlang/runtime/RnTypes.h>
#include <nlang/runtime/Variant.h>
#include <cmath>
#include <cstdio>

namespace nlang
{

//Constant-fit special case (Java/C#, spec §2.2): a CONSTANT whose
//value fits the narrowing target converts implicitly. Only literals
//and negated literals count (negative ints carry the sign inside the
//token — DecInt keeps its sign prefix; the OP_Neg form mainly serves
//negative double literals once Task 7 retires the lexer sign, and
//rejects negated ulong); folded expressions stay out of scope for v1.
struct ConstLiteralValue
{
	NodeKind kind;   // NK_Int32 / NK_Long / NK_ULong / NK_Double / NK_Float
	int64    i;      // valid for NK_Int32 / NK_Long
	uint64   u;      // valid for NK_ULong
	double   d;      // valid for NK_Double / NK_Float
};

static bool TryGetConstantLiteral(SnExpression* e, ConstLiteralValue& v)
{
	if (e->Kind() == NK_LiteralExpr)
	{
		Variant& val = static_cast<SnLiteralExpr*>(e)->Value();
		v.kind = val.Type()->Kind();
		//Kind-exact member read: an int32 literal writes only the
		//4-byte m_Int member; Get<int64> would read the 8-byte m_Long
		//member and pick up garbage in the upper bytes.
		if (v.kind == NK_ULong)                 v.u = val.Get<uint64>();
		else if (v.kind == NK_Double)           v.d = val.Get<double>();
		else if (v.kind == NK_Long)             v.i = val.Get<int64>();
		else if (v.kind == NK_Int32)            v.i = val.Get<int32>();
		return true;    // NK_Float: kind set, value unread (domain gates reject)
	}
	if (e->Kind() == NK_BinaryExpr)
	{
		auto* pBin = static_cast<SnBinaryExpr*>(e);
		if (pBin->Op() == SnBinaryExpr::OP_Neg && !pBin->Right() &&
			pBin->Left()->Kind() == NK_LiteralExpr)
		{
			TryGetConstantLiteral(pBin->Left(), v);
			if (v.kind == NK_ULong) return false;  // negated ulong: not a candidate
			if (v.kind == NK_Int32 || v.kind == NK_Long) v.i = -v.i;
			else if (v.kind == NK_Double)               v.d = -v.d;
			return true;
		}
	}
	return false;
}

//Domain gate: int-like sources reach integer targets only, ulong only
//unsigned targets (tiering puts <= INT64_MAX into long, so a ulong
//literal never fits a signed target), double only the float target —
//integer targets never accept float constants (int x = 2e5 is an
//error).
static bool ConstantFitDomain(NodeKind srcKind, const ScalarPrimInfo& tgt)
{
	if (!PrimCategoryIsNumeric(tgt.category)) return false;
	if (srcKind == NK_Int32 || srcKind == NK_Long || srcKind == NK_ULong)
		return tgt.category != PC_Float;
	return srcKind == NK_Double && tgt.kind == NK_Float;
}

//Registry-driven range check: signed targets span [-2^(8w-1), 2^(8w-1)-1],
//unsigned targets [0, 2^(8w)-1], w = slotWidth from the registry row.
//bits==64 short-circuits the shifts (1<<63 on int64 is not portable).
static bool IntFitsRow(int64 v, const ScalarPrimInfo& p)
{
	int bits = p.slotWidth * 8;
	if (p.category == PC_UInt)
		return v >= 0 && (bits == 64 || (uint64)v <= ((uint64(1) << bits) - 1));
	return bits == 64 ||
		(v >= -(int64(1) << (bits - 1)) && v < (int64(1) << (bits - 1)));
}
static bool UIntFitsRow(uint64 v, const ScalarPrimInfo& p)
{
	return p.category == PC_UInt &&
		(p.slotWidth == 8 || v <= ((uint64(1) << (p.slotWidth * 8)) - 1));
}
static bool DoubleFitsFloat(double v)
{
	float f = static_cast<float>(v);
	return std::isfinite(f) && static_cast<double>(f) == v;
}

ConstantFitOutcome TryConstantFit(SnExpression& srcExpr,
	const ScalarPrimInfo& tgtRow)
{
	ConstantFitOutcome out;
	ConstLiteralValue v;
	if (!TryGetConstantLiteral(&srcExpr, v))
		return out;
	if (!ConstantFitDomain(v.kind, tgtRow))
		return out;
	const bool fits = (v.kind == NK_ULong) ? UIntFitsRow(v.u, tgtRow)
		: (v.kind == NK_Double) ? DoubleFitsFloat(v.d)
		: IntFitsRow(v.i, tgtRow);
	if (fits)
	{
		out.verdict = CF_Fits;
		return out;
	}
	out.verdict = CF_OutOfRange;
	if (v.kind == NK_ULong)
		std::snprintf(out.constantText, sizeof(out.constantText), "%llu",
			(unsigned long long)v.u);
	else if (v.kind == NK_Double)
		std::snprintf(out.constantText, sizeof(out.constantText), "%g", v.d);
	else
		std::snprintf(out.constantText, sizeof(out.constantText), "%lld",
			(long long)v.i);
	return out;
}

//Lossy implicit pairs (spec §2.2): int/uint/long/ulong → float;
//long/ulong → double. 32-bit integers → double are exact (52-bit
//mantissa covers every int32/uint32 value), float → double is exact.
bool IsLossyImplicitPair(NodeKind s, NodeKind d)
{
	int si = ScalarPrimIndexOf(s), di = ScalarPrimIndexOf(d);
	if (si < 0 || di < 0) return false;
	const ScalarPrimInfo& src = kScalarPrims[si];
	const ScalarPrimInfo& dst = kScalarPrims[di];
	if (dst.category != PC_Float) return false;
	if (src.category != PC_SInt && src.category != PC_UInt) return false;
	//int/uint/long/ulong → float; long/ulong → double. The
	//src.slotWidth>=4 guard excludes byte/ubyte/short/ushort → float —
	//their value ranges are always exactly representable.
	return (dst.slotWidth == 4 && src.slotWidth >= 4)
		|| (dst.slotWidth == 8 && src.slotWidth == 8);
}

//Exemption: a constant operand whose value survives the conversion
//exactly (float f = 5 is silent; float f = 16777217 warns). (int64)f
//for an out-of-int64-range float saturates to INT64_MIN on the target
//— deterministic, and the != verdict correctly reports lossy.
bool ConstantExactlyRepresentable(SnExpression* e, NodeKind d)
{
	ConstLiteralValue v;
	if (!TryGetConstantLiteral(e, v)) return false;
	if (v.kind == NK_Int32 || v.kind == NK_Long)
	{
		if (d == NK_Float)  { float  f = (float)v.i;   return (int64)f == v.i; }
		if (d == NK_Double) { double x = (double)v.i;  return (int64)x == v.i; }
	}
	if (v.kind == NK_ULong)
	{
		if (d == NK_Float)  { float  f = (float)v.u;   return (uint64)f == v.u; }
		if (d == NK_Double) { double x = (double)v.u;  return (uint64)x == v.u; }
	}
	return false;
}

} //namespace nlang
