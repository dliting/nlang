/*---
    ExprResolverValues.cpp — 值位节点解析
    从 ExprResolver.cpp 抽取（2026-09-26 可维护性重构，零行为变化）。
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
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

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

	//Builtin class names: ByteStream, FileStream, Object (Phase 8e-1).
	//Phase 9d: Exception hierarchy (Exception, NullPointerException,
	//DivByZeroException, IndexOutOfBoundsException, AssertionException).
	//When used as a type name (e.g. "ByteStream s = ..."), the name
	//doesn't exist in the AST namespace. Synthesize a singleton SnClassDecl.
	if (!pFieldExpr->IsResolved())
	{
		const auto& name = pFieldExpr->ToString();
		if (IsBuiltinClassName(name))
		{
			ResolveFieldExprAs(*pFieldExpr,
				GetBuiltinClassDecl(name, pFieldExpr->Location()));
		}
	}

	if (!pFieldExpr->IsResolved())
		return;

	ResolveFieldExprAs(nameExpr, pFieldExpr->Field());
}

//Phase 9c: SnNamedArgExpr resolver. The name is consumed by TryBindInvoke
//when matching formals; here we only need to resolve the inner expression
//and propagate its EvalDataType / resolved flag so the parent invoke can
//type-check the binding.
void ExprResolveAccessor::Access(SnNamedArgExpr &sn)
{
	assert(!sn.IsResolved());
	auto *pInner = sn.Inner();
	assert(pInner);
	pInner->Accept(*m_pVisitor);
	if (!pInner->IsResolved())
		return;
	sn.EvalDataType(pInner->EvalDataType());
	sn.AddFlags(NF_Resolved);
}

//Phase 9e: SnOutArgExpr resolver. Grammar restricts the inner to a plain
//identifier; here we additionally require it to bind to a caller-frame
//slot — a local variable or a formal parameter of the calling function
//(both register as NK_FormalParam-kind fields). Class fields (implicit
//this.x), enum members and globals are rejected: the writeback copies
//the callee's out slot to a frame offset only.
void ExprResolveAccessor::Access(SnOutArgExpr &sn)
{
	assert(!sn.IsResolved());
	auto *pInner = sn.Inner();
	assert(pInner);
	if (pInner->Kind() != NK_IdentifierExpr)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"out argument must be a plain local variable.");
		return;
	}
	pInner->Accept(*m_pVisitor);
	if (!pInner->IsResolved())
		return;
	auto *pField = static_cast<SnIdentifierExpr*>(pInner)->Field();
	if (!pField || pField->Kind() != NK_FormalParam)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"out argument \"%s\" must be a local variable or parameter "
			"of the calling function.",
			pInner->ToString().c_str());
		return;
	}
	sn.EvalDataType(pInner->EvalDataType());
	sn.AddFlags(NF_Resolved);
}

void ExprResolveAccessor::Access(SnCastExpr &sn)
{
}

//Phase 8e-1.5: resolve `expr as T` runtime-checked cast.
//Valid kinds: TCK_Same (no-op), TCK_Box (primitive→Object), TCK_Unbox (Object→primitive),
//TCK_Downcast (ancestor→subclass). Other kinds → compile error.
void ExprResolveAccessor::Access(SnAsExpr &sn)
{
	assert(!sn.IsResolved());

	//Resolve operand first (its EvalDataType is needed for cast computation).
	sn.Operand()->Accept(*m_pVisitor);
	if (!sn.Operand()->IsResolved())
		return;

	//Resolve target type name (its Field() will be the target SnField*).
	sn.TargetType()->Accept(*m_pVisitor);
	if (!sn.TargetType()->IsResolved())
		return;

	auto *pSrcType = sn.Operand()->EvalDataType();
	auto *pTgtType = sn.TargetType()->Field();
	if (!pSrcType || !pTgtType)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Cannot resolve types for `as` expression.");
		return;
	}

	if (RejectAsArrayOperand(sn))
		return;

	TypeCastInfo castInfo(pSrcType, pTgtType);
	auto kind = castInfo.Kind();

	if (ResolveAsCastKind(sn, pSrcType, pTgtType, kind))
		return;

	sn.SetResolved(pTgtType, kind);
	//EvalDataType must be the target type itself (SnInt32 for `as int`,
	//SnClassDecl for `as Foo`). Calling pTgtType->EvalDataType() would
	//yield SnType::Instance() (the type-of-type) since type-name fields
	//like SnInt32 are SnBuiltinDataType whose EvalDataType() is SnType,
	//and downstream cast checks would fail with TCK_None.
	sn.EvalDataType(pTgtType);
	//`null as T` is still the null literal semantically: propagate the
	//flag so downstream gates (the null-identity skip in FixupExprType,
	//the boxing guard family) see through the cast wrapper. ContainFlags
	//is a flat bit test — wrapper nodes do not inherit child flags.
	if (sn.Operand()->ContainFlags(NF_NullLiteral))
		sn.AddFlags(NF_NullLiteral);
}

//2026-09-26 decomposition of Access(SnAsExpr&): the 0.7.2 array-operand
//guard as its own named phase. Returns true after logging the rejection.
bool ExprResolveAccessor::RejectAsArrayOperand(SnAsExpr &sn)
{
	//`as` has no array-typed target spelling (the target is a name
	//expression), and string targets are assignment-only coercions — so
	//no legal form exists for an array-valued operand. Post-0.7.3 B the
	//cast table would reject every spelling anyway (token×scalar =
	//None); the named branch keeps the diagnostic specific.
	if (!sn.Operand()->IsArrayValued())
		return false;
	m_Env.Log(CLL_Error, sn.Location(),
		"Invalid cast \"%s as %s\": the cast operand is an array.",
		sn.Operand()->ToString().c_str(),
		sn.TargetType()->ToString().c_str());
	return true;
}

//2026-09-26 decomposition of Access(SnAsExpr&): the cast-kind
//adjudication middle. Returns true when the expression is consumed —
//resolved as the func→string rendering, or rejected with a named
//diagnostic (TCK_None / implicit-conversion kinds); false means the
//kind is Same/Box/Unbox/Downcast and the caller finishes the binding.
bool ExprResolveAccessor::ResolveAsCastKind(SnAsExpr &sn, SnField *pSrcType,
	SnField *pTgtType, TypeCastKind kind)
{
	//Phase 13: `f as string` renders the handle ("func <name>"). Class→
	//string is normally an assignment-only coercion (TCK_Auto is rejected
	//for `as`); function handles get an explicit branch so all four
	//conversion paths agree (codegen emits OP_Func_to_str).
	if (pSrcType->Kind() == NK_ClassDecl && pTgtType->Kind() == NK_String
		&& static_cast<SnClassDecl*>(pSrcType)->IsFuncType())
	{
		sn.SetResolved(pTgtType, TCK_Auto);
		sn.EvalDataType(pTgtType);
		return true;
	}

	if (kind == TCK_None)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"Invalid cast: `%s as %s` is not allowed.",
			pSrcType->ToString().c_str(),
			pTgtType->ToString().c_str());
		return true;
	}

	//TCK_Auto (e.g. int→float) is not allowed via `as` — use primitive cast syntax.
	//TCK_Dynamic similarly. Only TCK_Same/Box/Unbox/Downcast are valid.
	if (kind != TCK_Same && kind != TCK_Box && kind != TCK_Unbox && kind != TCK_Downcast)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"`as` cannot perform implicit conversion `%s` → `%s`.",
			pSrcType->ToString().c_str(),
			pTgtType->ToString().c_str());
		return true;
	}
	return false;
}

//0.7.3 B D3: a string base has no subscript semantics (NLang has
//no char type — the substring methods are the char-access surface).
//Before this arm the subscript silently resolved to the string
//itself and codegen read the index as an array handle, failing
//only at runtime ("null array access"). True = rejected.
bool ExprResolveAccessor::RejectStringSubscriptBase(SnSubscriptExpr &sn,
	SnField *pBaseType)
{
	if (pBaseType && pBaseType->Kind() == NK_String)
	{
		m_Env.Log(CLL_Error, sn.Location(),
			"string does not support subscript access.");
		return true;
	}
	return false;
}

//List<T>/Dict<K,V> subscript (li[i] / d[k]): sugar over get().
//The base resolves to a synthetic generic-instantiation class; the
//element type is T (List) or V (Dict). Without this peel the
//subscript keeps the container type and every consumer (assignment,
//member chains, nested subscripts) mis-types it.
//
//0.7.3 B: an array-valued base carries the interned token in
//EvalDataType (NK_ArrayTypeToken, never a ClassDecl), so the Kind()
//check alone distinguishes `List<int>[] a` from `List<int> li` —
//the masquerade-era IsPlainLvalueShape guard here was removed with
//the side channel. Array bases keep the plain element-type
//propagation in the caller, peeling the token.
//True = consumed (resolved as the container's element type).
bool ExprResolveAccessor::TryResolveContainerSubscript(SnSubscriptExpr &sn,
	SnField *pBaseType)
{
	if (!pBaseType || pBaseType->Kind() != NK_ClassDecl)
		return false;
	auto* pClass = static_cast<SnClassDecl*>(pBaseType);
	if (!pClass->IsGenericInstantiation())
		return false;
	const auto& baseName = pClass->BaseName();
	const auto& typeArgs = pClass->GenericTypeArgs();
	SnField* elem = nullptr;
	if (baseName == "List" && !typeArgs.empty())
		elem = typeArgs[0];
	else if (baseName == "Dict" && typeArgs.size() > 1)
		elem = typeArgs[1];
	if (!elem)
		return false;
	sn.EvalDataType(elem);
	sn.AddFlags(NF_Resolved);
	BindArrayTypeToken(sn);
	return true;
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
	if (RejectStringSubscriptBase(sn, arrayType))
		return;
	if (TryResolveContainerSubscript(sn, arrayType))
		return;
	if (arrayType)
	{
		//0.7.3 B token path: an
		//array-valued base carries the interned array token — the
		//subscript's own type is its ELEMENT.
		if (arrayType->Kind() == NK_ArrayTypeToken)
			arrayType = static_cast<SnArrayTypeToken*>(
				arrayType)->ElemTypeOf();
		sn.EvalDataType(arrayType);
	}
	sn.AddFlags(NF_Resolved);
	BindArrayTypeToken(sn);
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
			//Phase 12: enum methods. `this` is the enum value — the decl
			//masquerades as Int32 at runtime (SnEnumDecl::EvalDataType),
			//so arithmetic and switch on `this` work unchanged.
			if (pParent && pParent->Kind() == NK_EnumDecl)
			{
				sn.EvalDataType(static_cast<SnEnumDecl*>(pParent));
				sn.AddFlags(NF_Resolved);
				return;
			}
		}
		pContext = pContext->Parent();
	}
	m_Env.Log(CLL_Error, sn.Location(),
		"'this' can only be used inside a class or enum method.");
}

} //namespace nlang
