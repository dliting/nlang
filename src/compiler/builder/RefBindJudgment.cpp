/*---
    RefBindJudgment.cpp — v0.13.9 phase 3 Task 5: the &-binding judgment
    as free functions (see RefBindJudgment.h). Bodies moved verbatim from
    StatementResolverRef.cpp; the member versions there are delegates.
    The call-site consumption (ExprResolverInvoke) and the declaration
    consumption (StatementResolverDecls) now share one judgment.
---*/
#include "RefBindJudgment.h"
#include "ExprResolver.h"
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/compiler/SnStatements.h>
#include <nlang/runtime/BuiltinGenericNames.h>

namespace nlang
{

//The storage gates shared by the identifier and member arms of the
//&-binding judgment AND by the &-return judgment — the out-argument
//three-check template (const/static) widened with the constant-position
//family. Locals and formals carry NF_Ref on their DESCRIPTOR
//(propagated from the type node); the binding-to-a-reference gate is a
//BINDING-only rule (RejectRefTargetIsReference) because returning a
//reference formal is the canonical legal shape.
bool RejectRefTargetStorage(BuildEnvironment &env, SnField *pField,
	const ISourceLocation *pLoc, bool constBinding)
{
	if (!pField)
		return false;	//resolution already reported the shape
	if (pField->Kind() == NK_EnumMember)
	{
		env.Log(CLL_Error, pLoc,
			"cannot bind a reference to enum member \"%s\".",
			pField->Name().c_str());
		return true;
	}
	//Task 9: a const T& binding is the read-only form — const storage
	//(and interface constants) alias legally; only the writable & form
	//keeps the write-through argument.
	if ((pField->ContainFlags(NF_Const)
			|| IsInterfaceConstantField(*pField))
		&& !constBinding)
	{
		env.Log(CLL_Error, pLoc,
			"cannot bind a reference to const storage \"%s\"; a "
			"reference may write through its target.",
			pField->Name().c_str());
		return true;
	}
	if (pField->ContainFlags(NF_Static))
	{
		env.Log(CLL_Error, pLoc,
			"cannot bind a reference to static storage \"%s\"; static "
			"storage promotion is not supported yet.",
			pField->Name().c_str());
		return true;
	}
	return false;
}

//The binding-only gate — a binding aliases STORAGE, not a binding slot,
//so the target must not itself be a reference (locals and formals carry
//NF_Ref on their descriptor). A reference formal MAY be returned (the
//canonical &-param plumbing), which is why this gate is not part of the
//shared storage check.
bool RejectRefTargetIsReference(BuildEnvironment &env, SnField *pField,
	const ISourceLocation *pLoc)
{
	if (!pField)
		return false;
	auto *pTargetType = pField->EvalDataType();
	if (pField->ContainFlags(NF_Ref)
		|| (pTargetType && pTargetType->ContainFlags(NF_Ref)))
	{
		env.Log(CLL_Error, pLoc,
			"cannot bind a reference to reference \"%s\"; a binding "
			"aliases storage, not a binding slot.",
			pField->Name().c_str());
		return true;
	}
	return false;
}

//True when a member-call target names storage a reference may bind.
//A &-returning callee passes the raw (record, cell) pair through the
//call boundary, so the binding aliases the callee's target. The
//container get spelling is identified by the receiver shape: the callee
//resolves to the containers.n declaration whose instantiation does not
//carry the & mark (generic synthesis, phase 3 Task 8), so the mark is
//no discriminator there. Every other method result is a temporary
//record — spec 3.9.
bool MemberCallBindsStorage(SnMemberExpr &member)
{
	auto *pInner = member.Inner();
	if (!pInner || pInner->Kind() != NK_InvokeExpr)
		return false;
	auto &invoke = static_cast<SnInvokeExpr &>(*pInner);
	auto *pOuterType = member.Outer() && member.Outer()->IsResolved()
		? member.Outer()->EvalDataType() : nullptr;
	if (invoke.CalleeName() == kBuiltinGetMethodName
		&& pOuterType && pOuterType->Kind() == NK_ClassDecl)
	{
		auto *pClass = static_cast<SnClassDecl *>(pOuterType);
		if (pClass->IsGenericInstantiation())
		{
			const auto &baseName = pClass->BaseName();
			const auto &typeArgs = pClass->GenericTypeArgs();
			if ((baseName == kBuiltinListTypeName && !typeArgs.empty())
				|| (baseName == kBuiltinDictTypeName
					&& typeArgs.size() > 1))
				return true;
		}
	}
	if (auto *pCallee = invoke.Callee())
		return pCallee->ReturnType()
			&& pCallee->ReturnType()->ContainFlags(NF_Ref);
	//Task 8: a member delegate slot whose func key marks the return & —
	//the dispatch parks the raw pair, so the binding aliases the
	//callee's target (same contract as a &-returning method). No
	//resolution gate: Field() is bound only by the completed delegate
	//bind, and slot descriptors never carry NF_Resolved.
	auto *pSlot = invoke.Field();
	if (pSlot && pSlot->Kind() != NK_Function
		&& IsFuncTypeDecl(pSlot->EvalDataType()))
	{
		auto *pFuncType
			= static_cast<SnClassDecl *>(pSlot->EvalDataType());
		const auto &refFlags = pFuncType->GenericRefFlags();
		return !refFlags.empty() && refFlags[0] != 0;
	}
	return false;
}

//Task 9: the root storage a member-chain receiver writes into, or
//null when the chain leaves a local's own slots. Struct-typed hops
//descend through slot-copy storage (every hop stores into the root's
//record); a class/array-typed field hop stops the walk — deeper
//stores mutate the shared object, not the root's slots (const is
//shallow for identity types). This/call/subscript receivers are never
//const frame storage.
SnField *FindMemberChainRootStorage(SnExpression *pReceiver)
{
	while (pReceiver)
	{
		if (pReceiver->Kind() == NK_MemberExpr)
		{
			auto &hop = static_cast<SnMemberExpr &>(*pReceiver);
			auto *pHopField = hop.Field();
			auto *pHopType
				= pHopField ? pHopField->EvalDataType() : nullptr;
			if (!pHopType || pHopType->Kind() != NK_StructDecl)
				return nullptr;	//identity hop or unresolved: pointee store
			pReceiver = hop.Outer();
			continue;
		}
		if (pReceiver->Kind() == NK_IdentifierExpr)
			return static_cast<SnIdentifierExpr &>(*pReceiver).Field();
		return nullptr;
	}
	return nullptr;
}

//Task 9: a const struct local/formal root — every store through the
//chain writes its record, so both the member-store gate and the
//writable &-binding judgment reject against it.
bool IsConstStructStorage(SnField *pRoot)
{
	if (!pRoot || !pRoot->ContainFlags(NF_Const)
		|| pRoot->Kind() == NK_ClassField
		|| pRoot->Kind() == NK_EnumMember)
		return false;
	auto *pRootType = pRoot->EvalDataType();
	return pRootType && pRootType->Kind() == NK_StructDecl;
}

//Task 9: the RECEIVER's constness bounds a member binding too — a
//writable & through a member of const storage would write through the
//const record (the const& form binds it read-only). True = rejected.
static bool RejectConstReceiverBinding(BuildEnvironment &env,
	SnMemberExpr &member, const ISourceLocation *pLoc)
{
	auto *pChainRoot = FindMemberChainRootStorage(member.Outer());
	if (!IsConstStructStorage(pChainRoot))
		return false;
	env.Log(CLL_Error, pLoc,
		"cannot bind a reference to const storage \"%s\"; a "
		"reference may write through its target.",
		pChainRoot->Name().c_str());
	return true;
}

//The position family of the &-binding judgment — the out-argument
//position template widened to the bindable-position list. Member/
//identifier targets run the shared storage gates; subscript binds
//elements and rejects slices. Task 6: a member-call target binds when
//the callee hands back a reference (MemberCallBindsStorage).
bool RejectRefBindPosition(BuildEnvironment &env, SnExpression &expr,
	const ISourceLocation *pLoc, bool constBinding)
{
	if (expr.Kind() == NK_MemberExpr)
	{
		auto &member = static_cast<SnMemberExpr &>(expr);
		if (member.Inner() && member.Inner()->Kind() == NK_InvokeExpr
			&& !MemberCallBindsStorage(member))
		{
			env.Log(CLL_Error, pLoc,
				"cannot bind a reference to a method call result.");
			return true;
		}
		if (!constBinding && RejectConstReceiverBinding(env, member, pLoc))
			return true;
		if (RejectRefTargetStorage(env,
				member.Inner() ? member.Inner()->Field() : nullptr,
				pLoc, constBinding))
			return true;
	}
	else if (expr.Kind() == NK_SubscriptExpr)
	{
		//Element positions bind; a slice is a view over a range, not
		//one element's storage.
		for (auto *pIndex
				: static_cast<SnSubscriptExpr &>(expr).Indices())
		{
			if (pIndex->Kind() == NK_SliceItemExpr)
			{
				env.Log(CLL_Error, pLoc,
					"a reference cannot bind a slice; slices are "
					"views, not element storage.");
				return true;
			}
		}
	}
	else if (expr.Kind() == NK_IdentifierExpr)
	{
		auto *pTargetField
			= static_cast<SnIdentifierExpr &>(expr).Field();
		if (RejectRefTargetStorage(env, pTargetField, pLoc, constBinding))
			return true;
		//Only a named binding target can itself be a reference (fields
		//cannot carry &: the grammar rejects the declaration spelling).
		if (RejectRefTargetIsReference(env, pTargetField, pLoc))
			return true;
	}
	return false;
}

//Promotion marking for a &-binding whose target resolved to a plain
//identifier. Only scalar/enum/string locals and value formals promote —
//their storage compiles into one stable boxed record (design §3.2, the
//closure-cell family). Struct targets keep their heap-record slot
//(OP_RefOfLocal binds them directly) and fields/elements bind through
//their owner records. Runs AFTER the binding judgment, so const/static/
//reference targets never reach here.
void MarkPromotedBindTarget(SnExpression &expr)
{
	if (expr.Kind() != NK_IdentifierExpr)
		return;
	auto *pField = static_cast<SnIdentifierExpr &>(expr).Field();
	//Only frame-slot storage promotes: the lightweight local descriptor
	//and the real formal param (both NK_FormalParam — SnLocalVar reuses
	//the kind). Struct/class fields, statics and enum members bind
	//through other representations or were already rejected.
	if (!pField || pField->Kind() != NK_FormalParam)
		return;
	auto *pType = pField->EvalDataType();
	if (!pType)
		return;
	const NodeKind kind = pType->Kind();
	//Scalars and string ride the primitive rows; enums ride their
	//int32 carrier. Struct/class/array-token kinds keep their own
	//storage shape.
	if (!IsPrimitiveType(kind) && kind != NK_EnumDecl)
		return;
	pField->AddFlags(NF_Promoted);
	//A promoted foreach loop variable must also flag its statement:
	//local declarators and formals read the flag off the descriptor,
	//but the loop var's slot is allocated from the statement node
	//(AllocForeachLocals reads AST-level flags — the same channel the
	//& form's VarType flag rides). Walk to the nearest ancestor foreach
	//whose loop variable IS this local; a shadowing nested loop never
	//appears on this chain, so only the owning statement is flagged.
	for (SyntaxNode *pAnc = expr.Parent(); pAnc; pAnc = pAnc->Parent()) {
		if (pAnc->Kind() != NK_ForeachStmt)
			continue;
		auto &fe = static_cast<SnForeachStmt&>(*pAnc);
		if (fe.VarName() == pField->Name()) {
			fe.AddFlags(NF_Promoted);
			break;
		}
	}
}

//The call-site consumption (ExprResolverInvoke.cpp): every argument
//bound to a & formal runs the same judgment as a &-binding initializer
//(spec §3.9: one MakeRef contract — position family, exact type,
//accepted rvalues). Accepted identifier arguments get the promotion
//mark so the caller's scalar local compiles to the stable box record
//the pair names. Judged on the RAW argument types, before the
//approximate cast fixup, so a mismatching argument names its true
//source type. B_Default never lands here on a & formal: the
//declaration gate already rejects defaults on references.
bool RejectRefBindCallArgs(BuildEnvironment &env,
	std::vector<FormalBinding> &bindings)
{
	for (auto &b : bindings)
	{
		if (!b.pFormal || !b.pCallerExpr
			|| !b.pFormal->ContainFlags(NF_Ref))
			continue;
		//Task 9: a const T& formal binds read-only — its arguments run
		//the judgment in constBinding mode (const storage aliases).
		const bool constBinding = b.pFormal->ContainFlags(NF_Const);
		if (RejectRefBindTarget(env, *b.pCallerExpr,
				b.pFormal->EvalDataType(),
				b.pCallerExpr->Location(), constBinding))
			return false;
		MarkPromotedBindTarget(*b.pCallerExpr);
	}
	return true;
}

//The &-binding judgment for one resolved initializer or argument.
bool RejectRefBindTarget(BuildEnvironment &env, SnExpression &expr,
	SnField *pTypeField, const ISourceLocation *pLoc, bool constBinding)
{
	//Identity types: & is a no-op mark — the statement degrades to a
	//plain value declaration with no binding contract (spec 3.1).
	if (IsRefNoOpType(pTypeField))
		return false;
	if (RejectRefBindPosition(env, expr, pLoc, constBinding))
		return true;
	//Everything else (constructor/free-function invoke results,
	//literals, binary results, explicit `as`) is an accepted rvalue:
	//the binding aliases a fresh temporary and writes to it are lost
	//(spec 3.9 rvalue-binding contract, documented).
	//A compiler-inserted SnCastExpr is different: it wraps the resolved
	//initializer AFTER resolution (the assign-side cast fixup), so its
	//EvalDataType already reports the conversion TARGET and the exact
	//compare below would pass. A non-Same cast means a conversion
	//happened — conversions build new storage and break alias pointing,
	//so the reject names the cast's SOURCE type (the byte in
	//`int& r = b;`). Explicit `as` keeps the rvalue contract.
	if (expr.Kind() == NK_CastExpr
		&& static_cast<SnCastExpr &>(expr).CastKind() != TCK_Same)
	{
		auto &cast = static_cast<SnCastExpr &>(expr);
		auto *pSourceType = cast.Source() && cast.Source()->IsResolved()
			? cast.Source()->EvalDataType() : nullptr;
		if (pSourceType && pTypeField)
		{
			env.Log(CLL_Error, pLoc,
				"cannot bind %s to %s&; a reference binding requires "
				"the exact type.",
				pSourceType->Name().c_str(),
				pTypeField->Name().c_str());
			return true;
		}
	}
	//Exact-type rule for slot-copy bindings: a conversion would build
	//new storage and break alias pointing, so even widening is a
	//reject — enums and their underlying integers included.
	auto *pInitType = expr.EvalDataType();
	if (pInitType && pTypeField && pInitType != pTypeField)
	{
		env.Log(CLL_Error, pLoc,
			"cannot bind %s to %s&; a reference binding requires "
			"the exact type.",
			pInitType->Name().c_str(), pTypeField->Name().c_str());
		return true;
	}
	return false;
}

//D9 fork warning (v0.13.9 phase 3): a template member writing through a
//T& formal is visible to the caller only when T binds a by-slot-copy
//type; when T binds an identity type (class/interface/array) the write
//reaches the local formal alone. The instantiation point warns per
//T& formal so the silent fork is named at the spelling that chose it.
//Warning-level — the program stays legal (spec 3.1 declares T&≡T for
//identity types).
void WarnIdentityRefFormals(BuildEnvironment &env, const SnClassDecl &tmpl,
	const std::vector<SnField*> &typeArgs, const ISourceLocation *pLoc)
{
	auto &generics = env.Registry().DeclaredGenerics();
	for (const SnField &member : tmpl.Members())
	{
		if (member.Kind() != NK_Function)
			continue;
		const auto &fn = static_cast<const SnFunction &>(member);
		for (auto &param : fn.Params())
		{
			//Task 9: a const T& formal is read-only — no write can fork,
			//so the identity binding carries nothing to warn about.
			if (param.ContainFlags(NF_Const))
				continue;
			auto *pType = param.Type();
			if (!pType || !pType->IsResolved()
				|| !pType->ContainFlags(NF_Ref))
				continue;
			auto *pField = pType->Field();
			if (!pField || !generics.IsPlaceholder(pField))
				continue;
			const size_t idx = generics.PlaceholderIndex(pField);
			if (idx >= typeArgs.size() || !typeArgs[idx])
				continue;
			const NodeKind kind = typeArgs[idx]->Kind();
			if (kind == NK_ClassDecl || kind == NK_InterfaceDecl
				|| kind == NK_ArrayTypeToken)
			{
				env.Log(CLL_Warn, pLoc,
					"the T& formal of \"%s\" binds an identity type; "
					"writes through it reach only the local formal.",
					fn.Name().c_str());
			}
		}
	}
}

} //namespace nlang
