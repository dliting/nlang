/*---
    ExprResolverStreamBuiltins.cpp — ByteStream/FileStream 内建方法按名解析族。
    从 ExprResolverMemberBuiltins.cpp 抽取（2026-09-30 可维护性重构，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "SnMisc.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include "BuildEnvironment.h"
#include "BuiltinNames.h"
#include <nlang/vm/StdLib.h>

namespace nlang
{
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
	if (name == "readLong")
		{ retKind = NK_Long; return true; }
	if (name == "readDouble")
		{ retKind = NK_Double; return true; }
	if (name == "readString")
		{ retKind = NK_String; return true; }
	if (name == "writeInt" || name == "writeFloat"
		|| name == "writeLong" || name == "writeDouble"
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

//Per-name value-argument expectation of the stream writers: returns
//the accepted-argument phrase for the diagnostic, or nullptr when the
//argument's kind is legal. Null is Int32-typed on the wire (KT_Null);
//only writeObject takes it (a null object reference serializes as the
//null marker).
static const char* StreamArgExpectation(const std::string &name,
	SnExpression &arg, bool nullLit)
{
	auto* pArgType = arg.EvalDataType();
	NodeKind ak = pArgType ? pArgType->Kind()
		: static_cast<NodeKind>(-1);
	if (name == "writeInt")
	{
		//Any integer category up to 32 bits: narrow rows ride the
		//value-extension convention, so the low 4 bytes are exact;
		//long/ulong would silently lose their high bits.
		int pi = ScalarPrimIndexOf(ak);
		bool ok = !nullLit && pi >= 0
			&& (kScalarPrims[pi].category == PC_SInt
				|| kScalarPrims[pi].category == PC_UInt)
			&& kScalarPrims[pi].slotWidth <= 4;
		return ok ? nullptr : "an integer of at most 32 bits";
	}
	if (name == "writeFloat")
		return (!nullLit && ak == NK_Float) ? nullptr : "a float";
	if (name == "writeString")
		return (!nullLit && ak == NK_String) ? nullptr : "a string";
	if (name == "writeStruct")
		return (!nullLit && ak == NK_StructDecl) ? nullptr : "a struct";
	if (name == "writeObject")
	{
		//Interface slots hold class references — serializing writes the
		//actual object (polymorphism is resolved at runtime).
		bool ok = nullLit || ak == NK_ClassDecl || ak == NK_InterfaceDecl;
		return ok ? nullptr : "a class";
	}
	return nullptr;
}

//0.7.5 Task 9 admission for the 8-byte writers (called with exactly
//one value argument in place): cast-matrix verdict with a widening
//wrap — the intrinsic reads 8 raw bytes off the value slot, and a
//4-byte kind leaves the slot's upper half stale, so narrower integers
//and floats MUST be normalized by an in-place PrimCast (the same
//admission math.sqrt arguments use). The null literal is Int32-typed
//on the wire; wrapping it would silently serialize 0, so reject it
//like the other writers do.
void ExprResolveAccessor::AdmitWideStreamWriterArg(SnInvokeExpr &invoke,
	const std::string &name)
{
	const bool wantLong = (name == "writeLong");
	auto it = invoke.Children().begin();
	auto& arg = static_cast<SnExpression&>(*it);
	if (!arg.IsResolved() || !arg.EvalDataType())
		return;   //its own resolution already reported
	if (arg.ContainFlags(NF_NullLiteral)
		|| !ScalarArgAdmitted(it, arg.EvalDataType(),
			wantLong ? RTK_Long : RTK_Double))
		m_Env.Log(CLL_Error, invoke.Location(),
			"stream method \"%s\" expects %s argument.",
			name.c_str(), wantLong ? "a long" : "a double");
}

//0.7.5: stream methods declare no NLang formals — the value argument
//travels via callParamBase and the intrinsic reads the slot raw — so
//nothing else gates what lands in that slot. A double literal into
//writeFloat's float slot wrote the double's low bytes silently (the
//literal tiering made `1.0` double-typed and surfaced the gap; the
//same hole accepted writeInt(1.5f) and wrong arities). Gate arity and
//value kind by name; readStruct/readObject's string-literal argument
//is gated upstream in ResolveStreamSpecialTypeArg.
void ExprResolveAccessor::CheckStreamMethodSignature(
	SnInvokeExpr &invoke, const std::string &name)
{
	if (name == "readStruct" || name == "readObject")
		return;  //string-literal argument gated upstream
	const size_t argCount = ArgCountOf(invoke);
	//Zero-argument methods: the readers and the stream-control pair.
	if (name == "readInt" || name == "readFloat" || name == "readString"
		|| name == "readLong" || name == "readDouble"
		|| name == "length" || name == "position"
		|| name == "reset" || name == "close")
	{
		if (argCount != 0)
			m_Env.Log(CLL_Error, invoke.Location(),
				"stream method \"%s\" takes no arguments.",
				name.c_str());
		return;
	}
	if (argCount != 1)
	{
		m_Env.Log(CLL_Error, invoke.Location(),
			"stream method \"%s\" expects one argument.",
			name.c_str());
		return;
	}
	//0.7.5 Task 9: the 8-byte writers admit through the cast matrix
	//with a mandatory widening wrap (see AdmitWideStreamWriterArg).
	if (name == "writeLong" || name == "writeDouble")
	{
		AdmitWideStreamWriterArg(invoke, name);
		return;
	}
	auto& arg = *invoke.Params().begin();
	if (!arg.IsResolved())
		return;   //its own resolution already reported
	const char* want = StreamArgExpectation(name, arg,
		arg.ContainFlags(NF_NullLiteral));
	if (want)
		m_Env.Log(CLL_Error, invoke.Location(),
			"stream method \"%s\" expects %s argument.",
			name.c_str(), want);
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
	CheckStreamMethodSignature(invoke, name);
	pInnerExpr->AddFlags(NF_Resolved);
	//For void-returning methods, leave EvalDataType unset.
	if (name != "writeInt" && name != "writeFloat"
		&& name != "writeLong" && name != "writeDouble"
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
