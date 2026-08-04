#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"

namespace nlang
{

//Canonical SnClassDecl instances for built-in classes. These must be
//singletons so that CalcTypeDistance's pointer identity check works
//across all resolution sites (type names, new expressions, member calls).
static SnClassDecl* s_pByteStreamClass = nullptr;
static SnClassDecl* s_pFileStreamClass = nullptr;

static SnClassDecl* GetBuiltinClassDecl(const std::string& name,
	const ISourceLocation* pLoc)
{
	SnClassDecl*& rpRef = (name == "ByteStream") ? s_pByteStreamClass
		: s_pFileStreamClass;
	if (!rpRef)
	{
		auto* pName = new std::string(name);
		auto* pMembers = new PtrList<SnField>();
		ScriptLocation loc;
		if (pLoc)
			loc = *static_cast<const ScriptLocation*>(pLoc);
		rpRef = new SnClassDecl(pName, nullptr, pMembers, loc);
		rpRef->SetBuiltinClass();
	}
	return rpRef;
}

void ExprResolveAccessor::Access(SnLiteralExpr &sn)
{
	assert(sn.IsResolved());
}

void ExprResolveAccessor::Access(SnNameExpr &nameExpr)
{
	if (nameExpr.IsResolved())
	{
		return;
	}

	auto pFieldExpr = nameExpr.Expr();
	assert(pFieldExpr);
	pFieldExpr->Accept(*m_pVisitor);

	//Builtin class names: ByteStream, FileStream.
	//When used as a type name (e.g. "ByteStream s = ..."), the name
	//doesn't exist in the AST namespace. Synthesize a singleton SnClassDecl.
	if (!pFieldExpr->IsResolved())
	{
		const auto& name = pFieldExpr->ToString();
		if (name == "ByteStream" || name == "FileStream")
		{
			ResolveFieldExprAs(*pFieldExpr,
				GetBuiltinClassDecl(name, pFieldExpr->Location()));
		}
	}

	if (!pFieldExpr->IsResolved())
		return;

	ResolveFieldExprAs(nameExpr, pFieldExpr->Field());
}

void ExprResolveAccessor::Access(SnArrayTypeExpr &arrTypeExpr)
{
	if (arrTypeExpr.IsResolved())
		return;

	auto *pElemType = arrTypeExpr.ElementType();
	assert(pElemType);
	pElemType->Accept(*m_pVisitor);
	if (!pElemType->IsResolved())
		return;

	//Propagate the element type's field to the array type expression.
	ResolveFieldExprAs(arrTypeExpr, pElemType->Field());
}

void ExprResolveAccessor::Access(SnIdentifierExpr &idExpr)
{
	if (idExpr.IsResolved())
	{
		if (!idExpr.PostResolveCheck(m_Env))
			idExpr.RemoveFlags(NF_Resolved);
		return;
	}

	SnField *pField;
	pField = FindFieldInAncestor(idExpr.Name(), *m_pContext, *m_pAccessor,
		Flags());

	if (!pField && ContainFlags(ERF_IgnoreUsings))
	{
		assert(idExpr.Usings());
		pField = FindFieldInUsings(idExpr.Name(), *idExpr.Usings(),
			*m_pAccessor);
	}

	if (!pField)
	{
		//Builtin class names: ByteStream, FileStream.
		//Synthesize a singleton SnClassDecl when the name is not found.
		const auto& name = idExpr.Name();
		if (name == "ByteStream" || name == "FileStream")
		{
			ResolveFieldExprAs(idExpr, GetBuiltinClassDecl(name, idExpr.Location()));
			return;
		}

		m_Env.Log(CLL_Error, "Cannot resolve the field: %s.",
			idExpr.Name().c_str());
		return;
	}

	ResolveFieldExprAs(idExpr, pField);
}

void ExprResolveAccessor::Access(SnInvokeExpr &snInvoke)
{
	assert(!snInvoke.IsResolved());

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

	auto pOuterExpr = snMember.Outer();
	assert(pOuterExpr);
	pOuterExpr->Accept(*m_pVisitor);
	if (!pOuterExpr->IsResolved())
		return;

	auto pSavedContext = m_pContext;

	if (snMember.Outer()->IsDataExpr())
		m_pContext = snMember.Outer()->EvalDataType();
	else
	{
		auto &outerFieldExpr = static_cast<SnFieldExpr &>(*snMember.Outer());
		auto* pOuterField = static_cast<SnField *>(outerFieldExpr.Field());
		m_pContext = pOuterField;
		if (pOuterField && !pOuterField->IsTypeField())
			m_pContext = snMember.Outer()->EvalDataType();
	}

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

		//Builtin array.length property.
		if (pOuterExpr->Kind() == NK_IdentifierExpr
			&& pInnerExpr->Kind() == NK_IdentifierExpr)
		{
			auto* pOuterField = static_cast<SnIdentifierExpr*>(pOuterExpr)->Field();
			auto& innerId = static_cast<SnIdentifierExpr&>(*pInnerExpr);
			if (pOuterField && pOuterField->IsArrayType()
				&& innerId.Name() == "length")
			{
				pInnerExpr->AddFlags(NF_Resolved);
				snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
				snMember.AddFlags(NF_Resolved);
				m_pContext = pSavedContext;
				return;
			}
		}

		//Builtin stream methods: ByteStream/FileStream member calls.
		//These are resolved by name since the synthesized SnClassDecl has
		//no real method members. The return type is determined by method name.
		if (m_pContext && m_pContext->Kind() == NK_ClassDecl
			&& static_cast<SnClassDecl*>(m_pContext)->IsBuiltinClass()
			&& pInnerExpr->Kind() == NK_InvokeExpr)
		{
			auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
			const auto& name = invoke.CalleeName();
			bool isStreamMethod = false;
			NodeKind retKind = NK_Int32;  //default, overridden below
			if (name == "ReadInt" || name == "Length" || name == "Position")
				isStreamMethod = true;  // retKind = NK_Int32
			else if (name == "ReadFloat")
				{ isStreamMethod = true; retKind = NK_Float; }
			else if (name == "ReadString")
				{ isStreamMethod = true; retKind = NK_String; }
			else if (name == "WriteInt" || name == "WriteFloat"
				|| name == "WriteString" || name == "Reset" || name == "Close")
				isStreamMethod = true;  // void return — no EvalDataType
			if (isStreamMethod)
			{
				pInnerExpr->AddFlags(NF_Resolved);
				if (name != "WriteInt" && name != "WriteFloat"
					&& name != "WriteString" && name != "Reset" && name != "Close")
					snMember.EvalDataType(SnBuiltinDataType::InstanceOf(retKind));
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
}

void ExprResolveAccessor::Access(SnBinaryExpr &sn)
{
	assert(!sn.IsResolved());

	sn.Left()->Accept(*m_pVisitor);
	if (!sn.Left()->IsResolved())
		return;

	if (sn.Right())
	{
		sn.Right()->Accept(*m_pVisitor);
		if (!sn.Right()->IsResolved())
			return;
	}

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

void ExprResolveAccessor::Access(SnNewExpr &sn)
{
	assert(!sn.IsResolved());

	auto pClassName = sn.ClassName();
	assert(pClassName);
	pClassName->Accept(*m_pVisitor);

	//Builtin class names: ByteStream, FileStream.
	//These don't exist in the AST namespace, so the name won't resolve
	//through the normal path. Use the singleton SnClassDecl.
	if (!pClassName->IsResolved())
	{
		const auto& name = pClassName->ToString();
		if (name == "ByteStream" || name == "FileStream")
		{
			ResolveFieldExprAs(*pClassName,
				GetBuiltinClassDecl(name, pClassName->Location()));
		}
	}

	if (!pClassName->IsResolved())
		return;

	auto pClassField = pClassName->Field();
	if (!pClassField || pClassField->Kind() != NK_ClassDecl)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"\"%s\" is not a class type.", pClassName->ToString().c_str());
		return;
	}

	auto pClassDecl = static_cast<SnClassDecl*>(pClassField);
	sn.ClassDecl(pClassDecl);
	sn.EvalDataType(pClassDecl);
	sn.AddFlags(NF_Resolved);

	if (!ResolveExpressionList(sn.Args()))
		return;
}

void ExprResolveAccessor::Access(SnNewArrayExpr &sn)
{
	assert(!sn.IsResolved());

	//Resolve element type name.
	auto pElemType = sn.ElementType();
	assert(pElemType);
	if (pElemType->IsArrayType())
	{
		//Resolve nested element type for array-of-arrays (future extension).
		m_Env.Log(CLL_Error, sn.Location(),
			"Multi-dimensional arrays are not supported.");
		return;
	}
	pElemType->Accept(*m_pVisitor);
	if (!pElemType->IsResolved())
		return;

	auto pElemField = pElemType->Field();
	if (!pElemField)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"\"%s\" is not a valid array element type.",
			pElemType->ToString().c_str());
		return;
	}

	//Resolve size expression with direct Accept (SnExpressionList wrapping is broken).
	sn.Size()->Accept(*m_pVisitor);
	if (!sn.Size()->IsResolved())
		return;

	//EvalDataType: store the element type for later array type registration.
	//We do NOT set Field() here since arrays are not a single field; the
	//backend registers a CompiledArrayType entry from this element type.
	sn.EvalDataType(pElemField);
	sn.AddFlags(NF_Resolved);
}

void ExprResolveAccessor::Access(SnSubscriptExpr &sn)
{
	assert(!sn.IsResolved());

	//Resolve array expression.
	auto& arrayExpr = *sn.Array();
	arrayExpr.Accept(*m_pVisitor);
	if (!arrayExpr.IsResolved())
		return;

	//Resolve index expression.
	auto& indexExpr = *sn.Index();
	indexExpr.Accept(*m_pVisitor);
	if (!indexExpr.IsResolved())
		return;

	//Look up arr.length-style access is handled by MemberExpr.
	//For now, the result type of subscript is the element type.
	auto* arrayType = arrayExpr.EvalDataType();
	if (arrayType)
		sn.EvalDataType(arrayType);
	sn.AddFlags(NF_Resolved);
}

void ExprResolveAccessor::Access(SnThisExpr &sn)
{
	assert(!sn.IsResolved());

	auto pContext = m_pContext;
	while (pContext)
	{
		if (pContext->Kind() == NK_Function)
		{
			auto pParent = pContext->Parent();
			if (pParent && pParent->Kind() == NK_ClassDecl)
			{
				auto pClassDecl = static_cast<SnClassDecl*>(pParent);
				sn.EvalDataType(pClassDecl);
				sn.AddFlags(NF_Resolved);
				return;
			}
		}
		pContext = pContext->Parent();
	}
	m_Env.Log(CLL_Error, sn.Location(),
		"'this' can only be used inside a class method.");
}

void ExprResolveAccessor::Access(SnClassDecl &sn)
{
	//Resolve super class reference.
	if (sn.SuperName())
	{
		sn.SuperName()->Accept(*m_pVisitor);
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
		pName->Accept(*m_pVisitor);
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
	sn.AddFlags(NF_Resolved);
}

void ExprResolveAccessor::Access(SnInterfaceDecl &sn)
{
	//Interface method signatures have no bodies; type resolution mirrors
	//class methods (handled by StatementResolver walking the members).
	sn.AddFlags(NF_Resolved);
}

void ExprResolveAccessor::Access(SnClassField &sn)
{
	//Class field type resolution is handled by StatementResolver.
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

	expr.EvalDataType(pField->EvalDataType());
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
	bool bFoundByName = false;
	auto &sFuncName = invoke.CalleeName();

	//Search a single scope's NameDict for matching functions.
	auto searchScope = [&](SnFunctionParentField& parent) -> FindFuncResult {
		auto range = parent.Members().NameDict().equal_range(sFuncName);
		for (auto iField = range.first; iField != range.second; ++iField) {
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
			if (nDistance < 0 || nDistance > n)
			{
				pFuncFound = pFunc;
				nDistance = n;
			}
		}
		return FFR_FuncNameNotFound;
	};

	SyntaxNode *pParent = m_pContext;
	while (pParent)
	{
		if (CanBeFuncParentEx(pParent->Kind()))
		{
			auto pParentType = static_cast<SnFunctionParentField*>(pParent);
			if (searchScope(*pParentType) == FFR_ExactMatch)
				return FFR_ExactMatch;

			//For class contexts, also search the inheritance chain
			//when the method is not found in the current class's Members().
			if (pParent->Kind() == NK_ClassDecl && !bFoundByName)
			{
				auto *pSuper = static_cast<SnClassDecl*>(pParent)->SuperClass();
				while (pSuper && !bFoundByName)
				{
					if (searchScope(*pSuper) == FFR_ExactMatch)
						return FFR_ExactMatch;
					pSuper = pSuper->SuperClass();
				}
			}
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
			return -1;
		if (iFormal == iFormalEnd)
			return -1;
		nDistance +=
			CalcTypeDistance(*concret.EvalDataType(), *iFormal->EvalDataType());
		++iFormal;
	}
	//Too few arguments — formal params remaining
	if (iFormal != iFormalEnd)
		return -1;
	return nDistance;
}

int ExprResolveAccessor::CalcTypeDistance(const SnField &source,
	const SnField &target) const
{
	if (&source == &target)
		return 0;
	auto srcKind = source.Kind();
	auto tgtKind = target.Kind();
	if (srcKind == NK_EnumDecl) srcKind = NK_Int32;
	if (tgtKind == NK_EnumDecl) tgtKind = NK_Int32;
	if (srcKind == NK_StructDecl && tgtKind == NK_StructDecl)
		return (&source == &target) ? 0 : -1;
	if (srcKind == NK_StructDecl || tgtKind == NK_StructDecl)
		return -1;
	if (srcKind == NK_ClassDecl && tgtKind == NK_ClassDecl)
	{
		if (&source == &target)
			return 0;
		auto *pSrc = static_cast<const SnClassDecl*>(&source);
		auto *pParent = pSrc->SuperClass();
		int depth = 1;
		while (pParent)
		{
			if (pParent == &target)
				return depth;
			pParent = pParent->SuperClass();
			++depth;
		}
		return -1;
	}
	//Class to interface: walk source class's inheritance chain and check
	//each ancestor's implements list. Distance is 1 + inheritance depth
	//(encourage upcast to direct implementor over a deeper ancestor's
	//implementation, but still accept any depth).
	if (srcKind == NK_ClassDecl && tgtKind == NK_InterfaceDecl)
	{
		auto *pSrc = static_cast<const SnClassDecl*>(&source);
		auto *pCur = pSrc;
		int depth = 0;
		while (pCur)
		{
			for (auto *pIface : pCur->ImplementsList())
				if (pIface == &target)
					return depth + 1;
			pCur = pCur->SuperClass();
			++depth;
		}
		return -1;
	}
	//Interface to interface: identity only (no inheritance between interfaces).
	if (srcKind == NK_InterfaceDecl && tgtKind == NK_InterfaceDecl)
		return (&source == &target) ? 0 : -1;
	//Interface to class is never valid — interface refs cannot be downcast
	//implicitly (no dynamic cast in this phase).
	if (srcKind == NK_InterfaceDecl || tgtKind == NK_InterfaceDecl)
		return -1;
	if (srcKind == NK_ClassDecl || tgtKind == NK_ClassDecl)
		return -1;
	if (IsPrimitiveType(srcKind) && IsPrimitiveType(tgtKind))
		return std::abs(srcKind - tgtKind);
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

	auto pSrcParent = srcExpr.Parent();
	assert(pSrcParent);

	auto iInsertPos = RemoveChildFrom(iSrcExpr, *pSrcParent);
	assert(srcExpr.Location());
	auto pCastExpr =
		new SnCastExpr(&srcExpr, castInfo, *srcExpr.Location());
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

bool ExprResolver::ResolveDataType(SnFieldExpr &typeExpr, SnField &outerType)
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
