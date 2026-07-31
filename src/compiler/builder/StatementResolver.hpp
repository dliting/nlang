#pragma once
#include "BuildEnvironment.h"
#include "SyntaxNodeVisitor.h"
#include "CastInfo.h"
#include "ExprResolver.h"
#include "SnStatements.h"
#include "SnData.h"

namespace nlang
{

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
			if (CanBeFuncParent(field.Kind()) || field.Kind() == NK_Function || field.Kind() == NK_EnumDecl)
				field.Accept(*m_pVisitor);
		}
	}

	void Access(SnFunction &sn)
	{
		assert(m_pVisitor);
		if (!sn.Body())
			return;

		m_pCurrType = &sn;
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

	/*
	Local variable declaration.
	Reference: EN's LocalDeclStmt::DoResolve (SeStatements.cpp:232).
	*/
	void Access(SnLocalDeclStmt &sn)
	{
		assert(m_pVisitor);
		//Resolve the type name
		sn.Type()->Accept(*m_pVisitor);
		if (!sn.Type()->IsResolved())
			return;

		//Find the parent paragraph to register locals
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

		//Register each local variable using SnLocalVar (a lightweight
		//descriptor that references the resolved type without owning it
		//via AddChild). SnFormalParam cannot be used here because its
		//constructor calls AddChild on the type expression, which would
		//fail — the type expr is already owned by SnLocalDeclStmt.
		//
		//Decomposition pattern (Reference: EN's LocalDeclStmt::ResolveItem,
		//SeStatements.cpp:261): declarations with initializers are split into
		//variable registration + AssignStmt. This ensures all assignment
		//semantics (type coercion, etc.) go through a single code path.
		auto *pTypeField = sn.Type()->Field();
		for (auto &decl : sn.Decls())
		{
			auto *pLocal = new SnLocalVar(decl.name, pTypeField,
				*sn.Location());
			pParagraph->AddLocal(decl.name, pLocal);

			if (decl.pInitExpr)
			{
				//Decomposition: create AssignStmt for the initializer.
				//Reference: EN's LocalDeclStmt::ResolveItem
				//(SeStatements.cpp:261).
				auto *pLeft = new SnIdentifierExpr(
					new std::string(decl.name), *sn.Location());
				auto *pAssign = new SnAssignStmt(
					pLeft, decl.pInitExpr, *sn.Location());

				//Insert right after this LocalDeclStmt in the Paragraph.
				//Use Node::InsertChild at the position after &sn.
				auto iPos = pParagraph->Children().find(&sn);
				++iPos;
				pParagraph->InsertChild(iPos, pAssign);

				//Immediately resolve the new AssignStmt.
				pAssign->Accept(*m_pVisitor);

				//The init expression is now owned by the AssignStmt.
				decl.pInitExpr = nullptr;
			}
		}
	}

	/*
	Assignment statement.
	Reference: EN's AssignStmt::DoResolve (SeStatements.cpp:306).
	*/
	void Access(SnAssignStmt &sn)
	{
		if (sn.IsResolved())
			return;
		assert(m_pVisitor);
		sn.Left()->Accept(*m_pVisitor);
		sn.Right()->Accept(*m_pVisitor);

		//Type coercion: if the right-hand expression's type differs from
		//the left-hand variable's type, insert a CastExpr.
		//Reference: EN's AssignStmt::DoResolve (SeStatements.cpp:306).
		auto* pLeftField = sn.Left()->Kind() == NK_IdentifierExpr
			? static_cast<SnIdentifierExpr&>(*sn.Left()).Field() : nullptr;
		if (!pLeftField)
			return;
		auto* pTargetType = pLeftField->EvalDataType();
		auto* pSourceType = sn.Right()->EvalDataType();
		if (!pTargetType || !pSourceType)
			return;
		auto castInfo = GetCastInfo(pSourceType, pTargetType);
		auto iExpr = sn.Children().find(sn.m_pRight);
		if (m_ExprResolver.FixupExprType(iExpr, castInfo))
			sn.m_pRight = &static_cast<SnCastExpr &>(*iExpr);
		sn.AddFlags(NF_Resolved);
	}

	/*
	If/else statement.
	Reference: EN's IfStmt::DoResolve (SeStatements.cpp:422).
	*/
	void Access(SnIfStmt &sn)
	{
		assert(m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		sn.ThenStmt()->Accept(*m_pVisitor);
		if (sn.ElseStmt())
			sn.ElseStmt()->Accept(*m_pVisitor);
	}

	/*
	While loop statement.
	Reference: EN's WhileStmt::DoResolve (SeStatements.cpp:463).
	*/
	void Access(SnWhileStmt &sn)
	{
		assert(m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		sn.Body()->Accept(*m_pVisitor);
	}

	/*
	Do-while loop statement.
	Reference: EN's DoStmt::DoResolve — same as WhileStmt.
	*/
	void Access(SnDoStmt &sn)
	{
		assert(m_pVisitor);
		sn.Body()->Accept(*m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
	}

	/*
	For loop statement.
	Reference: EN's ForStmt::DoResolve (SeStatements.cpp:611).
	*/
	void Access(SnForStmt &sn)
	{
		assert(m_pVisitor);

		//1. Handle init part
		//Reference: EN's CompositeStatement::DoResolve + LocalDeclStmt::ResolveItem
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
				for (auto& d : decl.Decls()) {
					auto *pLocal = new SnLocalVar(d.name, pTypeField,
						*decl.Location());
					if (pParagraph) {
						pParagraph->AddLocal(d.name, pLocal);
					}
					if (d.pInitExpr) {
						auto *pLeft = new SnIdentifierExpr(
							new std::string(d.name), *decl.Location());
						auto *pAssign = new SnAssignStmt(
							pLeft, d.pInitExpr, *decl.Location());
						sn.InitExtras().push_back(pAssign);

						//Resolve expressions using the Paragraph as
						//context, since pAssign is not in the AST
						//child list and has no parent for
						//FindFieldInAncestor to walk up.
						if (pParagraph) {
							m_ExprResolver.Resolve(*pLeft,
								*pParagraph, *m_pCurrType, ERF_None);
							m_ExprResolver.Resolve(*d.pInitExpr,
								*pParagraph, *m_pCurrType, ERF_None);

							//Type coercion (same as Access(SnAssignStmt)).
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

		//2. Resolve condition
		sn.Cond()->Accept(*m_pVisitor);

		//3. Resolve body
		sn.Body()->Accept(*m_pVisitor);

		//4. Resolve fini
		if (sn.Fini())
			sn.Fini()->Accept(*m_pVisitor);
	}

	/*
	Break statement.
	Reference: EN's BreakStmt::DoResolve (SeStatements.cpp:876) — no-op.
	*/
	void Access(SnBreakStmt &sn)
	{
	}

	/*
	Continue statement.
	Reference: EN's ContinueStmt::DoResolve (SeStatements.cpp:902) — no-op.
	*/
	void Access(SnContinueStmt &sn)
	{
	}

	/*
	Switch statement.
	Reference: EN's SwitchStmt::DoResolve (SeStatements.cpp:824).
	*/
	void Access(SnSwitchStmt &sn)
	{
		assert(m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		for (auto* pCase : sn.Cases())
			pCase->Accept(*m_pVisitor);
		if (sn.Default())
			sn.Default()->Accept(*m_pVisitor);
	}

	/*
	Case clause.
	Reference: EN's CondClause::Resolve (SeStatements.cpp:197).
	*/
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

	/*
	Enum type declaration.
	Resolve enum member values: auto-increment or explicit assignment.
	*/
	void Access(SnEnumDecl &sn)
	{
		assert(m_pVisitor);
		int32_t nextValue = 0;
		for (auto &member : sn.Members())
		{
			if (member.ValueExpr())
			{
				member.ValueExpr()->Accept(*m_pVisitor);
				//Only support integer literal values for enum members.
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

		void Access(SyntaxNode &sn)
	{
		//Fallback for unhandled node types.
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
