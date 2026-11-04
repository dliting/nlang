/*---
    StatementResolverDecls.cpp — 声明类解析：函数形式参数、局部声明分解与默认值检查。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"
#include "ExprResolverCastFit.hpp"
#include "ModuleBuilderChecks.h"
#include <vector>
#include <nlang/runtime/StackDump.h>

namespace nlang
{

//Phase 9c round 5 / P3: the default-expression subtree walkers
//(ReferencesFormal, ReferencesThis) moved with the default contract
//family to StatementResolverDeclDefaults.cpp (2026-11-02 size guard).

void StatementResolveAccessor::Access(SnFunction &sn)
{
	NLANG_ASSERT(m_pVisitor);
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

	CheckCtorDeclarationContract(sn);

	//Formals are collected into a vector because ParamList exposes
	//begin/end iterators but no operator[]; the checks below index by
	//position (ResolveFunctionFormals detects default[i] referencing
	//formal[j] with j >= i).
	std::vector<SnFormalParam*> formals;
	for (auto& fp : sn.Params())
		formals.push_back(&fp);
	//0.9.0 semantic restriction: parameter names may not reuse visible
	//type names. Imported stubs carry synthesized p0..pN names that were
	//never user-written — skip them.
	if (!sn.IsImported()) {
		for (auto *param : formals)
			RejectTypeNameVariable(param->Name(), sn, sn,
				param->Location());
	}
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

		//P3 review: a ctor default cannot touch `this` — imported stubs
		//skip like CheckDefaultTypeCompat (the producer unit already
		//validated its own defaults).
		if (param->Value()->IsResolved() && !sn.IsImported())
			CheckCtorDefaultThisRefs(sn, *param);

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

//Phase 9c round 5 / P3 / 0.7.5: the formal-default contract family
//(forward refs, ctor `this` refs, constant fit, type compat) lives in
//StatementResolverDeclDefaults.cpp (2026-11-02 source-size guard).

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
		//0.9.0: ctor args (and, from Task 7, an array size) count as
		//initialized — `const Point p(2, 3);` is a legal const init.
		if (!decl.pInitExpr && !decl.pCtorArgs && !decl.pArraySize)
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
	auto *pAssign = new SnAssignStmt(
		pLeft, sn.DetachChild(decl.pInitExpr), *sn.Location());
	//v0.12.6 Task 7: a static local's decomposed init is the
	//declaration itself — the flag marks it so codegen wraps the
	//once-guard (D2/D10) around exactly this store; user stores
	//never carry it.
	if (sn.NodeFlags() & NF_Static)
		pAssign->AddFlags(NF_Generated);
	return pAssign;
}

//0.9.0: BuildCtorInitAssign — the `T name(args);` synthesis — lives in
//StatementResolverDeclCtor.cpp (per-concern TU split, source-size guard).
//Enum declaration resolving (member values, enum-method rejections)
//likewise lives in StatementResolverDeclEnum.cpp.

//First-wins AddLocal would leave a duplicate declarator's references
//statically bound to the earlier declaration (masked only by layout).
static void RejectDuplicateDeclarator(BuildEnvironment &env,
	SnParagraph &paragraph, const std::string &name, SnLocalVar *pLocal,
	const ISourceLocation *pLoc)
{
	if (!paragraph.AddLocal(name, pLocal))
		env.Log(CLL_Error, pLoc,
			"local '%s' is already declared in this scope", name.c_str());
}

//0.10.7 D2: a size payload types only its own declarator; the
//interned array token is that declarator's field (the statement
//type stays the head type for every declarator).
SnLocalVar *StatementResolveAccessor::CreateDeclaratorLocal(
	SnLocalDeclStmt::LocalDecl &decl, const ISourceLocation &loc,
	SnParagraph &paragraph, SnField *pTypeField, bool bIsArray,
	SnField *pSizedArrayField)
{
	SnField *pDeclField = (pSizedArrayField && decl.pArraySize)
		? pSizedArrayField : pTypeField;
	auto *pLocal = new SnLocalVar(decl.name, pDeclField, loc);
	//0.9.0/0.10.7: bare [...] inference and member receivers key on
	//the array flag — a head-array statement flags every
	//declarator, a size payload flags only its own.
	if (bIsArray || (pSizedArrayField && decl.pArraySize))
		pLocal->SetArrayType(true);
	if (pSizedArrayField && decl.pArraySize)
		decl.bArrayType = true;
	RejectDuplicateDeclarator(m_Env, paragraph, decl.name, pLocal, &loc);
	return pLocal;
}

//Post-init flag marking for one declarator local: const lands only
//AFTER the init AssignStmt resolved (marking earlier trips the
//"cannot assign to const" check in Access(SnAssignStmt&)); v0.12.6
//static locals carry the flag onto the descriptor (the backend rejects
//uses until slot-backed storage lands with the static-local tasks).
static void MarkLocalDeclFlags(const SnLocalDeclStmt &sn, SnField &local)
{
	if (sn.IsConst())
		local.AddFlags(NF_Const);
	if (sn.NodeFlags() & NF_Static)
		local.AddFlags(NF_Static);
}

void StatementResolveAccessor::RegisterLocalDeclarators(SnLocalDeclStmt &sn,
	SnParagraph &paragraph, SnField *pTypeField, bool bIsArray,
	SnField *pSizedElemField)
{
	//v0.12.3: a sized T declarator is array CREATION (the statement form
	//of new T[3]) — the spec D2 creation family rejects it; T[] TYPE
	//positions stay legal (handle slots).
	if (pSizedElemField && RejectTypeParamArrayElement(pSizedElemField,
			m_Env, sn.Location()))
		return;

	//Insert every decomposed AssignStmt before the ORIGINAL successor
	//of sn — recomputing find(&sn) + 1 inside the loop would reverse
	//declarator order (int a = 2, b = a * 3 gave b = 0). std::list
	//insertion keeps this iterator valid.
	auto iInsert = paragraph.Children().find(&sn);
	++iInsert;
	//0.10.7 D2: the interned array token types every sized declarator
	//(CreateDeclaratorLocal); the statement type stays the head type.
	SnField *pSizedArrayField = pSizedElemField
		? m_Env.InternArrayTypeToken(pSizedElemField) : nullptr;
	for (auto &decl : sn.Decls())
	{
		RejectTypeNameVariable(decl.name, paragraph, *m_pCurrType,
			sn.Location());
		auto *pLocal = CreateDeclaratorLocal(decl, *sn.Location(),
			paragraph, pTypeField, bIsArray, pSizedArrayField);

		//Each payload shape delegates to its concern TU: the
		//decomposition is built, inserted at iInsert and resolved
		//there; the consumed slots never reach codegen.
		if (decl.pCtorArgs)
			SynthesizeCtorInitDeclarator(sn, decl, paragraph, iInsert);
		else if (decl.pArraySize)
			SynthesizeSizedArrayDeclarator(sn, decl, paragraph,
				iInsert, pSizedElemField);
		else if (decl.pInitExpr)
		{
			auto *pAssign = BuildLocalInitAssign(sn, decl);
			paragraph.InsertChild(iInsert, pAssign);
			pAssign->Accept(*m_pVisitor);
			decl.pInitExpr = nullptr;
		}
		MarkLocalDeclFlags(sn, *pLocal);
	}
}

//An unbraced control-flow body (the grammar accepts a bare Statement
//under if/while/do/for) makes the declaration a child of the control
//node, not of the paragraph. The insertion anchor of the declarator
//decomposition would then be an end()-derived position and the
//decomposed AssignStmts would land at the top of the paragraph as
//silently dead code (the locals are only allocated where the decl
//itself is emitted, inside the branch). Reject loudly instead of
//emitting wrong code.
bool StatementResolveAccessor::RejectUnbracedControlFlowDecl(
	SnLocalDeclStmt &sn, SnParagraph *pParagraph)
{
	if (sn.Parent() == pParagraph)
		return false;
	m_Env.Log(CLL_Error, sn.Location(),
		"a local declaration cannot be the unbraced body of a "
		"control-flow statement; use braces");
	return true;
}

void StatementResolveAccessor::Access(SnLocalDeclStmt &sn)
{
	NLANG_ASSERT(m_pVisitor);
	//Resolve the type expression via ExprResolver, not the
	//StatementResolver visitor (which doesn't have Access for type
	//expressions). This is critical for SnArrayTypeExpr which would
	//hit the empty Access(SnArrayTypeExpr&) and never resolve.
	m_ExprResolver.Resolve(*sn.Type(), *sn.Parent(), *m_pCurrType, ERF_None);
	if (!sn.Type()->IsResolved())
		return;
	//v0.12.2 face gate: face classes are not usable types.
	if (RejectPreludeFaceUse(m_Env, sn.Type()->Field(),
		sn.Type()->Location(), "used as a type"))
		return;

	//Array redesign B: jagged declarations (T[][]) have no VM
	//layout and used to degrade silently — reject here.
	if (ArrayTypeDepth(sn.Type()) >= 2)
		m_Env.Log(CLL_Error, sn.Location(),
			"jagged arrays (T[][]) are not supported");

	//0.9.0/0.10.7: the sized-declarator typing (jagged reject, element
	//capture) lives in StatementResolverDeclArray.cpp.
	auto *pSizedElemField = BindSizedArrayDeclType(sn);

	auto *pParagraph = FindEnclosingParagraph(sn.Parent());
	if (!pParagraph)
		return;
	if (RejectUnbracedControlFlowDecl(sn, pParagraph))
		return;

	auto *pTypeField = sn.Type()->Field();
	auto bIsArray = sn.Type()->IsArrayType();
	if (sn.NodeFlags() & NF_Static)
	{
		RejectStaticLocalPlacement(sn);
		RejectStaticLocalTypeContract(sn);
	}
	else
		RejectConstWithoutInit(sn);
	RegisterLocalDeclarators(sn, *pParagraph, pTypeField, bIsArray,
		pSizedElemField);
}
} //namespace nlang
