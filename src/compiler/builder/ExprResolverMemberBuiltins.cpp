/*---
    ExprResolverMemberBuiltins.cpp — 内建按名方法族阶段：string 方法 / 数组 length() 方法 / 流方法。
    从 ExprResolver.cpp 抽取（2026-09-25 可维护性重构，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "SnArrayTypeToken.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include "ModuleRegistry.h"

namespace nlang
{

//Formal count of a declared face method. Overloads are separate
//members (substring declares twice), so callers match by count.
static size_t FormalCountOf(const SnFunction &fn)
{
	size_t count = 0;
	for (auto& param : fn.Params())
		++count;
	return count;
}

//Resolved type of the i-th formal. The ResolveDataTypes pre-pass
//resolves the prelude signatures before any function body, so the
//Field() link is live by the time a face bridge reads it.
static SnField *FormalTypeAt(const SnFunction &fn, size_t index)
{
	size_t i = 0;
	for (auto& param : fn.Params())
		if (i++ == index)
			return param.Type()->Field();
	return nullptr;
}

//Match a face method by name and exact formal count. Overloads are
//separate members (substring declares twice); *ppOtherArity receives
//the first same-name candidate with a different count, for the arity
//error. Null with *ppOtherArity null = no same-name member at all.
static SnFunction *MatchFaceMethod(SnClassDecl &face,
	const std::string &name, size_t argCount, SnFunction **ppOtherArity)
{
	*ppOtherArity = nullptr;
	for (auto& child : face.Members())
	{
		if (child.Kind() != NK_Function)
			continue;
		auto& fn = static_cast<SnFunction&>(child);
		if (fn.Name() != name)
			continue;
		if (FormalCountOf(fn) == argCount)
			return &fn;
		if (!*ppOtherArity)
			*ppOtherArity = &fn;
	}
	return nullptr;
}

//String method face, declaration-driven (v0.12.2): the prelude class
//String owns every signature; this bridge validates against those
//declarations and binds result types from the declared returns. The
//emitter keeps its name-keyed fast paths — the javac model:
//declarations are the interface, opcodes/intrinsics the implementation.
bool ExprResolveAccessor::TryResolveStringBuiltinMethod(
	SnMemberExpr &snMember, SnFieldExpr *pInnerExpr,
	SyntaxNode *pSavedContext)
{
	if (!(m_pContext && m_pContext->Kind() == NK_String
		&& pInnerExpr->Kind() == NK_InvokeExpr))
		return false;
	auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	const auto& name = invoke.CalleeName();
	//Anchor is prelude IDENTITY, never the bare name: a user shadow of
	//String never hijacks keyword-typed receivers.
	SnClassDecl* pFace = FindPreludeClassDecl(m_Env.Registry(), "String");
	if (!pFace)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"internal: prelude String face class is missing.");
		m_pContext = pSavedContext;
		return true;
	}
	SnFunction* pOtherArity = nullptr;
	SnFunction* pMatch = MatchFaceMethod(*pFace, name, ArgCountOf(invoke),
		&pOtherArity);
	if (!pMatch)
	{
		if (!pOtherArity)
			return false;   //not a face method — generic diagnostics
		m_Env.Log(CLL_Error, invoke.Location(),
			"string.%s expects %d argument(s).", name.c_str(),
			(int)FormalCountOf(*pOtherArity));
		m_pContext = pSavedContext;
		return true;
	}
	if (RejectNamedOrOutArguments(invoke))
	{
		m_pContext = pSavedContext;
		return true;
	}
	//Args resolve in the caller's scope (the equals recipe: restore the
	//context and drop the parent-only search first, or the argument
	//stays unresolved and codegen silently compares stale memory).
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	CheckFaceArgKinds(invoke, *pMatch, "string");
	BindFaceMethodResult(snMember, pInnerExpr, *pMatch);
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}

//Per-arg policy from the DECLARED formals (v0.12.2): exact kind match,
//no widening — substring offsets are int; a float offset is a compile
//error. Null matches a string formal: null strings read as "" in this
//language, so a null argument is the empty-string argument. Array
//tokens carry their own Kind and match no scalar formal.
void ExprResolveAccessor::CheckFaceArgKinds(SnInvokeExpr &invoke,
	SnFunction &fn, const char *ownerName)
{
	auto& children = invoke.Children();
	size_t paramIdx = 0;
	for (auto it = children.begin(); it != children.end();
		++it, ++paramIdx)
	{
		auto& arg = static_cast<SnExpression&>(*it);
		auto* pArgType = arg.EvalDataType();
		if (!pArgType)
		{
			if (arg.IsResolved())
				m_Env.Log(CLL_Error, arg.Location(),
					"Argument %d of %s.%s has no value: a void "
					"function result cannot be used as an argument.",
					(int)paramIdx + 1, ownerName,
					fn.Name().c_str());
			continue;
		}
		auto* pFormalType = FormalTypeAt(fn, paramIdx);
		if (!pFormalType)
			continue;
		//The null literal is an Int32-typed node flagged NF_NullLiteral
		//— detection is the flag (the same test every other resolver
		//site uses), never a kind compare.
		const bool ok = pArgType->Kind() == pFormalType->Kind()
			|| (arg.ContainFlags(NF_NullLiteral)
				&& pFormalType->Kind() == NK_String);
		if (!ok)
			m_Env.Log(CLL_Error, arg.Location(),
				"Argument %d of %s.%s has type \"%s\"; \"%s\" "
				"expected.", (int)paramIdx + 1, ownerName,
				fn.Name().c_str(), pArgType->ToString().c_str(),
				pFormalType->ToString().c_str());
	}
}

//Resolve tail shared by the face bridges: resolved flag, the DECLARED
//return type bound as EvalDataType + m_pField so a chained member
//(s.substring(1).toUpper()) survives IsDataExpr(). An array-typed
//return is re-interned in the CALLER's TU — token identity is
//per-TU pointer equality, a prelude TU's token must not leak.
void ExprResolveAccessor::BindFaceMethodResult(SnMemberExpr &snMember,
	SnFieldExpr *pInnerExpr, SnFunction &fn)
{
	pInnerExpr->AddFlags(NF_Resolved);
	//fn.ReturnType() is the declared return EXPRESSION; its resolved
	//field rides the Field() link. Every face method declares a return
	//type and the pre-pass resolves prelude signatures before any body
	//— a null here means the prelude itself is corrupt.
	SnField* pReturnType = fn.ReturnType()
		? fn.ReturnType()->Field() : nullptr;
	assert(fn.ReturnType() && pReturnType);
	if (pReturnType && pReturnType->Kind() == NK_ArrayTypeToken)
	{
		//Re-intern in the caller's environment (per-TU identity).
		auto* pToken = static_cast<SnArrayTypeToken*>(pReturnType);
		pReturnType = m_Env.InternArrayTypeToken(
			pToken->ElemTypeOf(), pToken->Rank());
	}
	if (pReturnType)
	{
		snMember.EvalDataType(pReturnType);
		snMember.m_pField = pReturnType;
	}
}

//Builtin array length() method. The receiver check widens from
//identifier-shape to the array-valued property (any bound shape —
//identifier, member like li.get(0), or call result like mk()).
//Three shapes: a zero-argument call resolves; a call with arguments
//and the old property spelling report dedicated errors (the member
//resolves to the error verdict here, so the dedicated message is
//always the first diagnostic).
bool ExprResolveAccessor::TryResolveArrayLengthMethod(
	SnMemberExpr &snMember, SnExpression *pOuterExpr,
	SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext)
{
	if (!pOuterExpr->IsArrayValued())
		return false;
	if (pInnerExpr->Kind() == NK_InvokeExpr)
	{
		auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
		return TryResolveArrayBuiltinInvoke(snMember, pInnerExpr,
			invoke, pSavedContext);
	}
	if (pInnerExpr->Kind() == NK_IdentifierExpr
		&& static_cast<SnIdentifierExpr*>(pInnerExpr)->Name()
			== "length")
	{
		m_Env.Log(CLL_Error, pInnerExpr->Location(),
			"'length' on an array is a method; call it as '.length()'.");
		m_pContext = pSavedContext;
		return true;
	}
	return false;
}

//The array-builtin invoke arm (length/rank/shape) of
//TryResolveArrayLengthMethod: a zero-argument call on an
//array-valued receiver resolves; per-name arity message and verdict
//(rank: int, shape: int[] minted over the int builtin at rank 1 —
//the same intern channel as every array type). Parallel blocks
//preempted each other once already; new array builtins extend the
//name dispatch here, they do not get a copied block.
bool ExprResolveAccessor::TryResolveArrayBuiltinInvoke(
	SnMemberExpr &snMember, SnFieldExpr *pInnerExpr,
	SnInvokeExpr &invoke, SyntaxNode *pSavedContext)
{
	const bool isRank = invoke.CalleeName() == "rank";
	const bool isShape = invoke.CalleeName() == "shape";
	if (invoke.CalleeName() != "length" && !isRank && !isShape)
		return false;
	if (invoke.Params().begin() != invoke.Params().end())
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			isRank ? "array.rank() takes no arguments."
			: isShape ? "array.shape() takes no arguments."
			: "array.length() takes no arguments.");
		m_pContext = pSavedContext;
		return true;
	}
	//Mirror of the string length/getHashCode arm: same flag set,
	//plus the m_pField bind so a chained member
	//(a.length().toString(), a.shape()[0]) survives IsDataExpr().
	pInnerExpr->AddFlags(NF_Resolved);
	auto* pInt = SnBuiltinDataType::InstanceOf(NK_Int32);
	SnField* pResultType = pInt;
	if (isShape)
		pResultType = static_cast<SnField*>(
			m_Env.InternArrayTypeToken(pInt, 1));
	snMember.EvalDataType(pResultType);
	snMember.m_pField = pResultType;
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}


} //namespace nlang
