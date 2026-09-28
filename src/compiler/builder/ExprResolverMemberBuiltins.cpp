/*---
    ExprResolverMemberBuiltins.cpp — 内建按名方法族阶段：string 方法 / 数组 .length / ByteStream·FileStream 流方法。
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
#include <nlang/vm/StdLib.h>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nlang
{

//Builtin string methods: s.length(), s.GetHashCode(), s.Equals(other).
bool ExprResolveAccessor::TryResolveStringBuiltinMethod(
	SnMemberExpr &snMember, SnFieldExpr *pInnerExpr,
	SyntaxNode *pSavedContext)
{
	if (!(m_pContext && m_pContext->Kind() == NK_String
		&& pInnerExpr->Kind() == NK_InvokeExpr))
		return false;
	auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	const auto& name = invoke.CalleeName();
	//Phase 8e-1: string.length()/getHashCode() — value semantics,
	//intrinsified in VmBackend; both are zero-argument int returns
	//(identical resolution, one arm).
	if ((name == "length" || name == "getHashCode")
		&& invoke.Params().begin() == invoke.Params().end())
	{
		pInnerExpr->AddFlags(NF_Resolved);
		snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		m_pContext = pSavedContext;
		return true;
	}
	if (name == "equals")
		return ResolveStringEqualsMethod(snMember, pInnerExpr, invoke,
			pSavedContext);
	if (const StringMethodEntry* pMethod = FindStringMethod(name))
		return TryResolveTableStringMethod(snMember, pInnerExpr, invoke,
			name, pSavedContext);
	//Phase 8e-9b: string.toString() — identity. Resolver folds the call
	//to a no-op (callee=null, EvalDataType=String). Codegen emits nothing
	//and the inner string idx flows through unchanged.
	if (name == "toString" && invoke.Params().begin() == invoke.Params().end())
	{
		pInnerExpr->AddFlags(NF_Resolved);
		snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_String));
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		//Mark the invoke as folded so codegen skips it. Use NF_Resolved flag
		//on the inner expression (already set above) and leave callee as-is;
		//VmBackend detects string receiver + toString name and emits nothing.
		m_pContext = pSavedContext;
		return true;
	}
	//Not consumed: m_pContext stays on the receiver context — the next
	//phases' guards read it (an early restore here would misdispatch them).
	return false;
}

//string.Equals(other) — value semantics, args resolve in the caller's scope.
bool ExprResolveAccessor::ResolveStringEqualsMethod(SnMemberExpr &snMember,
	SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke, SyntaxNode *pSavedContext)
{
	if (RejectNamedOrOutArguments(invoke))
	{
		m_pContext = pSavedContext;
		return true;
	}
	if (ArgCountOf(invoke) != 1)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"string.equals requires exactly 1 argument.");
		m_pContext = pSavedContext;
		return true;
	}
	//Round-11: args must resolve in the CALLER's scope — same recipe
	//as the user-class equals path below. Without this the argument
	//stayed unresolved and codegen's silent fallbacks (ConstZero /
	//skipped call) made t.equals(t) compare against stale memory.
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	pInnerExpr->AddFlags(NF_Resolved);
	snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}

//Table-driven string method call shape check: shared by-name rejection plus
//the arity window. Logs on invalid; returns true when the call is valid.
bool ExprResolveAccessor::CheckTableStringMethodCall(SnInvokeExpr &invoke,
	const StringMethodEntry *pMethod)
{
	if (RejectNamedOrOutArguments(invoke))
		return false;
	const size_t argCount = ArgCountOf(invoke);
	if (argCount < pMethod->minArgs || argCount > pMethod->maxArgs)
	{
		if (pMethod->minArgs == pMethod->maxArgs)
			m_Env.Log(CLL_Error, invoke.Location(),
				"string.%s expects %d argument(s).",
				pMethod->name, (int)pMethod->minArgs);
		else
			m_Env.Log(CLL_Error, invoke.Location(),
				"string.%s expects %d to %d argument(s).",
				pMethod->name, (int)pMethod->minArgs,
				(int)pMethod->maxArgs);
		return false;
	}
	return true;
}

//Per-arg policy: exact kind match vs paramKinds, no widening
//(substring offsets are int; a float offset is a compile
//error). Array-valued args carry the interned array token
//whose Kind matches no scalar paramKind — the kind match
//below rejects them with the generic diagnostic. Same shape
//as the namespace-call path: a void call has no value.
void ExprResolveAccessor::CheckTableStringArgKinds(SnInvokeExpr &invoke,
	const StringMethodEntry *pMethod)
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
					"Argument %d of string.%s has no value: a void "
					"function result cannot be used as an argument.",
					(int)paramIdx + 1, pMethod->name);
			continue;
		}
		const uint8_t want = pMethod->paramKinds[paramIdx];
		const bool ok =
			(pArgType->Kind() == NK_Int32 && want == RTK_Int32)
			|| (pArgType->Kind() == NK_String && want == RTK_String);
		if (!ok)
		{
			m_Env.Log(CLL_Error, arg.Location(),
				"Argument %d of string.%s has type \"%s\"; \"%s\" "
				"expected.",
				(int)paramIdx + 1, pMethod->name,
				pArgType->ToString().c_str(), StdLibKindName(want));
			continue;
		}
	}
}

//Resolve tail of a table-driven string method: resolved flag, return type,
//and the m_pField bind that lets chained access survive IsDataExpr().
void ExprResolveAccessor::BindTableStringMethodResult(SnMemberExpr &snMember,
	SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke,
	const StringMethodEntry *pMethod)
{
	pInnerExpr->AddFlags(NF_Resolved);
	SnField* pResultField = nullptr;
	switch ((StdLibReturnType)pMethod->returnType)
	{
	case SLRT_Int32:
		pResultField = SnBuiltinDataType::InstanceOf(NK_Int32);
		break;
	case SLRT_Bool:   //0.7.5: predicates
		pResultField = SnBuiltinDataType::InstanceOf(NK_Bool);
		break;
	case SLRT_Float:
		pResultField = SnBuiltinDataType::InstanceOf(NK_Float);
		break;
	case SLRT_String:
		pResultField = SnBuiltinDataType::InstanceOf(NK_String);
		break;
	case SLRT_ListString:
	{
		std::vector<SnField*> listArgs{
			SnBuiltinDataType::InstanceOf(NK_String) };
		pResultField = GetGenericClassDecl("List", listArgs, {},
			invoke.Location());
		break;
	}
	case SLRT_Void:
		break;
	}
	if (pResultField)
	{
		snMember.EvalDataType(pResultField);
		//m_pField directly (not via ResolveFieldExprAs) so chained
		//access (s.substring(1).toUpper()) survives IsDataExpr() —
		//same rationale as the stdlib path.
		snMember.m_pField = pResultField;
	}
}

//Phase 11 Step 3: table-driven built-in string methods (12 new;
//equals/getHashCode above keep their 8e-1 ids). The method surface
//is frozen as the future string class's methods (user decision #6).
bool ExprResolveAccessor::TryResolveTableStringMethod(SnMemberExpr &snMember,
	SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke, const std::string &name,
	SyntaxNode *pSavedContext)
{
	const StringMethodEntry* pMethod = FindStringMethod(name);
	if (!pMethod)
		return false;
	if (!CheckTableStringMethodCall(invoke, pMethod))
	{
		m_pContext = pSavedContext;
		return true;
	}
	//Args resolve in the caller's scope (equals recipe: restore the
	//context and drop the parent-only search first).
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	CheckTableStringArgKinds(invoke, pMethod);
	BindTableStringMethodResult(snMember, pInnerExpr, invoke, pMethod);
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
	return true;
}

//Builtin array.length property. Array redesign B: the receiver
//check widens from identifier-shape to the array-valued property
//(any bound shape — identifier, member like li.get(0), or call
//result like mk()/lib.mk(3)).
bool ExprResolveAccessor::TryResolveArrayLengthProperty(
	SnMemberExpr &snMember, SnExpression *pOuterExpr,
	SnFieldExpr *pInnerExpr, SyntaxNode *pSavedContext)
{
	if (pOuterExpr->IsArrayValued()
		&& pInnerExpr->Kind() == NK_IdentifierExpr
		&& static_cast<SnIdentifierExpr*>(pInnerExpr)->Name()
			== "length")
	{
		pInnerExpr->AddFlags(NF_Resolved);
		snMember.EvalDataType(SnBuiltinDataType::InstanceOf(NK_Int32));
		snMember.AddFlags(NF_Resolved);
		BindArrayTypeToken(snMember);
		m_pContext = pSavedContext;
		return true;
	}
	return false;
}

//Name ladder for the plain stream methods (everything except
//readStruct/readObject, whose type-name argument needs special
//resolution). retKind defaults to NK_Int32 at the call site;
//void-returning names leave it untouched (EvalDataType stays unset).
static bool ClassifyStreamMethod(const std::string &name, NodeKind &retKind)
{
	if (name == "readInt" || name == "length" || name == "position")
		return true;  // retKind = NK_Int32
	if (name == "readFloat")
		{ retKind = NK_Float; return true; }
	if (name == "readString")
		{ retKind = NK_String; return true; }
	if (name == "writeInt" || name == "writeFloat"
		|| name == "writeString" || name == "reset" || name == "close"
		|| name == "writeStruct" || name == "writeObject")
		return true;  // void return — no EvalDataType
	return false;
}

//ReadStruct("TypeName")/ReadObject("TypeName") type-name argument: the
//argument MUST be a string literal so the type resolves at compile time
//(variables rejected — no generics in NLang). The lookup walks the CALLER's
//namespace chain, NOT m_pContext — that is the synthesized builtin stream
//class, whose Parent() is null, so the walk would never reach the user's
//translation-unit scope where structs/classes are declared.
//Returns nullptr after logging (context restored); readObject's declared
//type may be a base class of the stream's actual type — polymorphic
//deserialization is enforced in VmExecutor via IsSubclassOf (Phase 8d).
SnField *ExprResolveAccessor::ResolveStreamSpecialTypeArg(
	SnInvokeExpr &invoke, NodeKind wantKind, const char *pMethodDisp,
	SyntaxNode *pSavedContext)
{
	auto& params = invoke.Params();
	auto it = params.begin();
	if (it == params.end() || (*it).Kind() != NK_LiteralExpr
		|| !(*it).EvalDataType()
		|| (*it).EvalDataType()->Kind() != NK_String)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"%s requires a string literal argument.", pMethodDisp);
		m_pContext = pSavedContext;
		return nullptr;
	}
	auto& lit = static_cast<SnLiteralExpr&>(*it);
	const std::string* pTypeName = lit.Value().Data().m_String;
	const std::string typeName = pTypeName ? *pTypeName : std::string();
	SnField* found = nullptr;
	auto* ctx = pSavedContext;
	while (ctx && !found)
	{
		found = ctx->FindField(typeName);
		ctx = ctx->Parent();
	}
	if (!found || found->Kind() != wantKind)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"%s type not found: %s.", pMethodDisp, typeName.c_str());
		m_pContext = pSavedContext;
		return nullptr;
	}
	return found;
}

//Builtin stream methods: ByteStream/FileStream member calls.
//These are resolved by name since the synthesized SnClassDecl has
//no real method members. The return type is determined by method name.
bool ExprResolveAccessor::TryResolveStreamBuiltinMethod(
	SnMemberExpr &snMember, SnFieldExpr *pInnerExpr,
	SyntaxNode *pSavedContext)
{
	if (!(m_pContext && m_pContext->Kind() == NK_ClassDecl
		&& static_cast<SnClassDecl*>(m_pContext)->IsBuiltinClass()
		&& pInnerExpr->Kind() == NK_InvokeExpr))
		return false;
	auto& invoke = static_cast<SnInvokeExpr&>(*pInnerExpr);
	const auto& name = invoke.CalleeName();
	bool isStreamMethod = false;
	NodeKind retKind = NK_Int32;  //default, overridden below
	isStreamMethod = ClassifyStreamMethod(name, retKind);
	if (name == "readStruct")
	{
		//Returns a struct value of the named type.
		isStreamMethod = true;
		SnField* pFound = ResolveStreamSpecialTypeArg(invoke,
			NK_StructDecl, "ReadStruct", pSavedContext);
		if (!pFound)
			return true;
		snMember.EvalDataType(pFound);
	}
	else if (name == "readObject")
	{
		//Returns a class object of the named type.
		isStreamMethod = true;
		SnField* pFound = ResolveStreamSpecialTypeArg(invoke,
			NK_ClassDecl, "ReadObject", pSavedContext);
		if (!pFound)
			return true;
		snMember.EvalDataType(pFound);
	}
	if (!isStreamMethod)
		return false;
	ResolveStreamMethodTail(snMember, pInnerExpr, invoke, name,
		retKind, pSavedContext);
	return true;
}

//Stream method resolve tail: shared by-name rejection, caller-scope arg
//resolution, and the per-name return kind (void-returning writers leave
//EvalDataType unset; readStruct/readObject already set theirs above).
void ExprResolveAccessor::ResolveStreamMethodTail(SnMemberExpr &snMember,
	SnFieldExpr *pInnerExpr, SnInvokeExpr &invoke, const std::string &name,
	NodeKind retKind, SyntaxNode *pSavedContext)
{
	//Round-12/14: by-name dispatch cannot bind named args; out args
	//cannot write back.
	if (RejectNamedOrOutArguments(invoke))
	{
		m_pContext = pSavedContext;
		return;
	}
	//Resolve the args so each param's Field()/EvalDataType() is
	//populated (e.g. struct-typed IdentifierExpr needs Field() set
	//so VmBackend can emit the correct load opcode). Without this,
	//the early return below skips arg resolution entirely and the
	//backend falls through to const_zero for unresolved idents.
	//
	//Args are evaluated in the CALLER's scope, not the synthesized
	//builtin-class scope (which is empty). Restore m_pContext AND
	//clear ERF_SearchInParentOnly (set above for the member lookup)
	//so a normal scope walk finds the caller's locals.
	m_pContext = pSavedContext;
	RemoveFlags(ERF_SearchInParentOnly);
	ResolveExpressionList(invoke.Params());
	pInnerExpr->AddFlags(NF_Resolved);
	//For void-returning methods, leave EvalDataType unset.
	if (name != "writeInt" && name != "writeFloat"
		&& name != "writeString" && name != "reset" && name != "close"
		&& name != "writeStruct" && name != "writeObject")
	{
		//ReadStruct/ReadObject already set EvalDataType above; others use retKind.
		if (name != "readStruct" && name != "readObject")
			snMember.EvalDataType(SnBuiltinDataType::InstanceOf(retKind));
	}
	snMember.AddFlags(NF_Resolved);
	BindArrayTypeToken(snMember);
	m_pContext = pSavedContext;
}

} //namespace nlang
