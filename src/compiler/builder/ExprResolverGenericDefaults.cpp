/*--- ExprResolverGenericDefaults.cpp — 泛型实例成员签名克隆中的
    常量默认值深拷贝家族（字面量/运算符树/常量引用三臂）。
    从 ExprResolverGenericMint.cpp 抽取（2026-10-30 source-size 守卫
    驱动的拆分，零行为变化）。
---*/
#include "ExprResolver.h"
#include "SnExtraTypes.h"
#include "ScriptLocation.h"
#include "SyntaxTree.h"
#include <nlang/runtime/RnTypes.h>
#include <string>

namespace nlang
{

//Literal arm of CloneConstantDefaultExpr: rebuild through the typed
//constructors (the same shapes SynthScalarDefault uses for imported
//defaults), so reference payloads re-allocate and the copy is born
//resolved; null keeps its flag.
static SnLiteralExpr* CloneLiteralDefault(SnLiteralExpr& lit)
{
	auto* pType = lit.Value().Type();
	if (!pType)
		return nullptr;
	const auto& data = lit.Value().Data();
	const ISourceLocation& loc = *lit.Location();
	SnLiteralExpr* pCopy = nullptr;
	switch (pType->Kind())
	{
	case NK_Int32:
		pCopy = new SnLiteralExpr(*RnInt32::Instance(), data.m_Int, loc);
		break;
	case NK_Long:
		pCopy = new SnLiteralExpr(*RnLong::Instance(), data.m_Long, loc);
		break;
	case NK_ULong:
		pCopy = new SnLiteralExpr(*RnULong::Instance(), data.m_ULong, loc);
		break;
	case NK_Double:
		pCopy = new SnLiteralExpr(*RnDouble::Instance(), data.m_Double, loc);
		break;
	case NK_Float:
		pCopy = new SnLiteralExpr(*RnFloat::Instance(), data.m_Float, loc);
		break;
	case NK_Bool:
		pCopy = new SnLiteralExpr(*RnBool::Instance(), data.m_Int, loc);
		break;
	case NK_Char:
		pCopy = new SnLiteralExpr(*RnChar::Instance(),
			static_cast<uint32>(data.m_Int), loc);
		break;
	case NK_String:
		if (!data.m_String)
			return nullptr;
		pCopy = new SnLiteralExpr(*RnString::Instance(),
			new std::string(*data.m_String), loc);
		break;
	default:
		return nullptr;
	}
	if (lit.ContainFlags(NF_NullLiteral))
		pCopy->AddFlags(NF_NullLiteral);
	return pCopy;
}

//Operator-tree arm of CloneConstantDefaultExpr: a structural copy that
//ALSO keeps the source's resolved type channel. Emit reads each
//operand's own kind for the operation itself, but the default-binding
//tail casts off the tree's EvalDataType — a clone that loses it would
//stage raw int bits into a wider formal slot (double d = 1 + 2) with
//no cast, where the template row emits one.
SnExpression* ExprResolveAccessor::CloneOperatorDefault(
	SnBinaryExpr& src)
{
	auto* pLeft = CloneConstantDefaultExpr(src.Left());
	if (src.Left() && !pLeft)
		return nullptr;
	auto* pRight = CloneConstantDefaultExpr(src.Right());
	if (src.Right() && !pRight)
	{
		delete pLeft;
		return nullptr;
	}
	SnBinaryExpr* pCopy = src.Right()
		? new SnBinaryExpr(src.Op(), pLeft, pRight, *src.Location())
		: new SnBinaryExpr(src.Op(), pLeft, *src.Location());
	if (auto* pSrcType = src.EvalDataType())
		pCopy->EvalDataType(pSrcType);
	if (src.ContainFlags(NF_Resolved))
		pCopy->AddFlags(NF_Resolved);
	return pCopy;
}

//Deep copy of one constant-expression default for a member clone: the
//clone's surface needs the default at match time (arity) and at emit
//time (EmitDefaultBinding re-emits the tree), and the template's own
//node cannot be shared — a formal owns its default child, so sharing
//would free it twice. Literals rebuild through the typed constructors
//(CloneLiteralDefault); operator trees — binary and unary alike, a
//unary tree is a SnBinaryExpr with a null right operand — copy with
//their type channel (CloneOperatorDefault); constant references — an
//enum member, a formal, a field — transfer their bindings onto the
//copies (CloneConstantReferenceDefault); a this expression rebuilds
//with its type channel transferred (the name shape carries no field
//binding — the receiver read goes through the caller-side this-slot
//override — but the member ladder keys off the type channel). Any
//other shape — invoke defaults (`C.g()`) and cast defaults
//(`p as Payload`) are the known members of that set — degrades to no
//default; the call site then reports the arity mismatch, the same
//surface it had before defaults carried over.
SnExpression* ExprResolveAccessor::CloneConstantDefaultExpr(
	SnExpression* pSrc)
{
	if (!pSrc)
		return nullptr;
	if (pSrc->Kind() == NK_LiteralExpr)
		return CloneLiteralDefault(static_cast<SnLiteralExpr&>(*pSrc));
	if (pSrc->Kind() == NK_BinaryExpr)
		return CloneOperatorDefault(static_cast<SnBinaryExpr&>(*pSrc));
	if (pSrc->Kind() == NK_ThisExpr)
	{
		//The name shape carries no binding, but resolution stamps a
		//type channel on a this expression and the member ladder's
		//typed-receiver dispatch reads member.Outer()->EvalDataType()
		//— a copy without it emits the receiver and then silently
		//nothing for the field read.
		auto& src = static_cast<SnThisExpr&>(*pSrc);
		auto* pCopy = new SnThisExpr(*src.Location());
		if (auto* pSrcType = src.EvalDataType())
			pCopy->EvalDataType(pSrcType);
		if (src.ContainFlags(NF_Resolved))
			pCopy->AddFlags(NF_Resolved);
		return pCopy;
	}
	if (pSrc->Kind() == NK_IdentifierExpr || pSrc->Kind() == NK_NameExpr
			|| pSrc->Kind() == NK_MemberExpr)
		return CloneConstantReferenceDefault(
			static_cast<SnFieldExpr&>(*pSrc));
	return nullptr;
}

//Constant-REFERENCE arm of CloneConstantDefaultExpr: copy the name
//shape and TRANSFER the binding whenever the source resolved. Every
//binding a default can carry names a declaration node of the template
//row — formals, fields, enum members, statics, module constants — and
//those nodes live through codegen (the row is owned by the unit tree;
//only the clone's own fresh formals are new, and nothing binds to
//them). The transfer is what makes the clone emit exactly what the
//template row emits: a formal reference keeps its type channel for the
//member-receiver dispatch while the identifier emit stays name-keyed
//(LookupOverride fires before any binding read), and a field binding
//drives the same receiver read the row performs. Unsupported SHAPES —
//an invoke default like `C.g()` — never reach this arm and degrade to
//no default, which the call site reports as an arity mismatch.
SnExpression* ExprResolveAccessor::CloneConstantReferenceDefault(
	SnFieldExpr& src)
{
	if (src.Kind() == NK_IdentifierExpr)
	{
		auto& id = static_cast<SnIdentifierExpr&>(src);
		auto* pCopy = new SnIdentifierExpr(
			new std::string(id.Name()), *id.Location());
		if (id.IsResolved())
			ResolveFieldExprAs(*pCopy, id.Field());
		return pCopy;
	}
	if (src.Kind() == NK_NameExpr)
	{
		auto& name = static_cast<SnNameExpr&>(src);
		auto* pInner = dynamic_cast<SnIdentifierExpr*>(name.Expr());
		if (!pInner)
			return nullptr;
		auto* pCopy = new SnNameExpr(new SnIdentifierExpr(
			new std::string(pInner->Name()), *pInner->Location()),
			*pInner->Location());
		if (name.IsResolved())
			ResolveFieldExprAs(*pCopy, name.Field());
		return pCopy;
	}
	return CloneMemberReferenceDefault(static_cast<SnMemberExpr&>(src));
}

//Member arm of CloneConstantReferenceDefault (`Color.Red`, `p.x`):
//outer is the type qualifier or the value receiver, inner names the
//member; the member expression itself carries the value binding. An
//unresolved member has no binding to transfer and its emit would
//silently decline every dispatch arm — that shape degrades to no
//default.
SnMemberExpr* ExprResolveAccessor::CloneMemberReferenceDefault(
	SnMemberExpr& src)
{
	auto* pOuterCopy = src.Outer()
		? CloneConstantDefaultExpr(src.Outer()) : nullptr;
	if (src.Outer() && !pOuterCopy)
		return nullptr;
	auto* pInner = src.Inner();
	if (!pInner || pInner->Kind() != NK_IdentifierExpr)
	{
		delete pOuterCopy;
		return nullptr;
	}
	auto& inner = static_cast<SnIdentifierExpr&>(*pInner);
	auto* pInnerCopy = new SnIdentifierExpr(
		new std::string(inner.Name()), *inner.Location());
	if (inner.IsResolved())
		ResolveFieldExprAs(*pInnerCopy, inner.Field());
	auto* pCopy = new SnMemberExpr(
		pOuterCopy, pInnerCopy, *src.Location());
	if (src.IsResolved())
		ResolveFieldExprAs(*pCopy, src.Field());
	return pCopy;
}

} //namespace nlang
