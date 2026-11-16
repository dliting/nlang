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

	//0.13.1b: the type-parameter cap rides the same declaration pass
	//(class side: CheckGenericDeclContract; free-function side: here).
	CheckGenericTypeParamCount(sn.TypeParamNames(), sn.Name(),
		sn.Location(), m_Env);

	CheckCtorDeclarationContract(sn);
	CheckFunctionModifierContract(sn);

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

//v0.13.0 D5: NF_Const on a function has no meaning — functions are not
//rebindable values, and const-aggregate immutability is a type-level
//concept (const class/struct). Before this contract the public spelling
//compiled and ran; the bare top-level spelling died in the parser with
//a generic syntax error (now named by the grammar's TopItem arms).
void StatementResolveAccessor::CheckFunctionModifierContract(SnFunction &sn)
{
	if (sn.ContainFlags(NF_Const)) {
		m_Env.Log(CLL_Error, sn.Location(),
			"const is not allowed on functions; declare it without const.");
	}
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
		//0.13.1a: the host ABI binds one fixed shape per function
		//name — a generic surface would route every inferred type
		//argument to that one shape with no descriptor pairing, so
		//the declaration is rejected instead of failing per call.
		if (sn.IsGenericDeclaration()) {
			m_Env.Log(CLL_Error, sn.Location(),
				"native function \"%s\" cannot declare type "
				"parameters; the host ABI has one fixed shape "
				"per function.",
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
		//v0.13.9 phase 3: propagate the & mark onto the param descriptor
		//(same convention as MarkLocalDeclFlags) so the binding gates
		//see a reference formal without re-reading its type node.
		if (param->Type() && param->Type()->ContainFlags(NF_Ref))
			param->AddFlags(NF_Ref);
		//v0.13.9 phase 3: a reference parameter binds caller storage; a
		//default value has nothing to bind (spec 3.9).
		if (param->Value() && param->Type()
			&& param->Type()->ContainFlags(NF_Ref))
		{
			m_Env.Log(CLL_Error, param->Location(),
				"a reference parameter cannot have a default value.");
			continue;
		}
		if (!param->Value())
			continue;
		ResolveFormalDefault(sn, formals, i);
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
	//v0.13.9 phase 3 Task 4: a & declarator's decomposed init is the
	//binding itself — the flag discriminates it from later write-through
	//stores through the same reference (the emit-side binding arm keys
	//on it exactly like the static once-guard above).
	if (sn.Type() && sn.Type()->ContainFlags(NF_Ref))
		pAssign->AddFlags(NF_Generated);
	return pAssign;
}

//v0.13.9 phase 3 split (source-size guard): resolve one init
//declarator's decomposition — build, insert, resolve, then judge the
//&-binding against the RESOLVED initializer. The judgment runs after
//decomposition so the init expression resolves exactly once, and the
//local registers before this resolves, so a self-binding (`Point& a
//= a;`) resolves and the reference-target gate catches it.
void StatementResolveAccessor::ResolveInitDeclarator(SnLocalDeclStmt &sn,
	SnLocalDeclStmt::LocalDecl &decl, SnParagraph &paragraph,
	NodeIterator iInsert, SnField *pTypeField)
{
	auto *pAssign = BuildLocalInitAssign(sn, decl);
	paragraph.InsertChild(iInsert, pAssign);
	pAssign->Accept(*m_pVisitor);
	decl.pInitExpr = nullptr;
	if (sn.Type()->ContainFlags(NF_Ref) && pAssign->Right())
	{
		if (!RejectRefBindTarget(*pAssign->Right(), pTypeField,
				sn.Location()))
			MarkPromotedBindTarget(*pAssign->Right());
	}
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
	//v0.13.9 phase 3 Task 4: hand the descriptor back through the decl
	//so codegen can read its NF_Promoted flag.
	decl.pNode = pLocal;
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
	//v0.13.9 phase 3: the & mark rides the type expression node, so a
	//reference local's descriptor would look like a plain value slot to
	//the storage gates (int& r2 = r would pass as int-to-int). Copy the
	//mark onto the descriptor — the same propagation NF_Const/NF_Static
	//already do.
	if (sn.Type() && sn.Type()->ContainFlags(NF_Ref))
		local.AddFlags(NF_Ref);
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
			ResolveInitDeclarator(sn, decl, paragraph, iInsert,
				pTypeField);
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

//v0.13.0 D5: true when the paragraph is the script's top level. By
//resolve time SynthesizeScriptEntries has wrapped script-mode top-level
//statements in a synthesized main, so the test walks to the nearest
//function and checks its NF_ScriptEntry marker (compile-only flag); no
//function ancestor at all is also top level.
static bool ParagraphIsTopLevel(const SnParagraph &paragraph)
{
	const SyntaxNode *pNode = paragraph.Parent();
	while (pNode)
	{
		if (pNode->Kind() == NK_Function)
			return pNode->ContainFlags(NF_ScriptEntry);
		pNode = pNode->Parent();
	}
	return true;
}

//v0.13.0 D5: the zero-argument semi form `const T f();` rides the
//statement path at top level (the grammar's TopItem diagnostic arms
//catch the param/body spellings and the comma-continuation tail is
//reached only through this shape) — name it here. A class/struct head
//keeps the legal 0.9.15 zero-argument construction reading (pinned by
//top_level_const_zero_arg_ctor); only types that can never be call-
//constructed (scalars, string, array, enum, interface) reject. True =
//rejected; the caller stops so the declarator walk adds no noise.
static bool RejectTopLevelConstFnShape(SnLocalDeclStmt &sn,
	const SnParagraph &paragraph, SnField *pTypeField,
	BuildEnvironment &env)
{
	if (!sn.IsConst() || !ParagraphIsTopLevel(paragraph) || !pTypeField)
		return false;
	NodeKind headKind = pTypeField->Kind();
	if (headKind == NK_ClassDecl || headKind == NK_StructDecl)
		return false;
	for (auto &decl : sn.Decls())
	{
		if (decl.pCtorArgs && decl.pCtorArgs->empty())
		{
			env.Log(CLL_Error, sn.Location(),
				"const is not allowed on functions; declare it without const.");
			return true;
		}
	}
	return false;
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

	//Array redesign B: jagged declarations (T[][]) have no VM layout.
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

	//v0.13.9 phase 3: a &-marked type routes through the reference
	//contract — declarator shapes here, the init judgment rides the
	//decomposition in RegisterLocalDeclarators.
	if (sn.Type()->ContainFlags(NF_Ref) && RejectRefDeclaratorShapes(sn))
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
	if (RejectTopLevelConstFnShape(sn, *pParagraph, pTypeField, m_Env))
		return;
	RegisterLocalDeclarators(sn, *pParagraph, pTypeField, bIsArray,
		pSizedElemField);
}
} //namespace nlang
