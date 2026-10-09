/*---
    StatementResolverAssign.cpp — 赋值类语句解析：return/赋值/复合赋值/下标赋值的类型检查与收敛。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"
#include <nlang/runtime/BuiltinGenericNames.h>

namespace nlang
{

void StatementResolveAccessor::Access(SnReturnStmt &sn)
{
	assert(m_pVisitor);
	assert(m_pCurrType && m_pCurrType->Kind() == NK_Function);
	if (m_inFinallyBody) {
		m_Env.Log(CLL_Error, sn.Location(),
			"return is not allowed inside a finally block");
		return;
	}
	auto pOuterFunc		= static_cast<SnFunction *>(m_pCurrType);
	auto pResultExpr	= sn.Result();

	auto pReturnType = pOuterFunc->ReturnType();
	if (!pResultExpr)
	{
		if (pReturnType)
			m_Env.Log(CLL_Error, sn.Location(),
				"Missing return value in a return statement.");
		return;
	}

	if (!pReturnType)
	{
		m_Env.Log(CLL_Error, pResultExpr->Location(),
			"No value should be returned here.");
		return;
	}

	pResultExpr->Accept(*m_pVisitor);

	//Phase 13: a return-position function reference binds against the
	//function's declared return type (TryBindStatementFuncRef).
	if (!TryBindStatementFuncRef(*pResultExpr,
		pOuterFunc->EvalDataType()))
		return;

	if (!pReturnType->IsResolved())
		return;
	auto pSourceType = pResultExpr->EvalDataType();
	auto pTargetType = pOuterFunc->EvalDataType();
	//0.7.3 B: array results and array return types both carry the
	//interned token, so the cast choke point adjudicates — same
	//token = Same (pass-through), different tokens = the named
	//array reject in FixupExprType, scalar targets = the generic
	//reject, string targets = Auto toString coercion.
	auto castInfo = GetCastInfo(pSourceType, pTargetType);
	auto iExpr = sn.Children().find(sn.m_pResult);
	if (m_ExprResolver.FixupExprType(iExpr, castInfo))
		sn.m_pResult = &static_cast<SnCastExpr &>(*iExpr);
}

void StatementResolveAccessor::Access(SnAssignStmt &sn)
{
	if (sn.IsResolved())
		return;
	assert(m_pVisitor);
	sn.Left()->Accept(*m_pVisitor);

	//Phase 9a: reject assignment to a const local.
	//(The const-init decomposition in Access(SnLocalDeclStmt&) sets
	//NF_Const AFTER resolving the initializer AssignStmt, so this
	//check correctly skips the init assignment.)
	RejectConstStoreTarget(*sn.Left(), sn.Location());
	RejectMethodCallStoreTarget(*sn.Left(), sn.Location());
	PropagateInitListTarget(sn);

	sn.Right()->Accept(*m_pVisitor);

	//Init lists set their own EvalDataType and don't go through the
	//cast-info path — skip cast fixup for them.
	if (FinishInitListAssign(sn))
		return;

	SnField* pTargetType = nullptr;
	if (!TryGetAssignTargetType(*sn.Left(), pTargetType))
		return;
	//Phase 13: an assignment-position function reference binds
	//against the LHS type. Local-decl decomposition, plain assignment
	//and field stores all flow through here (TryBindStatementFuncRef).
	if (!TryBindStatementFuncRef(*sn.Right(), pTargetType))
		return;
	SnField* pSourceType = nullptr;
	if (!TryGetAssignSourceType(*sn.Right(), pTargetType, pSourceType))
		return;
	//0.7.3 B: an array-valued RHS and an array target both carry
	//the interned token, so the cast choke point adjudicates —
	//same token = Same, different tokens = the named array reject
	//in FixupExprType, scalar targets = the generic reject, string
	//targets = Auto toString coercion. Local-decl initializers
	//decompose into SnAssignStmt, so declarations flow through the
	//same choke point. FixupExprType's false return leaves the
	//statement un-wrapped; NF_Resolved is set below so the
	//paragraph walk's revisit of the decomposed local-decl
	//statement doesn't log twice.
	auto castInfo = GetCastInfo(pSourceType, pTargetType);
	auto iExpr = sn.Children().find(sn.m_pRight);
	if (m_ExprResolver.FixupExprType(iExpr, castInfo))
		sn.m_pRight = &static_cast<SnCastExpr &>(*iExpr);
	sn.AddFlags(NF_Resolved);
}

void StatementResolveAccessor::Access(SnCompoundAssignStmt &sn)
{
	if (sn.IsResolved())
		return;
	assert(m_pVisitor);
	sn.Left()->Accept(*m_pVisitor);
	//Phase 9a: reject compound assignment to a const local.
	RejectConstStoreTarget(*sn.Left(), sn.Location());
	RejectMethodCallStoreTarget(*sn.Left(), sn.Location());
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
	auto castInfo = GetCastInfo(pSourceType, pTargetType);
	auto iExpr = sn.Children().find(sn.m_pRight);
	if (m_ExprResolver.FixupExprType(iExpr, castInfo))
		sn.m_pRight = &static_cast<SnCastExpr &>(*iExpr);
	sn.AddFlags(NF_Resolved);
}

void StatementResolveAccessor::Access(SnSubscriptAssignStmt &sn)
{
	if (sn.IsResolved())
		return;
	assert(m_pCurrType);
	if (sn.Array() && !sn.Array()->IsResolved())
		m_ExprResolver.Resolve(*sn.Array(), *sn.Array()->Parent(), *m_pCurrType, ERF_None);
	if (sn.Index() && !sn.Index()->IsResolved())
		m_ExprResolver.Resolve(*sn.Index(), *sn.Index()->Parent(), *m_pCurrType, ERF_None);
	if (sn.Value() && !sn.Value()->IsResolved())
		m_ExprResolver.Resolve(*sn.Value(), *sn.Value()->Parent(), *m_pCurrType, ERF_None);
	if (RejectStringBase(sn))
		return;
	if (sn.Value() && (IsUnboundFuncRef(*sn.Value())
		|| IsUnboundMemberFuncRef(*sn.Value())))
	{
		if (!TryBindSubscriptStoreFuncRef(sn))
			return;
	}
	//Element-type gate for array-valued bases vs. the container
	//subscript-store path (List/Dict sugar) — see the two Apply*
	//helpers for the coercion semantics of each arm.
	if (sn.Value() && sn.Value()->IsResolved() && sn.Array()
		&& sn.Array()->IsArrayValued())
	{
		ApplyArrayElementStoreCast(sn);
	}
	else if (sn.Value() && sn.Value()->IsResolved() && sn.Array()
		&& sn.Array()->IsResolved())
	{
		ApplyContainerStoreCasts(sn);
	}
	//Void-support: reject a resolved void call as the stored value —
	//codegen would store a stale pResult.
	if (sn.Value() && sn.Value()->IsResolved()
		&& !sn.Value()->EvalDataType())
	{
		m_Env.Log(CLL_Error, sn.Value()->Location(),
			"cannot assign the result of void function \"%s\".",
			sn.Value()->ToString().c_str());
	}
	sn.AddFlags(NF_Resolved);
}

//Phase 13: a return/assignment-position function reference binds against
//the expected type (return type or LHS type). Shared by every
//statement-level binding site in this TU.
bool StatementResolveAccessor::TryBindStatementFuncRef(SnExpression &expr,
	SnField *pExpectedType)
{
	if (IsUnboundFuncRef(expr))
		return BindFuncRefToExpected(m_Env,
			static_cast<SnIdentifierExpr&>(expr), pExpectedType);
	if (IsUnboundMemberFuncRef(expr))
		return BindMemberFuncRefToExpected(m_Env,
			static_cast<SnMemberExpr&>(expr), pExpectedType);
	return true;
}

//Assignment target type: an identifier LHS reads its bound field; a
//member Lvalue has already resolved its type onto the node. (Both
//callers — plain and compound assign — only ever receive identifier or
//member LHS shapes; subscript stores parse as SnSubscriptAssignStmt.)
//False = the target has no type yet (an unresolved identifier), and the
//caller must stop before deriving a source type.
bool StatementResolveAccessor::TryGetAssignTargetType(SnExpression &left,
	SnField* &pTargetType)
{
	if (left.Kind() == NK_IdentifierExpr)
	{
		auto* pLeftField = static_cast<SnIdentifierExpr&>(left).Field();
		if (!pLeftField)
			return false;
		pTargetType = pLeftField->EvalDataType();
	}
	else if (left.Kind() == NK_MemberExpr)
		pTargetType = left.EvalDataType();
	return true;
}

//Assignment source type, plus the shared void-RHS diagnostic. The log
//fires exactly when a resolved RHS has no type, so the caller sees the
//error and the false return skips the cast fixup.
bool StatementResolveAccessor::TryGetAssignSourceType(SnExpression &right,
	SnField *pTargetType, SnField* &pSourceType)
{
	pSourceType = right.EvalDataType();
	if (pTargetType && !pSourceType && right.IsResolved())
	{
		m_Env.Log(CLL_Error, right.Location(),
			"cannot assign the result of void function \"%s\".",
			right.ToString().c_str());
	}
	return pTargetType && pSourceType;
}

//Phase 9a: reject assignment to a const local (identifier targets only;
//the const-init decomposition sets NF_Const AFTER resolving the
//initializer AssignStmt, so initializer writes correctly pass).
void StatementResolveAccessor::RejectConstStoreTarget(SnExpression &left,
	const ISourceLocation *pLoc)
{
	if (left.Kind() != NK_IdentifierExpr)
		return;
	auto* pLeftField = static_cast<SnIdentifierExpr&>(left).Field();
	if (pLeftField && pLeftField->ContainFlags(NF_Const))
		m_Env.Log(CLL_Error, pLoc,
			"cannot assign to const local \"%s\".",
			pLeftField->Name().c_str());
}

//A method call LHS (`obj.f() = v`, `obj.f() += v`) parses as a store
//statement — the grammar's MemberExpr admits `Expression '.' InvokeExpr`
//because that is also how every method call is spelled — but the call
//result is a value, not a location. Without this gate the statement
//resolves quietly (the method's return type reads as the target type)
//and codegen's field-store paths silently emit nothing for it.
void StatementResolveAccessor::RejectMethodCallStoreTarget(
	SnExpression &left, const ISourceLocation *pLoc)
{
	if (left.Kind() != NK_MemberExpr)
		return;
	if (static_cast<SnMemberExpr&>(left).Inner()->Kind()
		== NK_IdentifierExpr)
		return;
	m_Env.Log(CLL_Error, pLoc,
		"cannot assign to the result of a method call.");
}

//Init lists set their own EvalDataType and don't go through the
//cast-info path — flag them resolved and skip cast fixup entirely.
bool StatementResolveAccessor::FinishInitListAssign(SnAssignStmt &sn)
{
	if (sn.Right()->Kind() != NK_InitListExpr)
		return false;
	if (sn.Right()->IsResolved())
		sn.AddFlags(NF_Resolved);
	return true;
}

//Init lists set their own type and don't go through the cast-info path,
//so an untyped `[]`/`{...}` RHS inherits the LHS field as its target.
void StatementResolveAccessor::PropagateInitListTarget(SnAssignStmt &sn)
{
	if (sn.Right()->Kind() != NK_InitListExpr)
		return;
	auto& initList = static_cast<SnInitListExpr&>(*sn.Right());
	if (initList.ExplicitType() || initList.InferredTarget())
		return;
	SnField* pLeftField = nullptr;
	if (sn.Left()->Kind() == NK_IdentifierExpr)
		pLeftField = static_cast<SnIdentifierExpr&>(
			*sn.Left()).Field();
	if (pLeftField)
		initList.InferredTarget(pLeftField);
}

//Phase 13 (subscript store): a function reference stored into an array
//element binds against the element type (an array-typed field's
//EvalDataType IS the element type). The receiver-bound member form is
//Step 2; `d[key] = value` binds against the Dict's VALUE type argument
//(review round-1 F7).
bool StatementResolveAccessor::TryBindSubscriptStoreFuncRef(
	SnSubscriptAssignStmt &sn)
{
	SnField* pElemType = nullptr;
	if (sn.Array() && sn.Array()->Kind() == NK_IdentifierExpr)
	{
		auto* pArrField = static_cast<SnIdentifierExpr&>(
			*sn.Array()).Field();
		if (pArrField && pArrField->IsArrayType())
		{
			pElemType = pArrField->EvalDataType();
			//0.7.3 B token path: an array-typed field carries
			//the interned token; the func-ref binds against the
			//ELEMENT type.
			if (pElemType && pElemType->Kind() == NK_ArrayTypeToken)
				pElemType = static_cast<SnArrayTypeToken*>(
					pElemType)->ElemTypeOf();
		}
		else if (pArrField && pArrField->EvalDataType()
			&& pArrField->EvalDataType()->Kind() == NK_ClassDecl)
		{
			auto* pDictDecl = static_cast<SnClassDecl*>(
				pArrField->EvalDataType());
			if (pDictDecl->IsGenericInstantiation()
				&& pDictDecl->BaseName() == kBuiltinDictTypeName)
			{
				auto typeArgs = GetGenericTypeArgs(pDictDecl);
				if (typeArgs.size() > 1)
					pElemType = typeArgs[1];
			}
		}
	}
	return TryBindStatementFuncRef(*sn.Value(), pElemType);
}

//0.7.3 B D3: write-path twin of the read arm in ExprResolver — a string
//base has no subscript store semantics. Before this arm the store fell
//through the container path with a null element type (strings are not
//generic instantiations) and failed only at runtime ("null array
//access").
bool StatementResolveAccessor::RejectStringBase(SnSubscriptAssignStmt &sn)
{
	if (sn.Array() && sn.Array()->IsResolved()
		&& sn.Array()->EvalDataType()
		&& sn.Array()->EvalDataType()->Kind() == NK_String)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"string does not support subscript access.");
		return true;
	}
	return false;
}

//Element-type gate for array-valued bases: coerce the stored value to
//the element type exactly like a plain assignment (box primitives into
//Object elements, coerce int→string, reject mismatches). The caller has
//already gated on the base's array-valued property (not its Kind),
//which covers every base shape — identifier, member, subscript (chained
//`li[0][1] = v`) and call (`mk()[0] = v`).
void StatementResolveAccessor::ApplyArrayElementStoreCast(
	SnSubscriptAssignStmt &sn)
{
	auto* pElemType = sn.Array()->EvalDataType();
	//0.7.3 B token path: an array-valued base carries the interned
	//token; the element gate consumes the ELEMENT.
	if (pElemType && pElemType->Kind() == NK_ArrayTypeToken)
		pElemType = static_cast<SnArrayTypeToken*>(
			pElemType)->ElemTypeOf();
	if (pElemType && sn.Value()->EvalDataType())
	{
		//0.7.3 B: the array-valued RHS carries its interned
		//token now, so the cast table sees it — string elements
		//coerce via the token→string Auto, every other element
		//kind is TCK_None ("Incompatible type"). The named
		//whole-value reject moved into GetCastInfo with the
		//representation flip.
		auto castInfo = GetCastInfo(
			sn.Value()->EvalDataType(), pElemType);
		auto iExpr = sn.Children().find(sn.m_pValue);
		if (m_ExprResolver.FixupExprType(iExpr, castInfo))
			sn.m_pValue = &static_cast<SnCastExpr &>(*iExpr);
	}
}

//Container subscript stores (List/Dict subscript sugar): the base is
//not array-valued. The value admits through the cast table against the
//element type-arg — the VALUE slot (List → args[0], Dict → args[1]),
//the same slot convention the codegen boxing plan uses.
//0.7.3 B terminal form: the slot carries the interned token for array
//elements, so the table adjudicates full identity with no array-ness
//pre-gate — same token = Same, mismatched tokens = the named array
//reject in FixupExprType, a non-null int against a class/interface/
//array slot = the null-only gate (l[0] = null passes), primitives
//coerce via Auto, Object elements box via Box (codegen's container-store
//lowering emits the wrapped value into the claim slot before its
//per-element boxing, so wraps are transparent there).
void StatementResolveAccessor::ApplyContainerStoreCasts(
	SnSubscriptAssignStmt &sn)
{
	SnField* pElemType = nullptr;
	SnField* pKeyType = nullptr;
	auto* pBaseType = sn.Array()->EvalDataType();
	if (pBaseType && pBaseType->Kind() == NK_ClassDecl)
	{
		auto* pGen = static_cast<SnClassDecl*>(pBaseType);
		auto typeArgs = GetGenericTypeArgs(pGen);
		if (pGen->BaseName() == kBuiltinDictTypeName && typeArgs.size() > 1)
		{
			pElemType = typeArgs[1];
			pKeyType = typeArgs[0];
		}
		else if (pGen->BaseName() == kBuiltinListTypeName
			&& !typeArgs.empty())
			pElemType = typeArgs[0];
	}
	//Dict subscript sugar `d[k] = v` lowers to set(k, v): the KEY
	//admits through the cast table against typeArgs[0] like the
	//method form — an ungated mismatched key corrupted the key slot
	//(DictKeysEqual compares by the declared kind) and crashed later
	//lookups. List indexes stay unchecked (a plain int position,
	//runtime-bounds-checked).
	if (pKeyType && sn.Index() && sn.Index()->IsResolved()
		&& sn.Index()->EvalDataType())
	{
		auto keyCast = GetCastInfo(
			sn.Index()->EvalDataType(), pKeyType);
		auto iKey = sn.Children().find(sn.m_pIndex);
		if (m_ExprResolver.FixupExprType(iKey, keyCast))
			sn.m_pIndex = &static_cast<SnCastExpr &>(*iKey);
	}
	if (pElemType && sn.Value()->EvalDataType())
	{
		auto castInfo = GetCastInfo(
			sn.Value()->EvalDataType(), pElemType);
		auto iExpr = sn.Children().find(sn.m_pValue);
		if (m_ExprResolver.FixupExprType(iExpr, castInfo))
			sn.m_pValue = &static_cast<SnCastExpr &>(*iExpr);
	}
}

} //namespace nlang
