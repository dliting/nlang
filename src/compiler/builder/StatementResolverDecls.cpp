/*---
    StatementResolverDecls.cpp — 声明类解析：函数形式参数、局部声明、枚举声明与成员值赋值。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"
#include <vector>

namespace nlang
{

//Phase 9c round 5: walk an expression subtree looking for any
//SnIdentifierExpr whose resolved Field() equals pTarget. Used by
//StatementResolver.Access(SnFunction) to detect that a default
//expression references a later formal parameter (which would
//compile-resolve but crash codegen's FindLocal in caller context).
//C++/C# likewise forbid forward references in default expressions.
static bool ReferencesFormal(SnExpression& expr, SnField* pTarget)
{
	if (expr.Kind() == NK_IdentifierExpr) {
		auto& id = static_cast<SnIdentifierExpr&>(expr);
		if (id.Field() == pTarget)
			return true;
	}
	for (auto& child : expr.Children()) {
		//Children of an SnExpression are always SyntaxNode subclasses
		//(SnExpression or SnFieldExpr in our case). IsExpression is on
		//SyntaxNode, not Node — cast then check.
		auto& synChild = static_cast<SyntaxNode&>(child);
		if (!synChild.IsExpression())
			continue;
		auto& childExpr = static_cast<SnExpression&>(synChild);
		if (ReferencesFormal(childExpr, pTarget))
			return true;
	}
	return false;
}

void StatementResolveAccessor::Access(SnFunction &sn)
{
	assert(m_pVisitor);
	m_pCurrType = &sn;

	//Phase 9c: enforce the parameter count sanity ceiling at
	//declaration time. The VM frame layout now sizes callParamBase
	//dynamically per caller, so there is no fixed 8-slot cap.
	//kMaxFuncParams is a sanity ceiling to prevent unreasonably
	//large frames.
	if (sn.Params().size() > kMaxFuncParams) {
		m_Env.Log(CLL_Error, sn.Location(),
			"function \"%s\" has %zu parameters; limit is %zu.",
			sn.Name().c_str(),
			sn.Params().size(),
			kMaxFuncParams);
	}

	//Formals are collected into a vector because ParamList exposes
	//begin/end iterators but no operator[]; the checks below index by
	//position (ResolveFunctionFormals detects default[i] referencing
	//formal[j] with j >= i).
	std::vector<SnFormalParam*> formals;
	for (auto& fp : sn.Params())
		formals.push_back(&fp);
	CheckFunctionNativeFormals(sn, formals);
	ResolveFunctionFormals(sn, formals);

	if (!sn.Body())
		return;

	for (auto &stmt : sn.Body()->Statements())
		stmt.Accept(*m_pVisitor);
}

//Phase 9f: native declarations are body-less by contract — the
//implementation lives in the host's registered table. A body would
//be silently ignored by GenerateFunction's native branch, so reject
//here. Out params are rejected too: writeback needs a callee frame
//and natives have none (the VM throws the same message at runtime).
//Phase 9e: out parameters must also be 4-byte scalar/class slots. A
//struct out param would need deep-copy writeback into the caller
//— unsupported in v1. Checked at declaration for clearer errors
//than at every call site.
void StatementResolveAccessor::CheckFunctionNativeFormals(SnFunction &sn,
	std::vector<SnFormalParam*> &formals)
{
	if (sn.ContainFlags(NF_Native)) {
		if (sn.Body()) {
			m_Env.Log(CLL_Error, sn.Location(),
				"native function \"%s\" cannot have a body; the "
				"implementation is host-provided.",
				sn.Name().c_str());
		}
		for (auto *param : formals) {
			if (param->ContainFlags(NF_Out)) {
				m_Env.Log(CLL_Error, param->Location(),
					"native function \"%s\" cannot have out parameters.",
					sn.Name().c_str());
			}
		}
	}
	for (auto *param : formals) {
		if (!param->ContainFlags(NF_Out))
			continue;
		auto *pT = param->EvalDataType();
		if (pT && pT->Kind() == NK_StructDecl) {
			m_Env.Log(CLL_Error, param->Location(),
				"out parameter \"%s\" cannot be a struct.",
				param->Name().c_str());
		}
	}
}

//Phase 9c: resolve formal param defaults in this function's scope.
//Earlier formals are in sn's local scope (as params) and become
//visible to later formals' defaults — e.g. `int b = a + 1` resolves
//`a` to formal[0]. Must run BEFORE body so default ASTs have their
//EvalDataType set when call sites are processed.
void StatementResolveAccessor::ResolveFunctionFormals(SnFunction &sn,
	std::vector<SnFormalParam*> &formals)
{
	for (size_t i = 0; i < formals.size(); ++i) {
		auto *param = formals[i];
		if (!param->Value())
			continue;
		if (!param->Value()->IsResolved())
			m_ExprResolver.Resolve(*param->Value(), sn, sn, ERF_None);

		//Phase 9c round 5: default[i] may only reference formals[0..i-1].
		if (param->Value()->IsResolved())
			CheckDefaultForwardRefs(formals, i);

		//Verify default's type is compatible with the formal's
		//declared type. Literals may already be IsResolved() from
		//parse time, so the check runs regardless of the resolve
		//above. Imported stubs are skipped: the stub formal's type
		//is a placeholder (int32) since CompiledFunction doesn't
		//carry per-formal type info; only the default expression
		//preserves the original type. R5-4 already skips call-site
		//type checks for imported callees; this declaration-time
		//check would falsely reject valid cross-module defaults
		//like string/null.
		if (param->Value()->IsResolved() && !sn.IsImported())
			CheckDefaultTypeCompat(param);
	}
}

//Phase 9c round 5: detect forward references in default expressions.
//Default[i] may only reference formals[0..i-1]. A reference to
//formal[j] (j >= i) would compile-resolve but crash codegen (FindLocal
//throws in caller context). C++/C# also forbid this. Walk default[i]'s
//subtree for any IdentifierExpr whose Field() is formal[j] (j >= i).
void StatementResolveAccessor::CheckDefaultForwardRefs(
	std::vector<SnFormalParam*> &formals, size_t i)
{
	auto *param = formals[i];
	for (size_t j = i; j < formals.size(); ++j) {
		auto *later_formal = formals[j];
		if (ReferencesFormal(*param->Value(), later_formal)) {
			m_Env.Log(CLL_Error,
				param->Value()->Location(),
				"default value for parameter \"%s\" references "
				"later parameter \"%s\"; defaults may only "
				"reference earlier parameters.",
				param->Name().c_str(),
				later_formal->Name().c_str());
			break;  //one error per default[i]
		}
	}
}

//Reporting an incompatible default at declaration gives clearer errors
//than at every call site that uses the default.
void StatementResolveAccessor::CheckDefaultTypeCompat(SnFormalParam *param)
{
	auto *pDefaultType = param->Value()->EvalDataType();
	auto *pFormalType = param->EvalDataType();
	if (pDefaultType && pFormalType) {
		auto ci = GetCastInfo(pDefaultType, pFormalType);
		if (ci.Kind() == TCK_None) {
			m_Env.Log(CLL_Error,
				param->Value()->Location(),
				"default value for parameter \"%s\" has "
				"incompatible type \"%s\"; expected \"%s\".",
				param->Name().c_str(),
				pDefaultType->ToString().c_str(),
				pFormalType->ToString().c_str());
		}
	}
}

//Walk up to the enclosing statement paragraph — the insertion point for
//locals and decomposed assigns. Callers bail when the walk fails. The
//walk deliberately does NOT restore anything on failure.
SnParagraph *StatementResolveAccessor::FindEnclosingParagraph(
	SyntaxNode *pNode)
{
	while (pNode)
	{
		if (pNode->Kind() == NK_Paragraph)
			return static_cast<SnParagraph *>(pNode);
		pNode = pNode->Parent();
	}
	return nullptr;
}

//Phase 9a: const locals must have an initializer.
void StatementResolveAccessor::RejectConstWithoutInit(SnLocalDeclStmt &sn)
{
	if (!sn.IsConst())
		return;
	for (auto &decl : sn.Decls())
	{
		if (!decl.pInitExpr)
			m_Env.Log(CLL_Error, sn.Location(),
				"const local must have an initializer.");
	}
}

//Build `x = init` for one declarator. The init expr is a child of sn
//since the container unification; hand its ownership over to the
//AssignStmt instead of letting both own it. The caller inserts and
//resolves the result.
SnAssignStmt *StatementResolveAccessor::BuildLocalInitAssign(
	SnLocalDeclStmt &sn, SnLocalDeclStmt::LocalDecl &decl)
{
	auto *pLeft = new SnIdentifierExpr(
		new std::string(decl.name), *sn.Location());
	return new SnAssignStmt(
		pLeft, sn.DetachChild(decl.pInitExpr), *sn.Location());
}

void StatementResolveAccessor::RegisterLocalDeclarators(SnLocalDeclStmt &sn,
	SnParagraph &paragraph, SnField *pTypeField, bool bIsArray)
{
	//Insert every decomposed AssignStmt before the ORIGINAL
	//successor of sn. Recomputing find(&sn) + 1 inside the
	//declarator loop would treat the assign just inserted as the
	//successor and place each later assign BEFORE the earlier
	//ones, reversing declarator order (int a = 2, b = a * 3
	//gave b = 0). std::list insertion keeps this iterator valid.
	auto iInsert = paragraph.Children().find(&sn);
	++iInsert;
	for (auto &decl : sn.Decls())
	{
		auto *pLocal = new SnLocalVar(decl.name, pTypeField,
			*sn.Location());
		if (bIsArray)
			pLocal->SetArrayType(true);
		paragraph.AddLocal(decl.name, pLocal);

		if (decl.pInitExpr)
		{
			auto *pAssign = BuildLocalInitAssign(sn, decl);
			paragraph.InsertChild(iInsert, pAssign);
			pAssign->Accept(*m_pVisitor);
			decl.pInitExpr = nullptr;
		}
		//Phase 9a: mark const AFTER init is resolved, so the
		//init AssignStmt doesn't trigger the "cannot assign to
		//const" check in Access(SnAssignStmt&).
		if (sn.IsConst())
			pLocal->AddFlags(NF_Const);
	}
}

void StatementResolveAccessor::Access(SnLocalDeclStmt &sn)
{
	assert(m_pVisitor);
	//Resolve the type expression via ExprResolver, not the
	//StatementResolver visitor (which doesn't have Access for type
	//expressions). This is critical for SnArrayTypeExpr which would
	//hit the empty Access(SnArrayTypeExpr&) and never resolve.
	m_ExprResolver.Resolve(*sn.Type(), *sn.Parent(), *m_pCurrType, ERF_None);
	if (!sn.Type()->IsResolved())
		return;

	//Array redesign B: jagged declarations (T[][]) have no VM
	//layout and used to degrade silently — reject here.
	if (ArrayTypeDepth(sn.Type()) >= 2)
		m_Env.Log(CLL_Error, sn.Location(),
			"jagged arrays (T[][]) are not supported");

	auto *pParagraph = FindEnclosingParagraph(sn.Parent());
	if (!pParagraph)
		return;
	//An unbraced control-flow body (the grammar accepts a bare
	//Statement under if/while/do/for) makes sn a child of the
	//control node, not of the paragraph. The insertion anchor below
	//would then be an end()-derived position and the decomposed
	//AssignStmts would land at the top of the paragraph as silently
	//dead code (the locals are only allocated where the decl itself
	//is emitted, inside the branch). Reject loudly instead of
	//emitting wrong code.
	if (sn.Parent() != pParagraph)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"a local declaration cannot be the unbraced body of a "
			"control-flow statement; use braces");
		return;
	}

	auto *pTypeField = sn.Type()->Field();
	auto bIsArray = sn.Type()->IsArrayType();
	RejectConstWithoutInit(sn);
	RegisterLocalDeclarators(sn, *pParagraph, pTypeField, bIsArray);
}

//D4/D5 declaration-side rejections for enum methods: toString is
//reserved for the built-in conversion, bodyless methods are silently
//skipped everywhere else, and defaults/out params would need call
//paths enum calls don't have (Callee() stays null).
void StatementResolveAccessor::RejectIllegalEnumMethod(SnFunction &method)
{
	//D5: toString on enums is the built-in OP_Enum_to_str
	//dispatch (Phase 8e-9b); a user method of that name would be
	//silently shadowed by it, so reject at declaration.
	if (method.Name() == "toString")
		m_Env.Log(CLL_Error, method.Location(),
			"enum method cannot be named \"toString\"; the name is "
			"reserved for the built-in conversion.");
	//D4: bodyless methods are rejected here — Access(SnFunction)
	//silently returns on !Body(), and RegisterFunctions skips
	//them too, so a declaration-side check is the only site that
	//reports.
	if (method.ContainFlags(NF_Abstract) || !method.Body())
		m_Env.Log(CLL_Error, method.Location(),
			"enum method \"%s\" must have a body.",
			method.Name().c_str());
	for (auto &param : method.Params())
	{
		//D4: default values need the callee-bound call path
		//(default fill + walker default-depth); enum method calls
		//keep Callee() null, so a default would silently read the
		//adjacent frame slot as garbage (plan 12b r3 M1).
		if (param.Value())
			m_Env.Log(CLL_Error, param.Location(),
				"enum method \"%s\" cannot have default parameter "
				"values.", method.Name().c_str());
		//D4: out parameters need the writeback path of
		//OP_CallMethodDirect's bound form; same Callee-null
		//limitation as above.
		if (param.ContainFlags(NF_Out))
			m_Env.Log(CLL_Error, param.Location(),
				"enum method \"%s\" cannot have out parameters.",
				method.Name().c_str());
	}
}

void StatementResolveAccessor::Access(SnEnumDecl &sn)
{
	assert(m_pVisitor);
	//Unconditional: the flag is NOT a reliable "values assigned" signal
	//here — ResolveDataTypes flags enum decls early without assigning
	//values (Step 1 finding), and the pre-pass already ran, so this is
	//an idempotent re-assignment.
	AssignEnumMemberValues(sn);
	/*
	Method bodies resolve AFTER the decl is flagged NF_Resolved —
	deliberately diverging from the class path (bodies first, flag
	last). The D8 duplicate-label defense gate in switch statements
	reads member values only from a resolved enum decl; resolving
	method bodies first would make `case Color.A, Color.A:` inside a
	method skip value extraction (plan 12b r4 MINOR-1).
	*/
	for (auto &method : sn.Methods())
	{
		RejectIllegalEnumMethod(method);
		method.Accept(*m_pVisitor);
	}
}

//Assign sequential/explicit values to members and flag the decl
//resolved. Shared by the document-order Access and the pre-pass
//(PreAssignEnumMemberValues), which must NOT resolve method bodies —
//that would double-resolve them once Access runs.
void StatementResolveAccessor::AssignEnumMemberValues(SnEnumDecl &sn)
{
	assert(m_pVisitor);
	int32_t nextValue = 0;
	for (auto &member : sn.Members())
	{
		if (member.ValueExpr())
		{
			member.ValueExpr()->Accept(*m_pVisitor);
			if (member.ValueExpr()->IsResolved()
				&& member.ValueExpr()->EvalDataType()
				&& member.ValueExpr()->EvalDataType()->Kind() == NK_Int32)
			{
				auto *pLit = dynamic_cast<SnLiteralExpr*>(member.ValueExpr());
				if (pLit)
				{
					nextValue = pLit->Value().Get<int32_t>();
					member.SetValue(nextValue);
					nextValue++;
				}
			}
		}
		else
		{
			member.SetValue(nextValue);
			nextValue++;
		}
	}
	sn.AddFlags(NF_Resolved);
}

void StatementResolveAccessor::Access(SnEnumMember &sn)
{
}

} //namespace nlang
