#pragma once
#include "BuildEnvironment.h"
#include "SyntaxNodeVisitor.h"
#include "CastInfo.h"
#include "ExprResolver.h"
#include "SnStatements.h"
#include "SnData.h"
#include <nlang/vm/StdLib.h>
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

//--- Phase 12 Step 1: switch family model (D1/D2) ----------------------
//Switch discriminants and labels come in three families: int (including
//enum values), float, string. Enum member references masquerade as
//NK_Int32 (SnEnumMember::EvalDataType), and enum-typed variables carry
//NK_EnumDecl — both map to Int, making enum and int labels one family.
enum class SwitchFamily { None, Int, Float, String };

static SwitchFamily SwitchFamilyOfKind(NodeKind kind)
{
	switch (kind)
	{
	case NK_Int32:
	case NK_EnumDecl:	return SwitchFamily::Int;
	case NK_Float:	return SwitchFamily::Float;
	case NK_String:	return SwitchFamily::String;
	default:	return SwitchFamily::None;
	}
}

class StatementResolveAccessor
{
public:
	explicit StatementResolveAccessor(BuildEnvironment &env) :
		m_Env(env), m_pCurrType(nullptr), m_pVisitor(nullptr),
		m_ExprResolver(m_Env)
	{
	}

	//--- Phase 12 Step 1: switch family gating (D1/D2/D8) ---------------
	//D8 duplicate detection: only FOLDABLE labels (literals and enum
	//member references) are keyed — anything else (calls, variables,
	//computed expressions) is left to runtime first-match-wins.
	struct SwitchLabelKey
	{
		SwitchFamily	family = SwitchFamily::None;
		int32_t		intValue = 0;
		double		floatValue = 0.0;
		std::string	stringValue;
	};

	static bool ExtractSwitchLabelKey(SnExpression& label,
		SwitchLabelKey& key)
	{
		if (!label.IsResolved())
			return false;
		//Enum member reference (`Color.Red`): the resolved Field() chain
		//carries the member node.
		if (label.Kind() == NK_MemberExpr || label.Kind() == NK_IdentifierExpr)
		{
			auto* pField = static_cast<SnFieldExpr&>(label).Field();
			if (pField && pField->Kind() == NK_EnumMember)
			{
				auto& member = static_cast<SnEnumMember&>(*pField);
				//Defense in depth. In the normal flow the value pre-pass
				//(Resolve -> PreAssignEnumMemberValues) has already
				//assigned values for every enum by the time any switch
				//resolves, so this gate never fires. It guards against a
				//future reordering (e.g. the pre-pass removed or the data
				//pass's early NF_Resolved trusted again) reintroducing
				//stale-0 member keys as false duplicates.
				auto* pDecl = member.Parent();
				if (!pDecl || pDecl->Kind() != NK_EnumDecl
					|| !pDecl->ContainFlags(NF_Resolved))
					return false;
				key.family = SwitchFamily::Int;
				key.intValue = member.Value();
				return true;
			}
			return false;
		}
		if (label.Kind() != NK_LiteralExpr)
			return false;
		auto& lit = static_cast<SnLiteralExpr&>(label);
		auto* pType = lit.EvalDataType();
		if (!pType)
			return false;
		switch (pType->Kind())
		{
		case NK_Int32:
			key.family = SwitchFamily::Int;
			key.intValue = lit.Value().Get<int32_t>();
			return true;
		case NK_Float:
			key.family = SwitchFamily::Float;
			key.floatValue = lit.Value().Get<float>();
			return true;
		case NK_String:
		{
			key.family = SwitchFamily::String;
			auto* pStr = lit.Value().Data().m_String;
			key.stringValue = pStr ? *pStr : "";
			return true;
		}
		default:
			return false;
		}
	}

	//Report one error per redundant occurrence of a foldable value.
	//Int-family keys (int literals + enum members, D2 one family) share
	//one set; float keys compare as doubles, so 0.0 and -0.0 are one key
	//(they match the same discriminant under IEEE equality).
	void CheckDuplicateCaseLabels(SnSwitchStmt& sn)
	{
		std::vector<SwitchLabelKey> seen;
		std::vector<std::string> reprs;
		for (auto* pCase : sn.Cases())
		{
			for (auto* pLabel : pCase->Labels())
			{
				SwitchLabelKey key;
				if (!ExtractSwitchLabelKey(*pLabel, key))
					continue;
				bool duplicate = false;
				for (size_t n = 0; n < seen.size() && !duplicate; ++n)
				{
					if (seen[n].family != key.family)
						continue;
					duplicate = key.family == SwitchFamily::Int
						? seen[n].intValue == key.intValue
						: key.family == SwitchFamily::Float
						? seen[n].floatValue == key.floatValue
						: seen[n].stringValue == key.stringValue;
					if (duplicate)
						m_Env.Log(CLL_Error, pLabel->Location(),
							"duplicate case label '%s'", reprs[n].c_str());
				}
				if (duplicate)
					continue;
				seen.push_back(key);
				if (key.family == SwitchFamily::Int)
					reprs.push_back(std::to_string(key.intValue));
				else if (key.family == SwitchFamily::Float)
				{
					char buf[32];
					snprintf(buf, sizeof(buf), "%g", key.floatValue);
					reprs.push_back(buf);
				}
				else
					reprs.push_back(key.stringValue);
			}
		}
	}

	void Visitor(ISyntaxNodeVisitor *pVisitor)
	{
		m_pVisitor = pVisitor;
	}

private:
	//Phase 11: math/io/fs are reserved stdlib namespaces (the resolver
	//routes `math.sqrt(x)` on the outer name alone, so any local with
	//that name would be silently shadowed). Called at every local
	//registration site below — decl, for-init, foreach, catch var.
	void CheckLocalNameReserved(const std::string &name,
		const ISourceLocation *pLoc)
	{
		if (IsStdLibNamespaceName(name))
			m_Env.Log(CLL_Error, pLoc,
				"The name \"%s\" is reserved for a standard library "
				"namespace.", name.c_str());
	}

	//Conditions feed OP_JumpIfNot, which reads a single int32 from
	//pResult. Non-int conditions get silent garbage semantics: a string
	//is a pool handle (index 0 encodes as 0 — a nonempty string reads
	//false), structs/arrays are heap indices, floats only work by bit
	//luck (IEEE non-zero bits ≠ int 0). Static typing: conditions must
	//be int; comparisons already produce int.
	void CheckIntCondition(SnExpression &cond, const char *what)
	{
		if (!cond.IsResolved())
			return;
		auto* pType = cond.EvalDataType();
		if (pType && pType->Kind() != NK_Int32) {
			m_Env.Log(CLL_Error, cond.Location(),
				"%s condition must be int, got \"%s\".",
				what, pType->ToString().c_str());
		}
	}

public:

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
		//Phase 9f: native declarations are body-less by contract — the
		//implementation lives in the host's registered table. A body would
		//be silently ignored by GenerateFunction's native branch, so reject
		//here. Out params are rejected too: writeback needs a callee frame
		//and natives have none (the VM throws the same message at runtime).
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
		//Phase 9e: out parameters must be 4-byte scalar/class slots. A
		//struct out param would need deep-copy writeback into the caller
		//— unsupported in v1. Checked at declaration for clearer errors
		//than at every call site.
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
			CheckLocalNameReserved(decl.name, sn.Location());
			pParagraph->AddLocal(decl.name, pLocal);

			if (decl.pInitExpr)
			{
				auto *pLeft = new SnIdentifierExpr(
					new std::string(decl.name), *sn.Location());
				//The init expr is a child of sn since the container
				//unification; hand its ownership over to the AssignStmt
				//instead of letting both own it.
				auto *pAssign = new SnAssignStmt(
					pLeft, sn.DetachChild(decl.pInitExpr),
					*sn.Location());

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
		{
			//Void-support: a resolved RHS with no EvalDataType is a void
			//function/method call. Codegen would store a stale pResult (the
			//last evaluated argument), so reject at the statement level.
			//(Return/binary consumers already reject via FixupExprType.)
			if (pTargetType && sn.Right()->IsResolved())
			{
				m_Env.Log(CLL_Error, sn.Right()->Location(),
					"cannot assign the result of void function \"%s\".",
					sn.Right()->ToString().c_str());
			}
			return;
		}
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
		CheckIntCondition(*sn.Cond(), "if");
		sn.ThenStmt()->Accept(*m_pVisitor);
		if (sn.ElseStmt())
			sn.ElseStmt()->Accept(*m_pVisitor);
	}

	void Access(SnWhileStmt &sn)
	{
		assert(m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		CheckIntCondition(*sn.Cond(), "while");
		sn.Body()->Accept(*m_pVisitor);
	}

	void Access(SnDoStmt &sn)
	{
		assert(m_pVisitor);
		sn.Body()->Accept(*m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		CheckIntCondition(*sn.Cond(), "do-while");
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
					CheckLocalNameReserved(d.name, decl.Location());
					if (pParagraph) {
						pParagraph->AddLocal(d.name, pLocal);
					}
					if (d.pInitExpr) {
						auto *pLeft = new SnIdentifierExpr(
							new std::string(d.name), *decl.Location());
						//Detach from the inner LocalDeclStmt (decl — not
						//sn!) so the AssignStmt becomes the sole owner.
						//The typed slot keeps aliasing the node until
						//the nulling below, so the d.pInitExpr reads in
						//this block stay valid.
						auto *pAssign = new SnAssignStmt(
							pLeft, decl.DetachChild(d.pInitExpr),
							*decl.Location());
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
		CheckIntCondition(*sn.Cond(), "for");
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
			CheckLocalNameReserved(sn.VarName(), sn.Location());
			pParagraph->AddLocal(sn.VarName(), pLocal);
		}

		//4. Descend into body.
		sn.Body()->Accept(*m_pVisitor);
	}

	void Access(SnBreakStmt &sn)
	{
		//Phase 9d-2: control transfer out of a finally body is rejected
		//(would swallow exceptions/control flow — Java allows it, MVP does not).
		if (m_inFinallyBody)
			m_Env.Log(CLL_Error, sn.Location(),
				"break is not allowed inside a finally block");
	}

	void Access(SnContinueStmt &sn)
	{
		if (m_inFinallyBody)
			m_Env.Log(CLL_Error, sn.Location(),
				"continue is not allowed inside a finally block");
	}

	void Access(SnSwitchStmt &sn)
	{
		assert(m_pVisitor);
		sn.Cond()->Accept(*m_pVisitor);
		//D1 family gate. Arrays masquerade as their element type
		//(EvalDataType trap), so the array check comes FIRST. An
		//unresolved cond already reported its own error — skip the
		//family check to avoid cascades. The null literal is Int32-typed
		//(nlang.y KT_Null) and would slip through the family check as
		//Int — reject it on the cond side too, mirroring the label side.
		if (sn.Cond()->ContainFlags(NF_NullLiteral))
			m_Env.Log(CLL_Error, sn.Cond()->Location(),
				"switch discriminant must be int, float, string, or enum");
		else if (IsArrayValuedExpr(*sn.Cond()))
			m_Env.Log(CLL_Error, sn.Cond()->Location(),
				"switch discriminant must be int, float, string, or enum");
		else if (sn.Cond()->IsResolved())
		{
			auto* pType = sn.Cond()->EvalDataType();
			if (pType
				&& SwitchFamilyOfKind(pType->Kind()) == SwitchFamily::None)
				m_Env.Log(CLL_Error, sn.Cond()->Location(),
					"switch discriminant must be int, float, string, or enum");
		}
		for (auto* pCase : sn.Cases())
			pCase->Accept(*m_pVisitor);
		if (sn.Default())
			sn.Default()->Accept(*m_pVisitor);
		CheckDuplicateCaseLabels(sn);
	}

	void Access(SnCaseClause &sn)
	{
		assert(m_pVisitor);
		//D2: every label must belong to the discriminant's family. The
		//cond's resolved type is read back through the parent switch
		//(zero new AST state); an unresolved cond skips the check.
		auto* pParent = sn.Parent();
		auto* pSwitch = (pParent && pParent->Kind() == NK_SwitchStmt)
			? static_cast<SnSwitchStmt*>(pParent) : nullptr;
		auto* pCondType = pSwitch ? pSwitch->Cond()->EvalDataType() : nullptr;
		auto condFamily = pCondType
			? SwitchFamilyOfKind(pCondType->Kind()) : SwitchFamily::None;
		for (auto* pLabel : sn.Labels())
		{
			pLabel->Accept(*m_pVisitor);
			if (!pLabel->IsResolved())
				continue;   //its own resolution already reported
			if (pLabel->ContainFlags(NF_NullLiteral))
			{
				m_Env.Log(CLL_Error, pLabel->Location(),
					"null is not a valid case label");
				continue;
			}
			if (condFamily == SwitchFamily::None)
				continue;
			auto* pLabelType = pLabel->EvalDataType();
			if (pLabelType
				&& SwitchFamilyOfKind(pLabelType->Kind()) != condFamily)
				m_Env.Log(CLL_Error, pLabel->Location(),
					"case label type must match the switch discriminant family");
		}
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
			method.Accept(*m_pVisitor);
		}
	}

	//Assign sequential/explicit values to members and flag the decl
	//resolved. Shared by the document-order Access and the pre-pass
	//(PreAssignEnumMemberValues), which must NOT resolve method bodies —
	//that would double-resolve them once Access runs.
	void AssignEnumMemberValues(SnEnumDecl &sn)
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
		//Resolve method bodies. Phase 9d-2: track the enclosing class so
		//super(...) statements can validate against it.
		m_pCurrClass = &sn;
		for (auto &field : sn.Members())
		{
			if (field.Kind() == NK_Function)
				field.Accept(*m_pVisitor);
		}
		m_pCurrClass = nullptr;
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
		{
			//Void-support: same guard as SnAssignStmt — a resolved void
			//RHS must not flow into compound assignment.
			if (pTargetType && sn.Right()->IsResolved())
			{
				m_Env.Log(CLL_Error, sn.Right()->Location(),
					"cannot assign the result of void function \"%s\".",
					sn.Right()->ToString().c_str());
			}
			return;
		}
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
		CheckIntCondition(*sn.Cond(), "assert");
		sn.AddFlags(NF_Resolved);
	}

	//Phase 9d: try body and catch clauses. Phase 9d-2 adds the optional
	//finally body. Either at least one catch or a finally body is required;
	//each catch type must be Exception or a subclass; each catch var is
	//registered in the body's enclosing paragraph scope. Control-flow
	//statements (break/continue/return/throw) are rejected inside a finally
	//body (m_inFinallyBody flag).
	void Access(SnTryStmt &sn)
	{
		if (sn.IsResolved())
			return;
		assert(m_pVisitor);
		if (sn.Catches().empty() && !sn.FinallyBody()) {
			m_Env.Log(CLL_Error, sn.Location(),
				"try statement must have at least one catch clause or a finally block");
			sn.AddFlags(NF_Resolved);
			return;
		}
		if (sn.TryBody())
			sn.TryBody()->Accept(*m_pVisitor);
		for (auto* pCatch : sn.Catches())
			pCatch->Accept(*m_pVisitor);
		if (sn.FinallyBody()) {
			bool prev = m_inFinallyBody;
			m_inFinallyBody = true;
			sn.FinallyBody()->Accept(*m_pVisitor);
			m_inFinallyBody = prev;
		}
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
			CheckLocalNameReserved(sn.VarName(), sn.Location());
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
		if (m_inFinallyBody) {
			m_Env.Log(CLL_Error, sn.Location(),
				"throw is not allowed inside a finally block");
			sn.AddFlags(NF_Resolved);
			return;
		}
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

	//Phase 9d-2: super(args); — forwards ctor args to the direct parent
	//class constructor. Valid only inside a user constructor (a method of
	//a class whose name equals the function name). Args are positional
	//only; named arguments are rejected.
	void Access(SnSuperCallStmt &sn)
	{
		if (sn.IsResolved())
			return;
		assert(m_pVisitor);
		sn.AddFlags(NF_Resolved);

		//1. Must be inside a constructor of a user class.
		if (!m_pCurrClass || !m_pCurrType
			|| m_pCurrType->Kind() != NK_Function
			|| m_pCurrType->Name() != m_pCurrClass->Name()) {
			m_Env.Log(CLL_Error, sn.Location(),
				"super(...) is only valid inside a constructor");
			return;
		}
		//2. Direct parent must exist (Object and extension-less classes
		//have no SuperClass).
		auto pParent = m_pCurrClass->SuperClass();
		if (!pParent) {
			m_Env.Log(CLL_Error, sn.Location(),
				"class \"%s\" has no parent class for super(...)",
				m_pCurrClass->Name().c_str());
			return;
		}
		//3. Resolve args; named arguments are rejected.
		for (auto* pArg : sn.Args()) {
			if (pArg->Kind() == NK_NamedArgExpr) {
				m_Env.Log(CLL_Error, pArg->Location(),
					"named arguments are not supported in super(...)");
				continue;
			}
			if (pArg->Kind() == NK_OutArgExpr) {
				//Phase 9e: super(...) forwards args positionally without
				//FormalBindings — an out argument could never write back.
				m_Env.Log(CLL_Error, pArg->Location(),
					"out arguments are not supported in super(...)");
				continue;
			}
			pArg->Accept(*m_pVisitor);
		}
		//4. Arity check against the parent constructor.
		size_t parentArity = 0;
		bool parentHasCtor = false;
		if (pParent->IsBuiltinClass()) {
			//Built-in Exception family ctor: (this, message) → 1 user arg.
			//Other built-ins are not subclassable; ExprResolver already
			//rejects those SuperNames.
			parentHasCtor = true;
			parentArity = 1;
		} else {
			for (auto& field : pParent->Members()) {
				if (field.Kind() == NK_Function
					&& field.Name() == pParent->Name()) {
					parentHasCtor = true;
					parentArity = static_cast<SnFunction&>(field)
						.Params().size();
					break;
				}
			}
		}
		if (!parentHasCtor) {
			if (!sn.Args().empty()) {
				m_Env.Log(CLL_Error, sn.Location(),
					"parent class \"%s\" has no constructor; super(...) "
					"cannot take arguments",
					pParent->Name().c_str());
			}
			//super(); with no parent ctor is a legal no-op.
			return;
		}
		if (sn.Args().size() != parentArity) {
			m_Env.Log(CLL_Error, sn.Location(),
				"super(...) expects %zu argument(s), got %zu",
				parentArity, sn.Args().size());
		}
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
	SnClassDecl *m_pCurrClass = nullptr;
	bool m_inFinallyBody = false;
	ExprResolver m_ExprResolver;
};

class StatementResolver
{
public:
	explicit StatementResolver(BuildEnvironment &env) :	m_Accessor(env)
	{
	}

	//Pre-pass for Resolve: run Access(SnEnumDecl) on every enum decl in
	//the tree before normal document-order traversal. The ResolveDataTypes
	//pass flags enum decls NF_Resolved WITHOUT assigning member values, so
	//a forward-referenced enum (a function above the decl using
	//`case Color.Red`) would otherwise read stale 0s — e.g. false
	//"duplicate case label" errors. Value assignment is pure literal work,
	//so pre-pass + in-order Access is idempotent.
	static void PreAssignEnumMemberValues(Node& node,
		StatementResolveAccessor& accessor)
	{
		if (node.Kind() == NK_EnumDecl)
			accessor.AssignEnumMemberValues(static_cast<SnEnumDecl&>(node));
		for (auto& child : node.Children())
			PreAssignEnumMemberValues(child, accessor);
	}

	void Resolve(SnNamespace &root)
	{
		SyntaxNodeVisitor<StatementResolveAccessor>
			visitor(m_Accessor, NVK_CustomTraverse);
		m_Accessor.Visitor(&visitor);
		PreAssignEnumMemberValues(root, m_Accessor);
		root.Accept(visitor);
	}
private:
	StatementResolveAccessor m_Accessor;
};

} //namespace nlang
