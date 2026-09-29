/*---
    ExprResolverBinary.cpp — 二元/逻辑表达式解析（对称提升、比较门、字符串拼接）
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
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Compare-family membership test for Access(SnBinaryExpr) — the six
//relational/equality operators that flow through the compare gates.
static bool IsCompareOp(SnBinaryExpr::Operator op)
{
	return op == SnBinaryExpr::OP_Less
		|| op == SnBinaryExpr::OP_LessEqual
		|| op == SnBinaryExpr::OP_Greater
		|| op == SnBinaryExpr::OP_GreaterEqual
		|| op == SnBinaryExpr::OP_Equal
		|| op == SnBinaryExpr::OP_NotEqual;
}

//Mixed string/non-string has no semantics ("a" < 5), in either operand
//order. Null literals are exempt: KT_Null is Int32-typed, and
//`s == null` / `c == null` are the established null checks — the null
//side keeps its raw sentinel bits and takes the identity/sentinel
//comparison path. True = rejected (diagnostic logged).
bool ExprResolveAccessor::RejectStringNonStringMix(SnBinaryExpr &sn,
	NodeKind lk, NodeKind rk, bool lNull, bool rNull)
{
	if ((lk == NK_String) != (rk == NK_String) && !lNull && !rNull)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"cannot compare string with a non-string operand "
			"(only null is allowed as the other side).");
		return true;
	}
	return false;
}

//Phase 13: function handles support content equality only (==/!=), and
//only against another function handle or null. Without this gate
//codegen's dispatch would silently fall to the integer variant
//comparing raw heap indices. True = rejected (diagnostic logged).
bool ExprResolveAccessor::RejectFuncHandleMisuse(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op, SnField *L, SnField *R,
	bool lNull, bool rNull)
{
	bool lFunc = IsFuncTypeDecl(L);
	bool rFunc = IsFuncTypeDecl(R);
	if (!lFunc && !rFunc)
		return false;
	bool bEq = op == SnBinaryExpr::OP_Equal
		|| op == SnBinaryExpr::OP_NotEqual;
	if (!bEq)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"function values cannot be ordered; only == and != "
			"are supported.");
		return true;
	}
	if (!((lFunc && rFunc) || (lFunc && rNull) || (rFunc && lNull)))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"a function value can only be compared with a "
			"function value or null.");
		return true;
	}
	return false;
}

//0.7.3 B D8: array values support identity comparison only (==/!=), and
//only against another array or null. Without this gate relational mixes
//(`a < b`) and scalar mixes (`a == 5`) compile and silently compare the
//raw heap handle against the operand. Mirrors the function-handle gate
//above; null keeps the sentinel comparison path. True = rejected.
bool ExprResolveAccessor::RejectArrayIdentityMisuse(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op, NodeKind lk, NodeKind rk,
	bool lNull, bool rNull)
{
	bool lArr = lk == NK_ArrayTypeToken;
	bool rArr = rk == NK_ArrayTypeToken;
	if (!lArr && !rArr)
		return false;
	bool bEq = op == SnBinaryExpr::OP_Equal
		|| op == SnBinaryExpr::OP_NotEqual;
	if (!bEq || !((lArr && rArr) || (lArr && rNull) || (rArr && lNull)))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"array values support identity comparison only "
			"(==/!=), and only against another array or null.");
		return true;
	}
	return false;
}

//0.7.5 numeric promotion (spec §2.2), registry-derived. Narrower-than-
//int integer operands promote to int FIRST (byte+byte = int, the C#
//rule); then the result is the smallest type that can implicitly
//receive BOTH operands. bool/char never arrive here (their categories
//are not numeric — callers gate). Returns null when no implicit common
//type exists (int/long + ulong).
SnField* ExprResolveAccessor::CommonNumericType(NodeKind lk, NodeKind rk)
{
	int li = ScalarPrimIndexOf(lk), ri = ScalarPrimIndexOf(rk);
	if (li < 0 || ri < 0)
		return nullptr;
	const auto &lRow = kScalarPrims[li], &rRow = kScalarPrims[ri];
	if (!PrimCategoryIsNumeric(lRow.category)
		|| !PrimCategoryIsNumeric(rRow.category))
		return nullptr;
	//float involved → the wider float rank wins; any float beats any
	//integer (C# rule): float+float stays float, byte+double is double.
	if (lRow.category == PC_Float || rRow.category == PC_Float)
	{
		bool anyDouble = (lRow.category == PC_Float && lRow.rank == 2)
			|| (rRow.category == PC_Float && rRow.rank == 2);
		return SnBuiltinDataType::InstanceOf(
			anyDouble ? NK_Double : NK_Float);
	}
	//both integers: widen narrower-than-int rows to int first
	//(registry rank <3 = narrower than the int/uint rank 3)
	if (lRow.rank < 3) { lk = NK_Int32; li = ScalarPrimIndexOf(lk); }
	if (rRow.rank < 3) { rk = NK_Int32; ri = ScalarPrimIndexOf(rk); }
	if (lk == rk)
		return SnBuiltinDataType::InstanceOf(lk);
	//smallest implicit receiver of both: the first registry integer row
	//both operands are domain-contained in (registry order is
	//int,..,uint,..,long,ulong — int+uint→long, long+uint→long,
	//uint+ulong→ulong; int+ulong finds no row → null).
	for (size_t i = 0; i < kScalarPrimCount; ++i)
	{
		const auto &p = kScalarPrims[i];
		if (!PrimCategoryIsNumeric(p.category) || p.category == PC_Float)
			continue;
		if (PrimDomainContained(kScalarPrims[li].category,
				kScalarPrims[li].rank, p.category, p.rank)
			&& PrimDomainContained(kScalarPrims[ri].category,
				kScalarPrims[ri].rank, p.category, p.rank))
			return SnBuiltinDataType::InstanceOf(p.kind);
	}
	return nullptr;  // e.g. int + ulong
}

//Symmetric numeric promotion (Phase 8e-8 mechanism) extended to
//comparisons. Prerequisite the arithmetic branch does not have: BOTH
//operands numeric and neither a null literal — class/enum/null pairs
//stay on their existing identity or sentinel paths, and wrapping them
//(as the arithmetic branch would) would break e.g. class identity
//equality. 0.7.5: the promotion type comes from CommonNumericType; a
//null common type (int vs ulong) is a compile error — codegen would
//otherwise compare mismatched widths. False = rejected.
bool ExprResolveAccessor::PromoteCompareOperands(SnBinaryExpr &sn,
	NodeKind lk, NodeKind rk, bool lNull, bool rNull)
{
	if (lNull || rNull || lk == rk)
		return true;
	if (!PrimKindIsNumeric(lk) || !PrimKindIsNumeric(rk))
		return true;
	SnField* T_promote = CommonNumericType(lk, rk);
	if (!T_promote)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"no implicit common type for \"%s\" and \"%s\".",
			sn.Left()->EvalDataType()->ToString().c_str(),
			sn.Right()->EvalDataType()->ToString().c_str());
		return false;
	}
	auto it = sn.Children().begin();
	auto& leftExpr = static_cast<SnExpression&>(*it);
	TypeCastInfo leftCI(leftExpr.EvalDataType(), T_promote);
	FixupExprType(it, leftCI);
	++it;
	auto& rightExpr = static_cast<SnExpression&>(*it);
	TypeCastInfo rightCI(rightExpr.EvalDataType(), T_promote);
	FixupExprType(it, rightCI);
	return true;
}

//Phase 11 Q4: relational operands get type checks here — the old
//shortcut set Int32 blindly and codegen picked the opcode variant from
//the left operand alone. Runs the four compare gates in order; false =
//a gate rejected (diagnostic logged), the caller stops resolution.
bool ExprResolveAccessor::CheckCompareOperands(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op)
{
	auto* L = sn.Left()->EvalDataType();
	auto* R = sn.Right() ? sn.Right()->EvalDataType() : nullptr;
	NodeKind lk = L ? L->Kind() : NK_Int32;
	NodeKind rk = R ? R->Kind() : NK_Int32;
	bool lNull = sn.Left()->ContainFlags(NF_NullLiteral);
	bool rNull = sn.Right() && sn.Right()->ContainFlags(NF_NullLiteral);
	if (RejectStringNonStringMix(sn, lk, rk, lNull, rNull))
		return false;
	if (RejectFuncHandleMisuse(sn, op, L, R, lNull, rNull))
		return false;
	if (RejectArrayIdentityMisuse(sn, op, lk, rk, lNull, rNull))
		return false;
	if (RejectBoolMisuse(sn, op, lk, rk))
		return false;
	if (!PromoteCompareOperands(sn, lk, rk, lNull, rNull))
		return false;
	return true;
}

//0.7.5 strict bool: logical operands feed OP_JumpIfNot and now must be
//bool — the same policy as statement conditions (CheckBoolCondition in
//StatementResolverFlow.cpp; widened together). Comparisons produce
//bool, so `a > 0 && b > 0` keeps working; raw int truthiness is a
//compile error. False = rejected.
bool ExprResolveAccessor::CheckLogicalBoolOperands(SnBinaryExpr &sn)
{
	auto bop = sn.Op();
	const char* szOp = bop == SnBinaryExpr::OP_LogicalAnd
		? "&&" : bop == SnBinaryExpr::OP_LogicalOr
		? "||" : "!";
	const char* szShape = sn.Right()
		? "bool operands" : "a bool operand";
	auto* pLT = sn.Left()->EvalDataType();
	if (pLT && pLT->Kind() != NK_Bool)
	{
		m_Env.Log(CLL_Error, sn.Left()->Location(),
			"operator '%s' requires %s, got \"%s\".",
			szOp, szShape, pLT->ToString().c_str());
		return false;
	}
	if (sn.Right())
	{
		auto* pRT = sn.Right()->EvalDataType();
		if (pRT && pRT->Kind() != NK_Bool)
		{
			m_Env.Log(CLL_Error, sn.Right()->Location(),
				"operator '%s' requires %s, got \"%s\".",
				szOp, szShape, pRT->ToString().c_str());
			return false;
		}
	}
	return true;
}

//0.7.5 strict bool: ==/!= between two bools is the only comparison
//bools support. Relational ordering on bool (< <= > >=) has no
//semantics, and mixed bool/non-bool (true == 1, b == null) must not
//silently compare the raw 0/1 carrier against an int — reject both.
//True = rejected (diagnostic logged).
bool ExprResolveAccessor::RejectBoolMisuse(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op, NodeKind lk, NodeKind rk)
{
	bool lBool = lk == NK_Bool;
	bool rBool = rk == NK_Bool;
	if (!lBool && !rBool)
		return false;
	bool bEq = op == SnBinaryExpr::OP_Equal
		|| op == SnBinaryExpr::OP_NotEqual;
	if (!bEq)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"bool values cannot be ordered; only == and != are "
			"supported.");
		return true;
	}
	if (!(lBool && rBool))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"a bool value can only be compared with a bool value.");
		return true;
	}
	return false;
}

//T_result selection for the arithmetic branch: string + OP_Add
//(concat; Phase 8e-9a) beats the numeric family beats int. Null = op
//not supported on a string operand, or no implicit common numeric type
//(both diagnosed).
SnField* ExprResolveAccessor::SelectArithmeticResultType(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op)
{
	auto* L = sn.Left()->EvalDataType();
	auto* R = sn.Right() ? sn.Right()->EvalDataType() : nullptr;
	NodeKind lk = L ? L->Kind() : NK_Int32;
	NodeKind rk = R ? R->Kind() : NK_Int32;
	if (lk == NK_String || rk == NK_String)
	{
		if (op != SnBinaryExpr::OP_Add)
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"operator not supported on string.");
			return nullptr;
		}
		//Phase 8e-9a: allow mixed (e.g. int + string). The non-string
		//operand is wrapped in SnCastExpr below; the backend emits the
		//kind-immediate OP_Prim_to_str for the conversion.
		//Then OP_Concat_str concatenates the two string indices.
		return SnBuiltinDataType::InstanceOf(NK_String);
	}
	//Unary negation keys on the operand alone — it never mixes two
	//operand types, so the common-type ladder below (whose int/ulong
	//pair has no implicit common) must not apply. `-9223372036854775808`
	//is the negated 2^63 ulong literal (the only lexical form of long
	//min since the 0.7.5 sign retirement); it resolves ulong-typed and
	//the assignment-side constant-fit gate folds it to long INT64_MIN.
	if (!R && PrimKindIsNumeric(lk))
		return L;
	//0.7.5: both operands numeric → the registry-derived common type;
	//a null common type (int + ulong) is a compile error, not a silent
	//int truncation. Non-numeric leftovers (enum arithmetic, void)
	//keep the legacy int result.
	if (PrimKindIsNumeric(lk) && PrimKindIsNumeric(rk))
	{
		SnField* T_common = CommonNumericType(lk, rk);
		if (!T_common)
		{
			m_Env.Log(CLL_Error, sn.Location(),
				"no implicit common type for \"%s\" and \"%s\".",
				L->ToString().c_str(),
				R->ToString().c_str());
			return nullptr;
		}
		return T_common;
	}
	return SnBuiltinDataType::InstanceOf(NK_Int32);
}

//Phase 8e-8: symmetric arithmetic promotion. Both operands are promoted
//to the wider type (int<float). For string only OP_Add is valid
//(concat); other ops on string are rejected in the T_result selection.
//Each operand is wrapped in SnCastExpr if its type differs from T_result
//so that codegen sees uniform operand types matching
//bin.EvalDataType(). Phase 11 Q4: null is only meaningful through the
//comparison identity path; arithmetic/concat with null is a compile
//error. (Before the null-sentinel fix it silently produced "0"
//concatenations; after it, an unwrapped raw 0.) False = rejected.
bool ExprResolveAccessor::ResolveArithmeticBinary(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op)
{
	if (sn.Left()->ContainFlags(NF_NullLiteral)
		|| (sn.Right() && sn.Right()->ContainFlags(NF_NullLiteral)))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"null is not a valid arithmetic operand.");
		return false;
	}
	SnField* T_result = SelectArithmeticResultType(sn, op);
	if (!T_result)
		return false;
	sn.EvalDataType(T_result);

	//Wrap each operand (in-place via FixupExprType) if its type differs
	//from T_result. After wrap, sn.Children()[0]/[1] hold the (possibly
	//cast) expressions; sn.Left()/Right() are stale but unused by codegen.
	auto it = sn.Children().begin();
	auto& leftExpr = static_cast<SnExpression&>(*it);
	TypeCastInfo leftCI(leftExpr.EvalDataType(), T_result);
	FixupExprType(it, leftCI);
	if (sn.Right())
	{
		++it;
		auto& rightExpr = static_cast<SnExpression&>(*it);
		TypeCastInfo rightCI(rightExpr.EvalDataType(), T_result);
		FixupExprType(it, rightCI);
	}
	return true;
}

void ExprResolveAccessor::Access(SnBinaryExpr &sn)
{
	assert(!sn.IsResolved());

	sn.Left()->Accept(*m_pVisitor);
	if (!sn.Left()->IsResolved())
		return;

	if (sn.Right())
	{
		sn.Right()->Accept(*m_pVisitor);
		if (!sn.Right()->IsResolved())
			return;
	}

	auto op = sn.Op();
	if (IsCompareOp(op) || op == SnBinaryExpr::OP_LogicalAnd ||
		op == SnBinaryExpr::OP_LogicalOr ||
		op == SnBinaryExpr::OP_LogicalNot)
	{
		if (IsCompareOp(op))
		{
			if (!CheckCompareOperands(sn, op))
				return;
		}
		else if (!CheckLogicalBoolOperands(sn))
			return;
		//0.7.5: comparisons and logical ops produce bool (carrier
		//int32 0/1 — OP_Cmp/OP_Eq_str/JumpIfNot bit patterns unchanged).
		auto* boolType = SnBuiltinDataType::InstanceOf(NK_Bool);
		sn.EvalDataType(boolType);
	}
	else if (!ResolveArithmeticBinary(sn, op))
		return;
	sn.AddFlags(NF_Resolved);
}

} //namespace nlang
