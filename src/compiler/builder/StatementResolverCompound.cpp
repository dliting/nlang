/*---
    StatementResolverCompound.cpp — 复合赋值语句解析（x op= v 的类型检查与门族）。
    从 StatementResolverAssign.cpp 拆出（0.9.10 尺寸守卫收口：按语句族分文件）。
---*/
#include "StatementResolver.h"
#include <nlang/runtime/PrimitiveTypes.h>

namespace nlang
{

void StatementResolveAccessor::Access(SnCompoundAssignStmt &sn)
{
	if (sn.IsResolved())
		return;
	assert(m_pVisitor);
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
	auto castInfo = GetCastInfo(pSourceType, pTargetType);
	auto iExpr = sn.Children().find(sn.m_pRight);
	if (m_ExprResolver.FixupExprType(iExpr, castInfo))
		sn.m_pRight = &static_cast<SnCastExpr &>(*iExpr);
	sn.AddFlags(NF_Resolved);
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

} //namespace nlang
