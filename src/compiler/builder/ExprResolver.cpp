#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"

namespace nlang
{

void ExprResolveAccessor::Access(SnLiteralExpr &sn)
{
	assert(sn.IsResolved());
}

void ExprResolveAccessor::Access(SnNameExpr &nameExpr)
{
	assert(!nameExpr.IsResolved());

	auto pFieldExpr = nameExpr.Expr();
	assert(pFieldExpr);
	pFieldExpr->Accept(*m_pVisitor);
	if (!pFieldExpr->IsResolved())
		return;

	ResolveFieldExprAs(nameExpr, pFieldExpr->Field());
}

void ExprResolveAccessor::Access(SnIdentifierExpr &idExpr)
{
	if (idExpr.IsResolved())
	{
		//Builtin types have been resolve during parsing, but not verified.
		if (!idExpr.PostResolveCheck(m_Env))
			idExpr.RemoveFlags(NF_Resolved);
		return;
	}

	//Search starting from the expression's context (parent node), not from
	//the enclosing function. This ensures local variables declared in nested
	//Paragraphs are found before falling through to function-level scope.
	SnField *pField;
	pField = FindFieldInAncestor(idExpr.Name(), *m_pContext, *m_pAccessor,
		Flags());

	if (!pField && ContainFlags(ERF_IgnoreUsings))
	{
		//Search in the using list.
		assert(idExpr.Usings());
		pField = FindFieldInUsings(idExpr.Name(), *idExpr.Usings(),
			*m_pAccessor);
	}

	if (!pField)
	{
		m_Env.Log(CLL_Error, "Cannot resolve the field: %s.",
			idExpr.Name().c_str());
		return;
	}

	ResolveFieldExprAs(idExpr, pField);
}

void ExprResolveAccessor::Access(SnInvokeExpr &snInvoke)
{
	assert(!snInvoke.IsResolved());

	//Resolve concrete parameters.
	if (!ResolveExpressionList(snInvoke.Params()))
		return;

	SnFunction *pCallee;
	auto res = FindFuncByInvoke(pCallee, snInvoke);
	switch (res)
	{
	case FFR_ApproximateMatch:
		assert(pCallee);
		FixupParamTypes(snInvoke, pCallee->Params());
		break;
	case FFR_ExactMatch:
		assert(pCallee);
		break;
	case FFR_Incompatible:
		assert(pCallee);
		m_Env.Log(CLL_Error, snInvoke.Location(),
			"The function invoke \"%s\" is not compatible with the "
			"declaration.", snInvoke.ToString().c_str());
		m_Env.Log(CLL_More, pCallee->Location(),
			"See also the declaration of \"%s\".",
			pCallee->ToString().c_str());
		return;
	default:
		assert(res == FFR_FuncNameNotFound);
		m_Env.Log(CLL_Error, snInvoke.Location(),
			"The function \"%s\" does not exist or is not accessible.",
			snInvoke.CalleeName().c_str());
		return;
	}

	ResolveFieldExprAs(snInvoke, pCallee);
}

void ExprResolveAccessor::Access(SnMemberExpr &snMember)
{
	assert(!snMember.IsResolved());

	//Resolve outer expression.
	auto pOuterExpr = snMember.Outer();
	assert(pOuterExpr);
	pOuterExpr->Accept(*m_pVisitor);
	if (!pOuterExpr->IsResolved())
		return;

	auto pSavedContext	= m_pContext;

	//Set the context type by outer expression.
	if (snMember.Outer()->IsDataExpr())
		m_pContext = snMember.Outer()->EvalDataType();
	else
	{
		//The outer expression is a type or a namespace.
		//e.g., "MyClass", "MyNamespace", "int", etc.
		auto &outerFieldExpr = static_cast<SnFieldExpr &>(*snMember.Outer());
		auto* pOuterField = static_cast<SnField *>(outerFieldExpr.Field());
		m_pContext = pOuterField;
		//For non-data fields (e.g. local variables) that have an
		//EvalDataType but are not IsDataExpr, use EvalDataType
		//instead so string method resolution works.
		if (pOuterField && !pOuterField->IsTypeField())
			m_pContext = snMember.Outer()->EvalDataType();
	}

	//Resolve inner expression.
	SCOPED_FLAG_RESETER(*this);
	AddFlags(ERF_SearchInParentOnly);
	auto pInnerExpr = snMember.Inner();
	assert(pInnerExpr);

	//Builtin string methods: s.length(), etc.
	if (m_pContext && m_pContext->Kind() == NK_String
		&& pInnerExpr->Kind() == NK_InvokeExpr)
	{
		auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
		const auto& name = invoke.CalleeName();
		if (name == "length" && invoke.Params().begin() == invoke.Params().end())
		{
			pInnerExpr->AddFlags(NF_Resolved);
			snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
			snMember.AddFlags(NF_Resolved);
			m_pContext = pSavedContext;
			return;
		}
	}

	pInnerExpr->Accept(*m_pVisitor);
	if (pInnerExpr->IsResolved())
		ResolveFieldExprAs(snMember, pInnerExpr->Field());

	m_pContext = pSavedContext; 
}

void ExprResolveAccessor::Access(SnCastExpr &sn)
{
	//TODO:
}

void ExprResolveAccessor::Access(SnBinaryExpr &sn)
{
	assert(!sn.IsResolved());

	//Resolve left operand
	sn.Left()->Accept(*m_pVisitor);
	if (!sn.Left()->IsResolved())
		return;

	//Resolve right operand (if binary)
	if (sn.Right())
	{
		sn.Right()->Accept(*m_pVisitor);
		if (!sn.Right()->IsResolved())
			return;
	}

	//The result type is the same as the left operand for arithmetic ops.
	//For comparison and logical ops, the result is int32.
	auto op = sn.Op();
	if (op == SnBinaryExpr::OP_Less || op == SnBinaryExpr::OP_LessEqual ||
		op == SnBinaryExpr::OP_Greater || op == SnBinaryExpr::OP_GreaterEqual ||
		op == SnBinaryExpr::OP_Equal || op == SnBinaryExpr::OP_NotEqual ||
		op == SnBinaryExpr::OP_LogicalAnd || op == SnBinaryExpr::OP_LogicalOr ||
		op == SnBinaryExpr::OP_LogicalNot)
	{
		auto* intType = SnBuiltinDataType::InstanceOf(NK_Int32);
		sn.EvalDataType(intType);
	}
	else
	{
		sn.EvalDataType(sn.Left()->EvalDataType());
	}
	sn.AddFlags(NF_Resolved);
}

void ExprResolveAccessor::ResolveFieldExprAs(SnFieldExpr &expr, SnField *pField)
{
	assert(pField && !expr.IsResolved());

	expr.m_pField = pField;
	if (!expr.PostResolveCheck(m_Env))
	{
		expr.AddFlags(NF_Invalid);
		return;
	}

	expr.EvalDataType(pField->EvalDataType()); //Would be null if is void type.
	expr.AddFlags(NF_Resolved);
	return;
}

SnField *ExprResolveAccessor::FindFieldInAncestor(const std::string &sName,
	SyntaxNode &parent, const SnField &accessor, ExprResolveFlagSet flags)
{
	SnField *pField = parent.FindField(sName);
	if (pField && pField->AllowAccess(accessor))
		return pField;
	if (flags & ERF_SearchInParentOnly)
		return pField;
	auto pParent = parent.Parent();
	if (!pParent)
		return nullptr;
	return FindFieldInAncestor(sName, *pParent, accessor, flags);
}

SnField *ExprResolveAccessor::FindFieldInUsings(std::string &sName, 
	const UsingList &usings, const SnField & accessor)
{
	for (auto pUsing : usings)
	{
		if (!pUsing->IsResolved())
			continue;
		auto pNamespace = pUsing->Namespace();
		assert(pNamespace);
		auto pField = FindFieldInAncestor(sName, *pNamespace, accessor,
			ERF_SearchInParentOnly);
		if (pField)
			return pField;
	}
	return nullptr;
}

bool ExprResolveAccessor::ResolveExpressionList(SnExpressionList &exprs)
{
	bool bOK = true;
	for (auto &expr : exprs)
	{
		expr.Accept(*m_pVisitor);
		if (!expr.IsResolved() && bOK)
			bOK = false;
	}
	return bOK;
}

FindFuncResult ExprResolveAccessor::FindFuncByInvoke(SnFunction *&pFuncFound,
	SnInvokeExpr &invoke)
{
	const bool bSearchInAncestor = !ContainFlags(ERF_SearchInParentOnly);
	int nDistance = -1;
	bool bFoundByName = false; //Is there a function with the given name?
	SyntaxNode *pParent = m_pContext;
	auto &sFuncName = invoke.CalleeName();
	while (pParent)
	{
		if (CanBeFuncParent(pParent->Kind()))
		{
			auto pParentType = static_cast<SnFunctionParentField *>(pParent);
			auto range = pParentType->Members().NameDict().equal_range(sFuncName);
			for (auto iField = range.first; iField != range.second; ++iField)
			{
				SnField *pField = iField->second;
				if (pField->Kind() != NK_Function)
					continue;

				auto pFunc = static_cast<SnFunction *>(pField);
				if (!pFunc->AllowAccess(*m_pAccessor))
					continue;

				if (!bFoundByName)
				{
					bFoundByName = true;
					pFuncFound = pFunc;
				}

				const int n = CalcDistanceOfParams(invoke.Params(), pFunc->Params());
				if (n < 0)
					continue;
				if (n == 0)
				{
					pFuncFound = pFunc;
					return FFR_ExactMatch;
				}
				//n > 0
				if (nDistance > n)
				{
					pFuncFound = pFunc;
					nDistance = n;
				}
			} //for
			if (!bSearchInAncestor)
				break;
		}
		pParent = pParent->Parent();
	}

	if (nDistance < 0)
		return bFoundByName ? FFR_Incompatible : FFR_FuncNameNotFound;
	assert(nDistance > 0);
	return FFR_ApproximateMatch;
}

int ExprResolveAccessor::CalcDistanceOfParams(
	const SnExpressionList &concretParams, 
	const SnFunction::ParamList &formalParams) const
{
	int nDistance = 0;
	auto iFormal = formalParams.begin();
	auto iFormalEnd = formalParams.end();
	for (auto &concret : concretParams)
	{
		if (!concret.EvalDataType() || !iFormal->EvalDataType()) 
			return -1; //invalid parameters.
		if (iFormal == iFormalEnd) 
			return -1; //# of params mismatch.
		nDistance +=
			CalcTypeDistance(*concret.EvalDataType(), *iFormal->EvalDataType());
	}
	return nDistance;
}

int ExprResolveAccessor::CalcTypeDistance(const SnField &source,
	const SnField &target) const
{
	if (&source == &target)
		return 0;
	auto srcKind = source.Kind();
	auto tgtKind = target.Kind();
	//Enum types are int32 at runtime.
	if (srcKind == NK_EnumDecl) srcKind = NK_Int32;
	if (tgtKind == NK_EnumDecl) tgtKind = NK_Int32;
	if (IsPrimitiveType(srcKind) && IsPrimitiveType(tgtKind))
		return std::abs(srcKind - tgtKind);
	//TODO: other types.
	return -1;
}

void ExprResolveAccessor::FixupParamTypes(SnInvokeExpr &invoke,
	SnFunction::ParamList &formalParams)
{
	auto &concreteParams = invoke.Children();
	auto iConcreteEnd = concreteParams.end();
	auto iFormalEnd = formalParams.end();
	auto iFormal = formalParams.begin();
	for (auto iConcrete = concreteParams.begin();
		iConcrete != iConcreteEnd; ++iConcrete)
	{
		assert(iFormal != iFormalEnd);
		assert(static_cast<SyntaxNode &>(*iConcrete).IsExpression());
		auto &cParam = static_cast<SnExpression &>(*iConcrete);
		auto &fParam = *iFormal;
		TypeCastInfo castInfo(cParam.EvalDataType(), fParam.EvalDataType());
		FixupExprType(iConcrete, castInfo);
	}
}

bool ExprResolveAccessor::FixupExprType(NodeIterator &iSrcExpr,
	TypeCastInfo &castInfo)
{
	if (castInfo.Kind() == TCK_Same)
		return false;

	assert(static_cast<SyntaxNode &>(*iSrcExpr).IsExpression());
	auto &srcExpr = static_cast<SnExpression &>(*iSrcExpr);

	if (castInfo.Kind() != TCK_Auto)
	{
		m_Env.Log(CLL_Error, srcExpr.Location(),
			"Incompatible type \"%s\".", srcExpr.ToString().c_str());
		return false;
	}

	/*
	castInfo.Kind() == TCK_Auto
	Create a cast expression and replace the expression at iSrcExpr.
	e.g., "foo = bar" become "foo = cast<type of foo>(bar)", where
	bar is the source expression.
	*/
	auto pSrcParent = srcExpr.Parent();
	assert(pSrcParent);

	/*
	The source expression must be removed firstly from its old parent, since it 
	will be a child of our new cast expression.
	*/
	auto iInsertPos = RemoveChildFrom(iSrcExpr, *pSrcParent);
	assert(srcExpr.Location());
	auto pCastExpr =
		new SnCastExpr(&srcExpr, castInfo, *srcExpr.Location());
	//Replace pSrcExpr with pCastExpr.
	iSrcExpr = InsertChildInto(iInsertPos, pCastExpr, *pSrcParent);
	return true;
}

bool ExprResolver::ResolveDataTypes(SnField &sn, SnField &outerType)
{
	if (sn.IsDataField())
	{
		if (sn.IsResolved())
			return true;

		if (sn.Kind() != NK_Function)
		{
			auto &dataField = static_cast<SnDataField &>(sn);
			return ResolveDataType(*dataField.Type(), outerType);
		}

		//Resolve function return type.
		auto pReturnType = static_cast<SnFunction &>(sn).ReturnType();
		if (pReturnType)
		{
			if (!ResolveDataType(*pReturnType, outerType))
				return false;
		}
	}

	if (!ResolveChildFields(sn))
		return false;
	sn.AddFlags(NF_Resolved);
	return true;
}

bool ExprResolver::ResolveDataType(SnNameExpr &typeExpr, SnField &outerType)
{
	if (!Resolve(typeExpr, outerType, outerType, ERF_None))
	{
		typeExpr.AddFlags(NF_Invalid);
		return false;
	}
	return true;
}

bool ExprResolver::ResolveChildFields(SnField & sn)
{
	//Recursively resolve the children of a type field.
	//Note: Fields in statements will not be resolved here.
	bool bOK = true;
	for (auto &child : sn.Children())
	{
		if (child.IsField())
		{
			auto &childField = static_cast<SnField &>(child);
			if (!ResolveDataTypes(childField, sn) && bOK)
				bOK = false;
		}
	}
	return bOK;
}

} //namespace nlang