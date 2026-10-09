/*---
    StatementResolverFlow.cpp — 控制流语句解析：条件 int 门与 if/while/do/for/foreach/break/continue。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"
#include <nlang/runtime/BuiltinGenericNames.h>

namespace nlang
{

//0.7.5 strict bool: conditions must be bool (carrier int32 0/1, so
//OP_JumpIfNot bit patterns are unchanged). Historically int was
//accepted and non-int kinds read as silent garbage (string = pool
//handle, struct/array = heap index, float = bit luck); comparisons
//now produce bool, so the int-truthiness escape hatch is closed.
//The logical-operator operand gate in ExprResolverBinary.cpp
//(CheckLogicalBoolOperands) mirrors this policy — widened together.
void StatementResolveAccessor::CheckBoolCondition(SnExpression &cond, const char *what)
{
	if (!cond.IsResolved())
		return;
	auto* pType = cond.EvalDataType();
	if (pType && pType->Kind() != NK_Bool) {
		m_Env.Log(CLL_Error, cond.Location(),
			"%s condition must be bool, not \"%s\".",
			what, pType->ToString().c_str());
	}
}

void StatementResolveAccessor::Access(SnIfStmt &sn)
{
	assert(m_pVisitor);
	sn.Cond()->Accept(*m_pVisitor);
	CheckBoolCondition(*sn.Cond(), "if");
	sn.ThenStmt()->Accept(*m_pVisitor);
	if (sn.ElseStmt())
		sn.ElseStmt()->Accept(*m_pVisitor);
}

void StatementResolveAccessor::Access(SnWhileStmt &sn)
{
	assert(m_pVisitor);
	sn.Cond()->Accept(*m_pVisitor);
	CheckBoolCondition(*sn.Cond(), "while");
	sn.Body()->Accept(*m_pVisitor);
}

void StatementResolveAccessor::Access(SnDoStmt &sn)
{
	assert(m_pVisitor);
	sn.Body()->Accept(*m_pVisitor);
	sn.Cond()->Accept(*m_pVisitor);
	CheckBoolCondition(*sn.Cond(), "do-while");
}

//For-init local declaration. Route the type through ExprResolver, not
//the StatementResolver visitor — its Access(SnArrayTypeExpr&) is empty,
//so an array-typed for-init (for (int[] x = ...)) would never resolve
//and body references die (same trap as SnLocalDeclStmt above).
void StatementResolveAccessor::ResolveForInitDecl(SnForStmt &sn)
{
	auto& decl = static_cast<SnLocalDeclStmt&>(*sn.Init());
	m_ExprResolver.Resolve(*decl.Type(), *sn.Parent(), *m_pCurrType,
		ERF_None);
	//Array redesign B: reject jagged for-init (int[][] i);
	//ArrayTypeDepth is shape-based (Kind chain) and works whether
	//or not the type resolved.
	if (ArrayTypeDepth(decl.Type()) >= 2)
		m_Env.Log(CLL_Error, decl.Location(),
			"jagged arrays (T[][]) are not supported");
	if (!decl.Type()->IsResolved())
		return;
	auto *pTypeField = decl.Type()->Field();
	auto *pParagraph = FindEnclosingParagraph(sn.Parent());
	auto bIsArray = decl.Type()->IsArrayType();
	for (auto& d : decl.Decls()) {
		auto *pLocal = new SnLocalVar(d.name, pTypeField,
			*decl.Location());
		if (bIsArray)
			pLocal->SetArrayType(true);
		if (pParagraph) {
			pParagraph->AddLocal(d.name, pLocal);
		}
		if (d.pInitExpr) {
			auto *pAssign = BuildForInitAssign(decl, d, pParagraph);
			sn.InitExtras().push_back(pAssign);
		}
	}
}

//Build and pre-resolve the decomposed `x = init` assign for one
//for-init declarator. Detach from the inner LocalDeclStmt (decl —
//not sn!) so the AssignStmt becomes the sole owner. The typed slot
//keeps aliasing the node until the nulling at the end, so the
//d.pInitExpr reads stay valid. The caller registers the result in
//InitExtras for codegen.
SnAssignStmt *StatementResolveAccessor::BuildForInitAssign(
	SnLocalDeclStmt &decl, SnLocalDeclStmt::LocalDecl &d,
	SnParagraph *pParagraph)
{
	auto *pLeft = new SnIdentifierExpr(
		new std::string(d.name), *decl.Location());
	auto *pAssign = new SnAssignStmt(
		pLeft, decl.DetachChild(d.pInitExpr), *decl.Location());

	if (pParagraph) {
		m_ExprResolver.Resolve(*pLeft,
			*pParagraph, *m_pCurrType, ERF_None);
		m_ExprResolver.Resolve(*d.pInitExpr,
			*pParagraph, *m_pCurrType, ERF_None);

		auto* pLeftField2 = pLeft->IsResolved()
			? pLeft->Field() : nullptr;
		if (pLeftField2) {
			auto* pTgt = pLeftField2->EvalDataType();
			auto* pSrc = d.pInitExpr->EvalDataType();
			if (pTgt && pSrc) {
				auto ci = GetCastInfo(pSrc, pTgt);
				auto iExpr = pAssign->Children().find(
					pAssign->m_pRight);
				if (m_ExprResolver.FixupExprType(
						iExpr, ci))
					pAssign->m_pRight =
						&static_cast<SnCastExpr&>(
							*iExpr);
			}
		}
		pAssign->AddFlags(NF_Resolved);
	}

	d.pInitExpr = nullptr;
	return pAssign;
}

void StatementResolveAccessor::Access(SnForStmt &sn)
{
	assert(m_pVisitor);

	if (sn.Init() && sn.Init()->Kind() == NK_LocalDeclStmt)
		ResolveForInitDecl(sn);
	else if (sn.Init()) {
		sn.Init()->Accept(*m_pVisitor);
	}

	sn.Cond()->Accept(*m_pVisitor);
	CheckBoolCondition(*sn.Cond(), "for");
	sn.Body()->Accept(*m_pVisitor);
	if (sn.Fini())
		sn.Fini()->Accept(*m_pVisitor);
}

//0.7.3 B D10: array-valued sources (get()/subscript/call/new
//results) are ACCEPTED — the source carries the interned array
//token in EvalDataType, and codegen's dispatch routes value
//forms through the same token (the masquerade-era rejection
//existed only because the degraded EvalDataType carried no
//array identity). The iterable still evaluates once into the
//hidden iter slot; the source expression is not re-evaluated
//per iteration.
//Array redesign B (spec §5.5 #3): a resolved source that is
//neither an array nor a List/Dict used to compile and die at
//runtime (null reference in CallMethod) — reject by name here.
//Array sources (lvalue and value forms alike) pass via isArray;
//container values (incl. invoke form) pass isContainer below.
void StatementResolveAccessor::CheckForeachSource(SnForeachStmt &sn)
{
	if (!sn.Iterable()->IsResolved())
		return;
	auto* pSrcType = sn.Iterable()->EvalDataType();
	const bool isArray = sn.Iterable()->IsArrayValued();
	const bool isContainer = pSrcType
		&& pSrcType->Kind() == NK_ClassDecl
		&& static_cast<SnClassDecl*>(pSrcType)->IsGenericInstantiation()
		&& (static_cast<SnClassDecl*>(pSrcType)->BaseName()
			== kBuiltinListTypeName
			|| static_cast<SnClassDecl*>(pSrcType)->BaseName()
			== kBuiltinDictTypeName);
	//0.7.5 char bridge: a string source iterates Unicode scalar values
	//(code points) via UTF-8 decoding; the loop variable is char.
	const bool isString = pSrcType && pSrcType->Kind() == NK_String;
	if (!isArray && !isContainer && !isString)
		m_Env.Log(CLL_Error, sn.Iterable()->Location(),
			"the foreach source must be an array, List, Dict, or string");
}

//Exact-match gate: the loop variable type must match the source
//element type. 0.7.3 B terminal form — both sides flow as type
//identities: the var side is the declared type's BOUND FIELD
//(Field(), the type node itself — the interned token for
//`int[]` vars, the plain field otherwise; a type expression's
//EvalDataType() is the type's own meta-type, e.g.
//SnType::Instance() for built-ins, and never matches), the
//source side is the iteration element: the token's element for
//array sources (lvalue or value form), the container's first
//type-arg for List<T> and Dict KEY iteration (Dict walks keys;
//for array elements that slot IS the interned token). Pointer
//identity carries array-ness — `foreach (int r in List<int[]>)`
//compares an int field against the token and rejects.
void StatementResolveAccessor::MatchForeachElemType(SnForeachStmt &sn,
	SnField *pVarField)
{
	if (!pVarField || !sn.Iterable()->IsResolved())
		return;
	auto* pSrcType = sn.Iterable()->EvalDataType();
	SnField *pElemField = nullptr;
	if (pSrcType && pSrcType->Kind() == NK_ArrayTypeToken)
	{
		pElemField = static_cast<SnArrayTypeToken*>(
			pSrcType)->ElemTypeOf();
	}
	else if (pSrcType && pSrcType->Kind() == NK_ClassDecl)
	{
		auto* pGen = static_cast<SnClassDecl*>(pSrcType);
		if (pGen->IsGenericInstantiation()
			&& (pGen->BaseName() == kBuiltinListTypeName
				|| pGen->BaseName() == kBuiltinDictTypeName)
			&& !pGen->GenericTypeArgs().empty())
		{
			pElemField = pGen->GenericTypeArgs()[0];
		}
	}
	//0.7.5 char bridge: a string source yields code points — the loop
	//variable must be char (singleton identity, like the var side).
	else if (pSrcType && pSrcType->Kind() == NK_String)
	{
		pElemField = SnBuiltinDataType::InstanceOf(NK_Char);
	}
	if (pElemField && pElemField != pVarField)
		m_Env.Log(CLL_Error, sn.VarType()->Location(),
			"the foreach variable type does not match the "
			"element type");
}

//Phase 8e-5: foreach statement resolver.
//Resolves iterable + var type, registers the loop var in the enclosing
//Paragraph scope (function-scoped, same as for-loop). Element type and
//3-way dispatch (Array / List / Dict) is determined later in codegen via
//EvalDataType; the resolver does not need to compute it.
void StatementResolveAccessor::Access(SnForeachStmt &sn)
{
	assert(m_pVisitor);

	//Find enclosing Paragraph for loop-var registration (mirror for-loop).
	auto *pParagraph = FindEnclosingParagraph(sn.Parent());

	//1. Resolve iterable (EvalDataType gets populated for codegen to use).
	sn.Iterable()->Accept(*m_pVisitor);
	CheckForeachSource(sn);

	//2. Resolve declared var type. Route through ExprResolver, not
	//the StatementResolver visitor — its Access(SnArrayTypeExpr&) is
	//empty, so an array-typed loop var (foreach (int[] row in ...))
	//would never resolve and body references die with "Cannot
	//resolve the field" (same trap as SnLocalDeclStmt above).
	m_ExprResolver.Resolve(*sn.VarType(), *sn.Parent(), *m_pCurrType,
		ERF_None);
	//Array redesign B: reject jagged loop vars (foreach (int[][] x in
	//...)); ArrayTypeDepth is shape-based (Kind chain) and works
	//whether or not the type resolved.
	if (ArrayTypeDepth(sn.VarType()) >= 2)
		m_Env.Log(CLL_Error, sn.VarType()->Location(),
			"jagged arrays (T[][]) are not supported");
	SnField *pVarField = nullptr;
	if (sn.VarType()->IsResolved())
		pVarField = sn.VarType()->Field();

	MatchForeachElemType(sn, pVarField);

	//3. Register loop var in paragraph scope (function-scoped).
	if (pVarField && pParagraph) {
		auto *pLocal = new SnLocalVar(sn.VarName(), pVarField,
			*sn.Location());
		if (sn.VarType()->IsArrayType())
			pLocal->SetArrayType(true);
		pParagraph->AddLocal(sn.VarName(), pLocal);
	}

	//4. Descend into body.
	sn.Body()->Accept(*m_pVisitor);
}

void StatementResolveAccessor::Access(SnBreakStmt &sn)
{
	//Phase 9d-2: control transfer out of a finally body is rejected
	//(would swallow exceptions/control flow — Java allows it, MVP does not).
	if (m_inFinallyBody)
		m_Env.Log(CLL_Error, sn.Location(),
			"break is not allowed inside a finally block");
}

void StatementResolveAccessor::Access(SnContinueStmt &sn)
{
	if (m_inFinallyBody)
		m_Env.Log(CLL_Error, sn.Location(),
			"continue is not allowed inside a finally block");
}

} //namespace nlang
