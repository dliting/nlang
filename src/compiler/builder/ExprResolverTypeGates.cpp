/*---
    ExprResolverTypeGates.cpp — 二元表达式的操作数类型门（bool/char 族）
    从 ExprResolverBinary.cpp 抽取（2026-10-18 source-size 守卫驱动的
    粒度拆分，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"
#include <nlang/runtime/PrimitiveTypes.h>

namespace nlang
{

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

//0.7.5 char: char×char supports all six comparisons in code-point
//order; a char mixed with anything else (number, bool, string, null)
//is rejected — the comparison dispatch would otherwise silently pick a
//numeric kind and mis-compare. Mirrors the bool gate shape. True =
//rejected (diagnostic logged).
bool ExprResolveAccessor::RejectCharMisuse(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op, NodeKind lk, NodeKind rk)
{
	bool lChar = lk == NK_Char;
	bool rChar = rk == NK_Char;
	if (!lChar && !rChar)
		return false;
	if (!(lChar && rChar))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"a char value can only be compared with a char value.");
		return true;
	}
	return false;
}

//0.7.5 char: a char is a Unicode scalar value, not a number — 'a'+1 has
//no semantics and must not fall into the numeric ladder (whose legacy
//int fallback would emit a misleading cast diagnostic). Comparing with
//a number is equally rejected: convert explicitly with 'as int' first.
//String operands are exempt ("x" + 'y' is a legal concat). True =
//rejected (diagnostic logged).
bool ExprResolveAccessor::RejectCharArithmetic(SnBinaryExpr &sn,
	NodeKind lk, NodeKind rk)
{
	if (lk != NK_Char && rk != NK_Char)
		return false;
	if (lk == NK_String || rk == NK_String)
		return false;
	m_Env.Log(CLL_Error, sn.Location(),
		"char values do not support arithmetic; convert with 'as int'.");
	return true;
}

} //namespace nlang
