/*---
    StatementResolverDecls.cpp — 声明类解析：函数形式参数、局部声明分解与默认值检查。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"
#include "ExprResolverCastFit.hpp"
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

//P3 review: walk an expression subtree looking for a `this` reference —
//an explicit SnThisExpr, or a bare identifier that resolved to a class
//field (the implicit this.field binding; NK_ClassField distinguishes it
//from formals/locals, and bare names cannot reach another class's
//fields). Same traversal contract as ReferencesFormal above.
static bool ReferencesThis(SnExpression& expr)
{
	if (expr.Kind() == NK_ThisExpr)
		return true;
	if (expr.Kind() == NK_IdentifierExpr) {
		auto& id = static_cast<SnIdentifierExpr&>(expr);
		auto* pField = id.Field();
		if (pField && pField->Kind() == NK_ClassField)
			return true;
	}
	for (auto& child : expr.Children()) {
		auto& synChild = static_cast<SyntaxNode&>(child);
		if (!synChild.IsExpression())
			continue;
		if (ReferencesThis(static_cast<SnExpression&>(synChild)))
			return true;
	}
	return false;
}

//P3 review: true when sn is a constructor — a member function of a class
//whose name equals the class name (the same declaration-shape rule
//FindCtorParamRange applies to class members).
static bool IsConstructorFunction(SnFunction& sn)
{
	auto* pParent = sn.Parent();
	return pParent && pParent->Kind() == NK_ClassDecl
		&& sn.Name() == static_cast<SnClassDecl*>(pParent)->Name();
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

//P3 review: reject a ctor default that references `this` (explicitly, or
//implicitly through a bare field). Defaults are emitted and evaluated at
//the CALL SITE: a construction call stages them before the object exists
//(thisSlot is UINT16_MAX there — NK_ThisExpr would fall back to the
//caller's local 0), and a super() call binds the derived object whose
//fields are not initialized yet. Either way the default reads the wrong
//object or crashes, so the declaration is the rejection point. Method
//and free-function defaults keep their existing behavior.
void StatementResolveAccessor::CheckCtorDefaultThisRefs(SnFunction &sn,
	SnFormalParam &param)
{
	if (!IsConstructorFunction(sn))
		return;
	if (!ReferencesThis(*param.Value()))
		return;
	m_Env.Log(CLL_Error,
		param.Value()->Location(),
		"default value for parameter \"%s\" of constructor \"%s\" "
		"cannot reference 'this' (explicitly or through a bare "
		"field); constructor defaults evaluate at the call site, "
		"where no valid 'this' exists.",
		param.Name().c_str(),
		sn.Name().c_str());
}

//0.7.5: an explicit-only default must clear the same constant-fit bar
//as an assignment — `float x = 3.14` is an error exactly where
//`float f = 3.14;` is (the double literal is not float-representable),
//while `float x = 0.5` passes and the fill-side EmitScalarSlotCast
//(VmBackend::EmitBinding) normalizes the staged slot to the formal's
//kind. Without the gate the double default's low bytes reached the
//callee as the float's bits.
void StatementResolveAccessor::CheckDefaultConstantFit(
	SnFormalParam *param, SnField *pFormalType)
{
	int ri = ScalarPrimIndexOf(pFormalType->Kind());
	ConstantFitOutcome fit;
	if (ri >= 0)
		fit = TryConstantFit(*param->Value(), kScalarPrims[ri]);
	if (fit.verdict == CF_Fits)
		return;
	if (fit.verdict == CF_OutOfRange)
		m_Env.Log(CLL_Error,
			param->Value()->Location(),
			"default value for parameter \"%s\": "
			"constant %s out of range for '%s'",
			param->Name().c_str(), fit.constantText,
			kScalarPrims[ri].name);
	else
		m_Env.Log(CLL_Error,
			param->Value()->Location(),
			"default value for parameter \"%s\" "
			"requires an explicit conversion to \"%s\".",
			param->Name().c_str(),
			pFormalType->ToString().c_str());
}

//Reporting an incompatible default at declaration gives clearer errors
//than at every call site that uses the default.
void StatementResolveAccessor::CheckDefaultTypeCompat(SnFormalParam *param)
{
	auto *pDefaultType = param->Value()->EvalDataType();
	auto *pFormalType = param->EvalDataType();
	if (!pDefaultType || !pFormalType)
		return;
	auto ci = GetCastInfo(pDefaultType, pFormalType);
	if (ci.Kind() == TCK_None) {
		m_Env.Log(CLL_Error,
			param->Value()->Location(),
			"default value for parameter \"%s\" has "
			"incompatible type \"%s\"; expected \"%s\".",
			param->Name().c_str(),
			pDefaultType->ToString().c_str(),
			pFormalType->ToString().c_str());
		return;
	}
	if (ci.Kind() == TCK_Explicit)
		CheckDefaultConstantFit(param, pFormalType);
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
	return new SnAssignStmt(
		pLeft, sn.DetachChild(decl.pInitExpr), *sn.Location());
}

//0.9.0: BuildCtorInitAssign — the `T name(args);` synthesis — lives in
//StatementResolverDeclCtor.cpp (per-concern TU split, source-size guard).
//Enum declaration resolving (member values, enum-method rejections)
//likewise lives in StatementResolverDeclEnum.cpp.

void StatementResolveAccessor::RegisterLocalDeclarators(SnLocalDeclStmt &sn,
	SnParagraph &paragraph, SnField *pTypeField, bool bIsArray,
	SnField *pSizedElemField)
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
		RejectTypeNameVariable(decl.name, paragraph, *m_pCurrType,
			sn.Location());
		auto *pLocal = new SnLocalVar(decl.name, pTypeField,
			*sn.Location());
		//0.9.0: a sized statement types EVERY declarator as an array
		//(re-bind is statement-level, so is the flag) — bare [...]
		//inference and member receivers key on it.
		if (bIsArray || pSizedElemField)
			pLocal->SetArrayType(true);
		paragraph.AddLocal(decl.name, pLocal);

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

	//0.9.0: the sized-declarator type re-shape (token re-bind, jagged
	//reject, element capture) lives in StatementResolverDeclArray.cpp.
	auto *pSizedElemField = BindSizedArrayDeclType(sn);

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
	RegisterLocalDeclarators(sn, *pParagraph, pTypeField, bIsArray,
		pSizedElemField);
}
} //namespace nlang
