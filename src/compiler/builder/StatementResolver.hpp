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
			if (CanBeFuncParentEx(field.Kind()) || field.Kind() == NK_Function || field.Kind() == NK_EnumDecl || field.Kind() == NK_StructDecl || field.Kind() == NK_ClassDecl)
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
		}
	}

	void Access(SnAssignStmt &sn)
	{
		if (sn.IsResolved())
			return;
		assert(m_pVisitor);
		sn.Left()->Accept(*m_pVisitor);
		sn.Right()->Accept(*m_pVisitor);

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
