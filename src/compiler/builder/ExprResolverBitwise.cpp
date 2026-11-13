/*---
    ExprResolverBitwise.cpp — bitwise operator resolution (the
    integer gate, symmetric promotion for `&`/`^`/`|`, unary
    promotion for `~`). Extracted from ExprResolverBinary.cpp
    (v0.13.7 source-size-guard split, zero behavior change).
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
#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include <nlang/runtime/StackDump.h>

namespace nlang
{

//v0.13.7 family entry: dispatches the four bitwise operators to
//the binary / unary resolvers sharing the integer gate.
bool ExprResolveAccessor::ResolveBitwise(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op)
{
	if (op == SnBinaryExpr::OP_BitNot)
		return ResolveBitNot(sn);
	return ResolveBitwiseBinary(sn, op);
}

//v0.13.7 bitwise family (`&`, `^`, `|`): integer-only gate with the same
//policy as shift (enum members masquerade as Int32 and ride through;
//enum-typed variables have no integer row and are rejected). Then the
//arithmetic family's symmetric promotion (Phase 8e-8) decides the carrier
//row — unlike shift there is no count side, both operands promote on
//equal footing, so `1 & long` is legal and keeps the long bits. int/ulong
//has no implicit common row and stays a compile error, same boundary as
//`+`. False = rejected (diagnostic logged).
bool ExprResolveAccessor::ResolveBitwiseBinary(SnBinaryExpr &sn,
	SnBinaryExpr::Operator op)
{
	if (sn.Left()->ContainFlags(NF_NullLiteral)
		|| (sn.Right() && sn.Right()->ContainFlags(NF_NullLiteral)))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"null is not a valid bitwise operand.");
		return false;
	}
	auto* L = sn.Left()->EvalDataType();
	auto* R = sn.Right() ? sn.Right()->EvalDataType() : nullptr;
	NodeKind lk = L ? L->Kind() : NK_Int32;
	NodeKind rk = R ? R->Kind() : NK_Int32;
	const char* opName = op == SnBinaryExpr::OP_BitAnd ? "&"
		: op == SnBinaryExpr::OP_BitOr ? "|" : "^";
	if (!PrimKindIsInteger(lk) || !PrimKindIsInteger(rk))
	{
		auto* bad = PrimKindIsInteger(lk) ? R : L;
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '%s' requires integer operands, got \"%s\".",
			opName, bad ? bad->ToString().c_str() : "?");
		return false;
	}
	SnField* T_result = CommonNumericType(lk, rk);
	if (!T_result)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"no implicit common type for \"%s\" and \"%s\".",
			L->ToString().c_str(), R->ToString().c_str());
		return false;
	}
	sn.EvalDataType(T_result);
	//Wrap both operands in place to the carrier row (WrapShiftOperands'
	//pattern minus the count gate: both children go through FixupExprType).
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

//v0.13.7 unary `~`: null gate and integer gate of the binary family, then
//the unary-promotion left-row rule shared with shift (:356-360) — rows
//narrower than int widen to int32, rows rank ≥3 keep themselves. Not
//OP_Neg's row line: OP_Neg carries the operand row verbatim, `~` widens
//narrow rows so the VM runs the operation on the int carrier it emits.
bool ExprResolveAccessor::ResolveBitNot(SnBinaryExpr &sn)
{
	if (sn.Left()->ContainFlags(NF_NullLiteral))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"null is not a valid bitwise operand.");
		return false;
	}
	auto* L = sn.Left()->EvalDataType();
	NodeKind lk = L ? L->Kind() : NK_Int32;
	if (!PrimKindIsInteger(lk))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '~' requires an integer operand, got \"%s\".",
			L ? L->ToString().c_str() : "?");
		return false;
	}
	int li = ScalarPrimIndexOf(lk);
	if (kScalarPrims[li].rank < 3)
		lk = NK_Int32;
	SnField* T_result = SnBuiltinDataType::InstanceOf(lk);
	sn.EvalDataType(T_result);
	auto it = sn.Children().begin();
	auto& leftExpr = static_cast<SnExpression&>(*it);
	TypeCastInfo leftCI(leftExpr.EvalDataType(), T_result);
	FixupExprType(it, leftCI);
	return true;
}

} //namespace nlang