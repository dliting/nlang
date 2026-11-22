/*---
    StatementResolverFlow.cpp — 控制流语句解析：条件 int 门与 if/while/do/for/foreach/break/continue。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"
#include <nlang/runtime/BuiltinGenericNames.h>
#include <nlang/runtime/StackDump.h>
#include <set>

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
	NLANG_ASSERT(m_pVisitor);
	sn.Cond()->Accept(*m_pVisitor);
	CheckBoolCondition(*sn.Cond(), "if");
	sn.ThenStmt()->Accept(*m_pVisitor);
	if (sn.ElseStmt())
		sn.ElseStmt()->Accept(*m_pVisitor);
}

void StatementResolveAccessor::Access(SnWhileStmt &sn)
{
	NLANG_ASSERT(m_pVisitor);
	sn.Cond()->Accept(*m_pVisitor);
	CheckBoolCondition(*sn.Cond(), "while");
	FlowDepthGuard guard(m_breakableDepth, m_loopDepth);
	sn.Body()->Accept(*m_pVisitor);
}

void StatementResolveAccessor::Access(SnDoStmt &sn)
{
	NLANG_ASSERT(m_pVisitor);
	FlowDepthGuard guard(m_breakableDepth, m_loopDepth);
	sn.Body()->Accept(*m_pVisitor);
	sn.Cond()->Accept(*m_pVisitor);
	CheckBoolCondition(*sn.Cond(), "do-while");
}

//For-init local declaration. Route the type through ExprResolver, not
//the StatementResolver visitor — its Access(SnArrayTypeExpr&) is empty,
//so an array-typed for-init (for (int[] x = ...)) would never resolve
//and body references die (same trap as SnLocalDeclStmt above).
//0.13.10 Task 9 (loop-scoped heads): returns the bindings the head
//declarations displaced in the enclosing paragraph map — the caller
//restores them (or erases the names) once the loop's subtree resolves.
std::vector<std::pair<std::string, SnField*>>
StatementResolveAccessor::ResolveForInitDecl(SnForStmt &sn)
{
	std::vector<std::pair<std::string, SnField*>> swapped;
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
		return swapped;
	//v0.12.2 face gate (mirror of Access(SnLocalDeclStmt&)).
	if (RejectPreludeFaceUse(m_Env, decl.Type()->Field(),
		decl.Type()->Location(), "used as a type"))
		return swapped;
	auto *pParagraph = FindEnclosingParagraph(sn.Parent());
	if (!pParagraph)
		return swapped;
	auto bIsArray = decl.Type()->IsArrayType();
	//Every head of one init declares into the same scope (this
	//statement): same-name declarators reject exactly like a plain
	//declaration statement (first-wins AddLocal there — mirror of
	//RejectDuplicateDeclarator's diagnostic). Rejecting up front keeps a
	//same-round swap from displacing an earlier head of this init,
	//which the restore bookkeeping below could not unwind.
	std::set<std::string> seenNames;
	for (auto& d : decl.Decls()) {
		if (!seenNames.insert(d.name).second) {
			m_Env.Log(CLL_Error, decl.Location(),
				"local '%s' is already declared in this scope",
				d.name.c_str());
			continue;
		}
		auto displacement = ResolveOneForInitHead(sn, decl, d,
			*pParagraph, decl.Type()->Field(), bIsArray);
		if (!displacement.first.empty())
			swapped.push_back(std::move(displacement));
	}
	return swapped;
}

//One for-init declarator: the head's descriptor, its swap into the
//enclosing paragraph map, and the decomposed init assign registered in
//InitExtras. Returns the binding the swap displaced (an empty name
//marks a rejected declarator — nothing was swapped).
std::pair<std::string, SnField*>
StatementResolveAccessor::ResolveOneForInitHead(SnForStmt &sn,
	SnLocalDeclStmt &decl, SnLocalDeclStmt::LocalDecl &d,
	SnParagraph &paragraph, SnField *pTypeField, bool bIsArray)
{
	//0.9.0: direct-init and sized-array declarators in for-init
	//position would be silently dropped here (no InitExtras
	//synthesis in this path) — reject loudly instead of compiling
	//an unconstructed local.
	if (d.pCtorArgs || d.pArraySize) {
		m_Env.Log(CLL_Error, decl.Location(),
			"direct-init and sized-array declarations are not "
			"supported in a for initializer; declare before the loop");
		return {};
	}
	//0.9.0 semantic restriction: the for-init variable may not
	//reuse a visible type name.
	RejectTypeNameVariable(d.name, paragraph, *m_pCurrType,
		decl.Location());
	//0.13.10 Task 9 (loop-scoped heads): descriptor creation goes
	//through the descriptor half of the factory every declarator path
	//shares — it publishes d.pNode besides carrying every codegen flag.
	//A hand-rolled descriptor here once left d.pNode null, hiding the
	//local from the capture analysis (no display, no capture stamp).
	auto *pLocal = CreateLocalDescriptor(d, *decl.Location(),
		pTypeField, bIsArray, nullptr);
	//The head scopes to the for statement: swap it into the
	//enclosing map (the header expressions resolve through parent-
	//chain lookups, not the body paragraph), shadowing any outer
	//binding; Access(SnForStmt&) restores when the subtree is done.
	std::pair<std::string, SnField*> displacement(d.name,
		paragraph.SwapLocal(d.name, pLocal));
	if (d.pInitExpr) {
		auto *pAssign = BuildForInitAssign(decl, d, &paragraph);
		sn.InitExtras().push_back(pAssign);
	}
	return displacement;
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
	NLANG_ASSERT(m_pVisitor);

	//0.13.10 Task 9 (loop-scoped heads): the init declarations scope to
	//this statement — swap them in ahead of the subtree and restore the
	//paragraph map afterwards (a shadowed outer binding comes back; an
	//unshadowed name disappears with the loop).
	std::vector<std::pair<std::string, SnField*>> swapped;
	if (sn.Init() && sn.Init()->Kind() == NK_LocalDeclStmt)
		swapped = ResolveForInitDecl(sn);
	else if (sn.Init()) {
		sn.Init()->Accept(*m_pVisitor);
	}

	sn.Cond()->Accept(*m_pVisitor);
	CheckBoolCondition(*sn.Cond(), "for");
	FlowDepthGuard guard(m_breakableDepth, m_loopDepth);
	sn.Body()->Accept(*m_pVisitor);
	if (sn.Fini())
		sn.Fini()->Accept(*m_pVisitor);

	if (auto *pParagraph = FindEnclosingParagraph(sn.Parent()))
		for (auto it = swapped.rbegin(); it != swapped.rend(); ++it)
			pParagraph->RestoreLocal(it->first, it->second);
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
				|| pGen->BaseName() == kBuiltinDictTypeName
				//v0.12.7: Array<E> iteration yields E (the generic
				//spelling of the single array type).
				|| pGen->BaseName() == kBuiltinArrayTypeName)
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

//0.9.13 D-new-2: a bare `[...]`/`{...}` iterable has no target until the loop-var
//type is known — supply the interned array token of the element field so
//the init list resolves against it (the same shape the assignment site
//passes). A jagged loop var (T[]) would create a token-of-token target:
//keep the existing jagged diagnostic instead.
static void SupplyForeachInitListTarget(BuildEnvironment &env,
	ExprResolver &resolver, SnForeachStmt &sn, SnField *pVarField)
{
	auto *pInitList = UntypedInitListOf(*sn.Iterable());
	if (!pInitList || !pVarField)
		return;
	if (ArrayTypeDepth(sn.VarType()) >= 1)
	{
		env.Log(CLL_Error, sn.VarType()->Location(),
			"jagged arrays (T[][]) are not supported");
		return;
	}
	//0.10.8 D3: the brace form iterates a List<T> instantiated from the
	//loop-var type (an array target would hit the array-form gate);
	//the bracket form keeps the interned array token.
	if (!pInitList->IsArrayForm())
	{
		auto *pListDecl = resolver.FindDeclaredGenericInstance(
			*sn.Parent(), kBuiltinListTypeName, {pVarField});
		if (pListDecl)
			pInitList->InferredTarget(pListDecl);
		return;
	}
	pInitList->InferredTarget(env.InternArrayTypeToken(pVarField));
}

//v0.13.9 phase 3 split (source-size guard): step 3 of the foreach walk
//— the loop-var swap into the enclosing paragraph's local map. 0.13.10
//Task 9 (loop-scoped heads): the variable scopes to the foreach
//statement, so it swaps in (shadowing any outer binding) and the caller
//restores the map once the loop's subtree resolves.
SnField *StatementResolveAccessor::RegisterForeachLoopVar(SnForeachStmt &sn,
	SnParagraph &paragraph, SnField *pVarField)
{
	//0.9.0 semantic restriction: the foreach loop variable may not
	//reuse a visible type name.
	RejectTypeNameVariable(sn.VarName(), paragraph, *m_pCurrType,
		sn.Location());
	auto *pLocal = new SnLocalVar(sn.VarName(), pVarField,
		*sn.Location());
	if (sn.VarType()->IsArrayType())
		pLocal->SetArrayType(true);
	//Task 7 (D6): the & mark rides the VarType wrapper; the descriptor
	//is built from the stripped inner node, so re-attach the flag for
	//the dereference machinery to engage on body reads/writes. Bindable
	//scalar/enum/string elements only — struct elements follow the
	//pair-reads-as-handle convention without a flag, and class/array
	//elements degrade to value form (spec 3.1).
	if (sn.VarType()->ContainFlags(NF_Ref) && pVarField)
	{
		const NodeKind elemKind = pVarField->Kind();
		if (IsPrimitiveType(elemKind) || elemKind == NK_EnumDecl)
			pLocal->AddFlags(NF_Ref);
	}
	//Publish the descriptor on the statement: the swap/restore of the
	//map means later passes (the capture analysis) cannot recover it
	//by name lookup.
	sn.SetVarLocal(pLocal);
	return paragraph.SwapLocal(sn.VarName(), pLocal);
}

//Foreach var-type resolution (phase 1 of the foreach walk). Route the
//type through ExprResolver, not the StatementResolver visitor — its
//Access(SnArrayTypeExpr&) is empty, so an array-typed loop var would
//never resolve and body references die (same trap as SnLocalDeclStmt
//above). Answers the bound type field; null on an unresolved type (the
//caller keeps walking for cascading diagnostics).
SnField *StatementResolveAccessor::ResolveForeachVarType(SnForeachStmt &sn)
{
	m_ExprResolver.Resolve(*sn.VarType(), *sn.Parent(), *m_pCurrType,
		ERF_None);
	//Array redesign B: reject jagged loop vars; ArrayTypeDepth is
	//shape-based and works whether or not the type resolved.
	if (ArrayTypeDepth(sn.VarType()) >= 2)
		m_Env.Log(CLL_Error, sn.VarType()->Location(),
			"jagged arrays (T[][]) are not supported");
	if (!sn.VarType()->IsResolved())
		return nullptr;
	return sn.VarType()->Field();
}

//Phase 8e-5: foreach statement resolver.
//Resolves iterable + var type, registers the loop var in the enclosing
//Paragraph scope. 0.13.10 Task 9 (loop-scoped heads): the variable
//scopes to the foreach statement — swap in, resolve the subtree,
//restore the displaced binding (or erase the name).
void StatementResolveAccessor::Access(SnForeachStmt &sn)
{
	NLANG_ASSERT(m_pVisitor);

	//Find enclosing Paragraph for loop-var registration (mirror for-loop).
	auto *pParagraph = FindEnclosingParagraph(sn.Parent());

	//1. Resolve declared var type.
	SnField *pVarField = ResolveForeachVarType(sn);
	//v0.12.2 face gate (mirror of Access(SnLocalDeclStmt&)).
	if (pVarField && RejectPreludeFaceUse(m_Env, pVarField,
		sn.VarType()->Location(), "used as a type"))
		return;

	//2. Resolve iterable; 0.9.13 D-new-2 supplies its target first.
	SupplyForeachInitListTarget(m_Env, m_ExprResolver, sn, pVarField);
	sn.Iterable()->Accept(*m_pVisitor);
	CheckForeachSource(sn);

	//v0.13.9 phase 3: a & loop variable aliases the element — Dict-key
	//and string sources are materialized copies and reject by name.
	if (sn.VarType()->ContainFlags(NF_Ref))
		RejectRefForeachSource(sn);

	MatchForeachElemType(sn, pVarField);

	//3. Swap the loop var into the paragraph map (loop-scoped).
	SnField *pDisplaced = nullptr;
	if (pVarField && pParagraph)
		pDisplaced = RegisterForeachLoopVar(sn, *pParagraph,
			pVarField);

	//4. Descend into body, then restore the displaced binding (or
	//erase the name when the loop shadowed nothing). The condition
	//mirrors the registration gate — restoring after a skipped
	//registration would erase a same-named outer binding (spurious
	//cascade diagnostic on top of the real unresolved-type error).
	FlowDepthGuard guard(m_breakableDepth, m_loopDepth);
	sn.Body()->Accept(*m_pVisitor);

	if (pParagraph && pVarField)
		pParagraph->RestoreLocal(sn.VarName(), pDisplaced);
}

void StatementResolveAccessor::Access(SnBreakStmt &sn)
{
	//v0.9.1: a break outside any loop or switch reaches codegen with an
	//empty loop stack — its empty-guard returns early, so Release emits
	//nothing and execution falls through (a silent no-op; Debug asserts).
	//Reject cleanly at resolve time instead.
	if (m_breakableDepth == 0)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"break statement outside of a loop or switch");
		return;
	}
	//Phase 9d-2: control transfer out of a finally body is rejected
	//(would swallow exceptions/control flow — Java allows it, MVP does not).
	if (m_inFinallyBody)
		m_Env.Log(CLL_Error, sn.Location(),
			"break is not allowed inside a finally block");
}

void StatementResolveAccessor::Access(SnContinueStmt &sn)
{
	//v0.9.1: mirror of the break depth gate — continue additionally
	//requires a loop (a bare switch is breakable, not continuable).
	if (m_loopDepth == 0)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"continue statement outside of a loop");
		return;
	}
	if (m_inFinallyBody)
		m_Env.Log(CLL_Error, sn.Location(),
			"continue is not allowed inside a finally block");
}

} //namespace nlang
