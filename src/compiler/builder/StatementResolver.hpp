#pragma once
#include "BuildEnvironment.h"
#include "SyntaxNodeVisitor.h"
#include "CastInfo.h"
#include "ExprResolver.h"
#include "SnStatements.h"
#include "SnData.h"
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

class StatementResolveAccessor
{
public:
	explicit StatementResolveAccessor(BuildEnvironment &env) :
		m_Env(env), m_pCurrType(nullptr), m_pVisitor(nullptr),
		m_ExprResolver(m_Env)
	{
	}

	void Visitor(ISyntaxNodeVisitor *pVisitor)
	{
		m_pVisitor = pVisitor;
	}

	void Access(SnNamespace &sn)
	{
		assert(m_pVisitor);
		for (auto& field : sn.Members())
		{
			if (CanBeFuncParentEx(field.Kind()) || field.Kind() == NK_Function || field.Kind() == NK_EnumDecl || field.Kind() == NK_StructDecl || field.Kind() == NK_ClassDecl)
				field.Accept(*m_pVisitor);
		}
	}

	void Access(SnFunction &sn)
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

		//Phase 9c: resolve formal param defaults in this function's scope.
		//Earlier formals are in sn's local scope (as params) and become
		//visible to later formals' defaults — e.g. `int b = a + 1` resolves
		//`a` to formal[0]. Must run BEFORE body so default ASTs have their
		//EvalDataType set when call sites are processed.
		//
		//Phase 9c round 5: also collect formals into a vector so we can
		//detect forward references (default[i] referencing formal[j] with
		//j >= i). ParamList exposes begin/end iterators but no operator[].
		std::vector<SnFormalParam*> formals;
		for (auto& fp : sn.Params())
			formals.push_back(&fp);
		for (size_t i = 0; i < formals.size(); ++i) {
			auto *param = formals[i];
			if (!param->Value())
				continue;
			if (!param->Value()->IsResolved())
				m_ExprResolver.Resolve(*param->Value(), sn, sn, ERF_None);

			//Phase 9c round 5: detect forward references in default
			//expressions. Default[i] may only reference formals[0..i-1].
			//A reference to formal[j] (j >= i) would compile-resolve but
			//crash codegen (FindLocal throws in caller context). C++/C#
			//also forbid this. Walk default[i]'s subtree for any
			//IdentifierExpr whose Field() is formal[j] (j >= i).
			if (param->Value()->IsResolved()) {
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

			//Verify default's type is compatible with the formal's
			//declared type. Reporting at declaration gives clearer
			//errors than at every call site that uses the default.
			//Note: literals may already be IsResolved() from parse
			//time, so the type check must run regardless.
			//Option B: skip for imported stubs. The stub formal's type is a
			//placeholder (int32) since CompiledFunction doesn't carry
			//per-formal type info; only the default expression preserves
			//the original type. R5-4 already skips call-site type checks
			//for imported callees; this declaration-time check would
			//falsely reject valid cross-module defaults like string/null.
			if (param->Value()->IsResolved() && !sn.IsImported()) {
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
		}

		if (!sn.Body())
			return;

		for (auto &stmt : sn.Body()->Statements())
			stmt.Accept(*m_pVisitor);
	}

	void Access(SnField &sn)
	{
		m_pCurrType = nullptr;
	}

	void Access(SnReturnStmt &sn)
	{
		assert(m_pVisitor);
		assert(m_pCurrType && m_pCurrType->Kind() == NK_Function);
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

		if (!pReturnType->IsResolved())
			return;
		auto pSourceType = pResultExpr->EvalDataType();
		auto pTargetType = pOuterFunc->EvalDataType();
		auto castInfo = GetCastInfo(pSourceType, pTargetType);
		auto iExpr = sn.Children().find(sn.m_pResult);
		if (m_ExprResolver.FixupExprType(iExpr, castInfo))
			sn.m_pResult = &static_cast<SnCastExpr &>(*iExpr);
	}

	void Access(SnExpression &sn)
	{
		if (sn.IsResolved())
			return;
		assert(m_pCurrType);
		assert(sn.Parent());
		m_ExprResolver.Resolve(sn, *sn.Parent(), *m_pCurrType, ERF_None);
	}

	void Access(SnLocalDeclStmt &sn)
	{
		assert(m_pVisitor);
		//Resolve the type expression via ExprResolver, not the
		//StatementResolver visitor (which doesn't have Access for type
		//expressions). This is critical for SnArrayTypeExpr which would
		//hit the empty Access(SnArrayTypeExpr&) and never resolve.
		m_ExprResolver.Resolve(*sn.Type(), *sn.Parent(), *m_pCurrType, ERF_None);
		if (!sn.Type()->IsResolved())
			return;

		auto pParent = sn.Parent();
		SnParagraph *pParagraph = nullptr;
		while (pParent)
		{
			if (pParent->Kind() == NK_Paragraph)
			{
				pParagraph = static_cast<SnParagraph *>(pParent);
				break;
			}
			pParent = pParent->Parent();
		}
		if (!pParagraph)
			return;

		auto *pTypeField = sn.Type()->Field();
		auto bIsArray = sn.Type()->IsArrayType();
		//Phase 9a: const locals must have an initializer.
		if (sn.IsConst())
		{
			for (auto &decl : sn.Decls())
			{
				if (!decl.pInitExpr)
				{
					m_Env.Log(CLL_Error, sn.Location(),
						"const local must have an initializer.");
				}
			}
		}
		for (auto &decl : sn.Decls())
		{
			auto *pLocal = new SnLocalVar(decl.name, pTypeField,
				*sn.Location());
			if (bIsArray)
				pLocal->SetArrayType(true);
			pParagraph->AddLocal(decl.name, pLocal);

			if (decl.pInitExpr)
			{
				auto *pLeft = new SnIdentifierExpr(
					new std::string(decl.name), *sn.Location());
				auto *pAssign = new SnAssignStmt(
					pLeft, decl.pInitExpr, *sn.Location());

				auto iPos = pParagraph->Children().find(&sn);
				++iPos;
				pParagraph->InsertChild(iPos, pAssign);

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

	void Access(SnAssignStmt &sn)
	{
		if (sn.IsResolved())
			return;
		assert(m_pVisitor);
		sn.Left()->Accept(*m_pVisitor);

		//Phase 9a: reject assignment to a const local.
		//(The const-init decomposition in Access(SnLocalDeclStmt&) sets
		//NF_Const AFTER resolving the initializer AssignStmt, so this
		//check correctly skips the init assignment.)
		if (sn.Left()->Kind() == NK_IdentifierExpr)
		{
			auto* pLeftField = static_cast<SnIdentifierExpr&>(
				*sn.Left()).Field();
			if (pLeftField && pLeftField->ContainFlags(NF_Const))
			{
				m_Env.Log(CLL_Error, sn.Location(),
					"cannot assign to const local \"%s\".",
					pLeftField->Name().c_str());
			}
		}

		//Phase 8e-6: propagate LHS type to bare init list RHS before
		//resolving, so the init list knows its target type. Only needed
		//for the bare `[...]` form (ExplicitType is null).
		//We pass the LHS variable itself (not its EvalDataType) because
		//for array variables, EvalDataType returns the element type and
		//loses the array-ness flag — codegen needs IsArrayType() on the
		//variable to detect the array case.
		if (sn.Right()->Kind() == NK_InitListExpr)
		{
			auto& initList = static_cast<SnInitListExpr&>(*sn.Right());
			if (!initList.ExplicitType() && !initList.InferredTarget())
			{
				SnField* pLeftField = nullptr;
				if (sn.Left()->Kind() == NK_IdentifierExpr)
				{
					pLeftField = static_cast<SnIdentifierExpr&>(
						*sn.Left()).Field();
				}
				if (pLeftField)
					initList.InferredTarget(pLeftField);
			}
		}

		sn.Right()->Accept(*m_pVisitor);

		//Init lists set their own EvalDataType and don't go through the
		//cast-info path — skip cast fixup for them.
		if (sn.Right()->Kind() == NK_InitListExpr)
		{
			if (sn.Right()->IsResolved())
				sn.AddFlags(NF_Resolved);
			return;
		}

		SnField* pTargetType = nullptr;
		if (sn.Left()->Kind() == NK_IdentifierExpr)
		{
			auto* pLeftField = static_cast<SnIdentifierExpr&>(*sn.Left()).Field();
			if (!pLeftField)
				return;
			pTargetType = pLeftField->EvalDataType();
		}
		else if (sn.Left()->Kind() == NK_MemberExpr)
		{
			pTargetType = sn.Left()->EvalDataType();
		}
		auto* pSourceType = sn.Right()->EvalDataType();
		if (!pTargetType || !pSourceType)
			return;
		auto castInfo = GetCastInfo(pSourceType, pTargetType);
		auto iExpr = sn.Children().find(sn.m_pRight);
		if (m_ExprResolver.FixupExprType(iExpr, castInfo))
			sn.m_pRight = &static_cast<SnCastExpr &>(*iExpr);
		sn.AddFlags(NF_Resolved);
	}

	void Access(SnIfStmt &sn)
	{
		assert(m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		sn.ThenStmt()->Accept(*m_pVisitor);
		if (sn.ElseStmt())
			sn.ElseStmt()->Accept(*m_pVisitor);
	}

	void Access(SnWhileStmt &sn)
	{
		assert(m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		sn.Body()->Accept(*m_pVisitor);
	}

	void Access(SnDoStmt &sn)
	{
		assert(m_pVisitor);
		sn.Body()->Accept(*m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
	}

	void Access(SnForStmt &sn)
	{
		assert(m_pVisitor);

		if (sn.Init() && sn.Init()->Kind() == NK_LocalDeclStmt) {
			auto& decl = static_cast<SnLocalDeclStmt&>(*sn.Init());
			decl.Type()->Accept(*m_pVisitor);
			if (decl.Type()->IsResolved()) {
				auto *pTypeField = decl.Type()->Field();
				auto pParent = sn.Parent();
				SnParagraph *pParagraph = nullptr;
				while (pParent) {
					if (pParent->Kind() == NK_Paragraph) {
						pParagraph = static_cast<SnParagraph *>(pParent);
						break;
					}
					pParent = pParent->Parent();
				}
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
						auto *pLeft = new SnIdentifierExpr(
							new std::string(d.name), *decl.Location());
						auto *pAssign = new SnAssignStmt(
							pLeft, d.pInitExpr, *decl.Location());
						sn.InitExtras().push_back(pAssign);

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
					}
				}
			}
		} else if (sn.Init()) {
			sn.Init()->Accept(*m_pVisitor);
		}

		sn.Cond()->Accept(*m_pVisitor);
		sn.Body()->Accept(*m_pVisitor);
		if (sn.Fini())
			sn.Fini()->Accept(*m_pVisitor);
	}

	//Phase 8e-5: foreach statement resolver.
	//Resolves iterable + var type, registers the loop var in the enclosing
	//Paragraph scope (function-scoped, same as for-loop). Element type and
	//3-way dispatch (Array / List / Dict) is determined later in codegen via
	//EvalDataType; the resolver does not need to compute it.
	void Access(SnForeachStmt &sn)
	{
		assert(m_pVisitor);

		//Find enclosing Paragraph for loop-var registration (mirror for-loop).
		auto pParent = sn.Parent();
		SnParagraph *pParagraph = nullptr;
		while (pParent) {
			if (pParent->Kind() == NK_Paragraph) {
				pParagraph = static_cast<SnParagraph *>(pParent);
				break;
			}
			pParent = pParent->Parent();
		}

		//1. Resolve iterable (EvalDataType gets populated for codegen to use).
		sn.Iterable()->Accept(*m_pVisitor);

		//2. Resolve declared var type.
		sn.VarType()->Accept(*m_pVisitor);
		SnField *pVarField = nullptr;
		if (sn.VarType()->IsResolved())
			pVarField = sn.VarType()->Field();

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

	void Access(SnBreakStmt &sn)
	{
	}

	void Access(SnContinueStmt &sn)
	{
	}

	void Access(SnSwitchStmt &sn)
	{
		assert(m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		for (auto* pCase : sn.Cases())
			pCase->Accept(*m_pVisitor);
		if (sn.Default())
			sn.Default()->Accept(*m_pVisitor);
	}

	void Access(SnCaseClause &sn)
	{
		assert(m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		sn.Body()->Accept(*m_pVisitor);
	}

	void Access(SnParagraph &sn)
	{
		assert(m_pVisitor);
		for (auto &stmt : sn.Statements())
		{
			stmt.Accept(*m_pVisitor);
		}
	}

	void Access(SnEnumDecl &sn)
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

	void Access(SnEnumMember &sn)
	{
	}

	void Access(SnStructDecl &sn)
	{
		assert(m_pVisitor);
		for (auto &field : sn.Members())
		{
			field.Type()->Accept(*m_pVisitor);
		}
		sn.AddFlags(NF_Resolved);
	}

	void Access(SnStructField &sn)
	{
	}

	void Access(SnClassDecl &sn)
	{
		assert(m_pVisitor);
		//Resolve super class reference using ExprResolver (not the
		//StatementResolver visitor, which lacks Access(SnNameExpr&)).
		if (sn.SuperName())
		{
			m_ExprResolver.Resolve(*sn.SuperName(), sn, sn, ERF_None);
			if (sn.SuperName()->IsResolved())
			{
				auto pSuperField = sn.SuperName()->Field();
				if (pSuperField && pSuperField->Kind() == NK_ClassDecl)
					sn.SuperClass(static_cast<SnClassDecl*>(pSuperField));
				else
					m_Env.Log(CLL_Error, sn.SuperName()->Location(),
						"\"%s\" is not a class type.", sn.SuperName()->ToString().c_str());
			}
		}
		//Resolve "implements I1, I2" names into SnInterfaceDecl* pointers.
		for (auto *pName : sn.ImplementsNames())
		{
			m_ExprResolver.Resolve(*pName, sn, sn, ERF_None);
			if (pName->IsResolved())
			{
				auto pField = pName->Field();
				if (pField && pField->Kind() == NK_InterfaceDecl)
					sn.AddImplements(static_cast<SnInterfaceDecl*>(pField));
				else
					m_Env.Log(CLL_Error, pName->Location(),
						"\"%s\" is not an interface type.",
						pName->ToString().c_str());
			}
		}
		//Like EN: a method that overrides a parent virtual method
		//is also virtual (implicit virtual propagation).
		//Check both name and parameter count to avoid false matches.
		auto *pSuper = sn.SuperClass();
		if (pSuper)
		{
			for (auto &field : sn.Members())
			{
				if (field.Kind() != NK_Function)
					continue;
				if (field.ContainFlags(NF_Virtual))
					continue;
				auto &childFunc = static_cast<SnFunction&>(field);
				auto *pAncestor = pSuper;
				while (pAncestor)
				{
					auto *pParentMethod = pAncestor->FindField(field.Name());
					if (pParentMethod && pParentMethod->Kind() == NK_Function
						&& pParentMethod->ContainFlags(NF_Virtual))
					{
						auto &parentFunc = static_cast<SnFunction&>(*pParentMethod);
						if (childFunc.Params().size() == parentFunc.Params().size())
						{
							field.AddFlags(NF_Virtual);
							break;
						}
					}
					pAncestor = pAncestor->SuperClass();
				}
			}
		}
		for (auto &field : sn.Members())
		{
			if (field.Kind() == NK_ClassField)
				field.Accept(*m_pVisitor);
		}
		//Resolve method bodies.
		for (auto &field : sn.Members())
		{
			if (field.Kind() == NK_Function)
				field.Accept(*m_pVisitor);
		}
		sn.AddFlags(NF_Resolved);
	}

	void Access(SnClassField &sn)
	{
		assert(m_pVisitor);
		if (sn.Type())
			sn.Type()->Accept(*m_pVisitor);
	}

	void Access(SnNewExpr &sn)
	{
		m_ExprResolver.Resolve(sn, *sn.Parent(), *m_pCurrType, ERF_None);
	}

	void Access(SnInvokeStmt &sn)
	{
		assert(m_pVisitor);
		sn.Expr()->Accept(*m_pVisitor);
	}

	void Access(SnCompoundAssignStmt &sn)
	{
		if (sn.IsResolved())
			return;
		assert(m_pVisitor);
		sn.Left()->Accept(*m_pVisitor);
		//Phase 9a: reject compound assignment to a const local.
		if (sn.Left()->Kind() == NK_IdentifierExpr)
		{
			auto* pLeftField = static_cast<SnIdentifierExpr&>(
				*sn.Left()).Field();
			if (pLeftField && pLeftField->ContainFlags(NF_Const))
			{
				m_Env.Log(CLL_Error, sn.Location(),
					"cannot assign to const local \"%s\".",
					pLeftField->Name().c_str());
			}
		}
		sn.Right()->Accept(*m_pVisitor);

		//Type check: RHS must be compatible with LHS type.
		//Apply cast fixup on RHS (same pattern as SnAssignStmt).
		SnField* pTargetType = nullptr;
		if (sn.Left()->Kind() == NK_IdentifierExpr)
		{
			auto* pLeftField = static_cast<SnIdentifierExpr&>(*sn.Left()).Field();
			if (!pLeftField)
				return;
			pTargetType = pLeftField->EvalDataType();
		}
		else if (sn.Left()->Kind() == NK_MemberExpr)
		{
			pTargetType = sn.Left()->EvalDataType();
		}
		else if (sn.Left()->Kind() == NK_SubscriptExpr)
		{
			pTargetType = sn.Left()->EvalDataType();
		}
		auto* pSourceType = sn.Right()->EvalDataType();
		if (!pTargetType || !pSourceType)
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

	void Access(SnAssertStmt &sn)
	{
		if (sn.IsResolved())
			return;
		assert(m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		sn.AddFlags(NF_Resolved);
	}

	//Phase 9d: try body and catch clauses. Catches must be non-empty;
	//each catch type must be Exception or a subclass; each catch var is
	//registered in the body's enclosing paragraph scope.
	void Access(SnTryStmt &sn)
	{
		if (sn.IsResolved())
			return;
		assert(m_pVisitor);
		if (sn.Catches().empty()) {
			m_Env.Log(CLL_Error, sn.Location(),
				"try statement must have at least one catch clause");
			sn.AddFlags(NF_Resolved);
			return;
		}
		if (sn.TryBody())
			sn.TryBody()->Accept(*m_pVisitor);
		for (auto* pCatch : sn.Catches())
			pCatch->Accept(*m_pVisitor);
		sn.AddFlags(NF_Resolved);
	}

	void Access(SnCatchClause &sn)
	{
		//1. Resolve catch type (must be Exception or subclass).
		sn.CatchType()->Accept(*m_pVisitor);
		if (!sn.CatchType()->IsResolved()
			|| !IsExceptionSubclass(sn.CatchType()->Field())) {
			m_Env.Log(CLL_Error, sn.Location(),
				"catch type must be Exception or a subclass");
		}

		//2. Find enclosing paragraph for catch var registration.
		auto pParent = sn.Parent();
		SnParagraph *pParagraph = nullptr;
		while (pParent) {
			if (pParent->Kind() == NK_Paragraph) {
				pParagraph = static_cast<SnParagraph *>(pParent);
				break;
			}
			pParent = pParent->Parent();
		}
		//3. Register catch var (typed by catch type; assignable in body).
		if (pParagraph && sn.CatchType()->IsResolved()
			&& sn.CatchType()->Field()) {
			auto *pLocal = new SnLocalVar(sn.VarName(),
				sn.CatchType()->Field(), *sn.Location());
			pParagraph->AddLocal(sn.VarName(), pLocal);
		}

		//4. Resolve body.
		if (sn.Body())
			sn.Body()->Accept(*m_pVisitor);
	}

	void Access(SnThrowStmt &sn)
	{
		if (sn.IsResolved())
			return;
		assert(m_pVisitor);
		if (sn.IsRethrow()) {
			//throw; — must be lexically inside a catch handler. Walk
			//parent chain looking for NK_CatchClause.
			auto pParent = sn.Parent();
			bool inCatch = false;
			while (pParent) {
				if (pParent->Kind() == NK_CatchClause) {
					inCatch = true;
					break;
				}
				pParent = pParent->Parent();
			}
			if (!inCatch)
				m_Env.Log(CLL_Error, sn.Location(),
					"throw; (rethrow) is only valid inside a catch block");
		} else {
			sn.Expr()->Accept(*m_pVisitor);
			//EvalDataType is populated by the resolver after Accept().
			auto pType = sn.Expr()->EvalDataType();
			if (pType && !IsExceptionSubclass(pType)) {
				m_Env.Log(CLL_Error, sn.Location(),
					"throw expression must be Exception or a subclass");
			}
		}
		sn.AddFlags(NF_Resolved);
	}

	//Phase 9d: walk the SuperClass chain of t (if any). Returns true if any
	//ancestor class is named "Exception" (case-sensitive). Also returns true
	//when t itself is "Exception".
	bool IsExceptionSubclass(SnField *t)
	{
		if (!t)
			return false;
		//Catch types are always class-typed (resolved by SnClassDecl).
		auto pClass = dynamic_cast<SnClassDecl*>(t);
		if (!pClass)
			return false;
		SnClassDecl *cur = pClass;
		while (cur) {
			if (cur->Name() == "Exception")
				return true;
			cur = cur->SuperClass();
		}
		return false;
	}

	void Access(SnSubscriptAssignStmt &sn)
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
		sn.AddFlags(NF_Resolved);
	}

	void Access(SnThisExpr &sn)
	{
		m_ExprResolver.Resolve(sn, *sn.Parent(), *m_pCurrType, ERF_None);
	}

	void Access(SnArrayTypeExpr &)
	{
		//Type expressions are resolved via ExprResolver when used in
		//declarations; nothing to do at statement level.
	}

	//Note: SnAsExpr intentionally has NO Access() override here — it falls
	//through to Access(SnExpression&) which delegates to ExprResolver,
	//which calls ExprResolveAccessor.Access(SnAsExpr&). Providing an empty
	//stub would shadow the catch-all and skip resolution entirely.

	void Access(SnInterfaceDecl &sn)
	{
		//Interface method signatures are resolved like class methods but
		//they have no bodies (the resolver tolerates an empty body when
		//NF_Abstract is set). No super-class chain to walk.
		for (auto &field : sn.Members())
		{
			if (field.Kind() == NK_Function)
				field.Accept(*m_pVisitor);
		}
		sn.AddFlags(NF_Resolved);
	}

	void Access(SyntaxNode &sn)
	{
	}

private:
	BuildEnvironment &m_Env;
	ISyntaxNodeVisitor *m_pVisitor;
	SnField *m_pCurrType;
	ExprResolver m_ExprResolver;
};

class StatementResolver
{
public:
	explicit StatementResolver(BuildEnvironment &env) :	m_Accessor(env)
	{
	}

	void Resolve(SnNamespace &root)
	{
		SyntaxNodeVisitor<StatementResolveAccessor>
			visitor(m_Accessor, NVK_CustomTraverse);
		m_Accessor.Visitor(&visitor);
		root.Accept(visitor);
	}
private:
	StatementResolveAccessor m_Accessor;
};

} //namespace nlang
