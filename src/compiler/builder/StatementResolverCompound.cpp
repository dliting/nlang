/*---
    StatementResolverCompound.cpp — 复合赋值语句解析（x op= v 的类型检查与门族）。
    从 StatementResolverAssign.cpp 拆出（0.9.10 尺寸守卫收口：按语句族分文件）。
---*/
#include "StatementResolver.h"
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/runtime/StackDump.h>

namespace nlang
{

void StatementResolveAccessor::Access(SnCompoundAssignStmt &sn)
{
	if (sn.IsResolved())
		return;
	NLANG_ASSERT(m_pVisitor);
	sn.Left()->Accept(*m_pVisitor);
	//Phase 9a: reject compound assignment to a const local.
	RejectConstStoreTarget(*sn.Left(), sn.Location());
	RejectMethodCallStoreTarget(*sn.Left(), sn.Location());
	if (RejectCompoundSubscriptTarget(*sn.Left(), sn.Location()))
		return;
	sn.Right()->Accept(*m_pVisitor);

	//Type check: RHS must be compatible with LHS type.
	//Apply cast fixup on RHS (same pattern as SnAssignStmt).
	SnField* pTargetType = nullptr;
	if (!TryGetAssignTargetType(*sn.Left(), pTargetType))
		return;
	SnField* pSourceType = nullptr;
	if (!TryGetAssignSourceType(*sn.Right(), pTargetType, pSourceType))
		return;
	//v0.13.7: arrays reject compound assignment by name (rationale on the helper).
	if (RejectArrayCompoundAssign(sn, pTargetType, pSourceType))
		return;
	//Phase 9a P3: operator-type legality check (mirrors
	//ExprResolveAccessor::Access(SnBinaryExpr&) logic).
	//String only supports += (concat); -=, *=, /=, %= are invalid.
	NodeKind lhsKind = pTargetType->Kind();
	if (lhsKind == NK_String && sn.Op() != SnBinaryExpr::OP_Add)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"operator not supported on string.");
		return;
	}
	//0.9.10: shift compound form (<<=/>>=) — mirrors ResolveShiftBinary's
	//gates with the LHS's declared row as the carrier (compound family
	//semantics — implicit narrowing store-back, the byte b += 5
	//precedent). No unary promotion: the left row IS the carrier, so the
	//count gate and the run-time range check key on the declared width.
	if (sn.Op() == SnBinaryExpr::OP_Shl || sn.Op() == SnBinaryExpr::OP_Shr)
	{
		FinishCompoundShift(sn, *pTargetType);
		return;
	}
	if (!CheckCompoundBitwiseTarget(sn, *pTargetType))
		return;
	auto castInfo = GetCastInfo(pSourceType, pTargetType);
	auto iExpr = sn.Children().find(sn.m_pRight);
	if (m_ExprResolver.FixupExprType(iExpr, castInfo))
		sn.m_pRight = &static_cast<SnCastExpr &>(*iExpr);
	sn.AddFlags(NF_Resolved);
}

//v0.13.7: array-involved compound assignment is rejected by name —
//array arithmetic lowers AS a tensor call on the plain binary form,
//and the compound emitter's kind dispatch has no array arm (the
//operand row would masquerade as its element carrier). The expanded
//form `a = a + b` takes the lowering. The string target keeps its
//pre-existing surface: `s += arr` appends the array's toString
//rendering, the same array-aware coercion the plain `s + arr` form
//takes. True = rejected (the caller stops).
bool StatementResolveAccessor::RejectArrayCompoundAssign(
	SnCompoundAssignStmt &sn, const SnField *pTargetType,
	const SnField *pSourceType)
{
	if (pTargetType->Kind() == NK_String)
		return false;
	if (!pTargetType->IsArrayType() && !pSourceType->IsArrayType())
		return false;
	if (pTargetType->IsArrayType())
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '%s=': compound assignment is not supported "
			"on arrays; write the expanded form instead (array "
			"arithmetic lowers to the tensor package).",
			SnBinaryExpr::OperatorSymbol(sn.Op()));
	else
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '%s=': compound assignment with an array value "
			"is not supported on this target type; write the expanded "
			"form instead.",
			SnBinaryExpr::OperatorSymbol(sn.Op()));
	return true;
}

//0.9.10: the shift branch of Access(SnCompoundAssignStmt&), split out at
//the source-size guard. The integer-operand gate mirrors the binary
//form's operand row (PrimKindIsInteger — enum-typed variables are not
//integer rows); the count goes through AdmitShiftCount, the shared
//binary-form gate, with the LHS's declared row as the carrier.
void StatementResolveAccessor::FinishCompoundShift(SnCompoundAssignStmt &sn,
	SnField &targetType)
{
	const char* opName = sn.Op() == SnBinaryExpr::OP_Shl ? "<<=" : ">>=";
	if (!PrimKindIsInteger(targetType.Kind()))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '%s' requires integer operands, got \"%s\".",
			opName, targetType.ToString().c_str());
		return;
	}
	auto iExpr = sn.Children().find(sn.m_pRight);
	m_ExprResolver.AdmitShiftCount(iExpr, &targetType, sn.Location(),
		opName);
	sn.AddFlags(NF_Resolved);
}

//v0.13.7: gate for the bitwise compound forms (&=/|=/^=). True for every
//other operator (pass-through); for the bitwise ops it applies the
//integer-only operand rule the binary form shares (PrimKindIsInteger —
//enum-typed variables are not integer rows). After the gate the right
//operand takes the arithmetic family's implicit conversion into the
//declared row and the node store-backs like += (compound family
//semantics: the store-back narrows, the byte b += 5 precedent). Unlike
//shift there is no count side, so no count gate runs.
bool StatementResolveAccessor::CheckCompoundBitwiseTarget(
	SnCompoundAssignStmt &sn, SnField &targetType)
{
	auto op = sn.Op();
	if (op != SnBinaryExpr::OP_BitAnd && op != SnBinaryExpr::OP_BitOr
		&& op != SnBinaryExpr::OP_BitXor)
		return true;
	const char* opName = op == SnBinaryExpr::OP_BitAnd ? "&="
		: op == SnBinaryExpr::OP_BitOr ? "|=" : "^=";
	if (!PrimKindIsInteger(targetType.Kind()))
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"operator '%s' requires integer operands, got \"%s\".",
			opName, targetType.ToString().c_str());
		return false;
	}
	return true;
}

} //namespace nlang
