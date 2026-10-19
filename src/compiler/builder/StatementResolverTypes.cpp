/*---
    StatementResolverTypes.cpp — 类型声明解析：struct/class/interface 成员遍历与 Exception 子类判定。
    从 StatementResolver.hpp 抽取（2026-09-26 后续轮次重构，零行为变化）。
---*/
#include "StatementResolver.h"
#include "ExprResolverCastFit.hpp"
#include <nlang/compiler/SnExpressions.h>
#include <nlang/runtime/RnTypes.h>

namespace nlang
{

//0.9.0 semantic restriction gate: a variable-style declaration — local,
//formal parameter, catch parameter, loop variable, or class/struct
//field — may not reuse the name of a type visible at that scope. The
//test is the exact complement of the type-position predicate
//(NamesVisibleType): a name spellable as a type here cannot name
//a variable here. This removes at the root the resolution ambiguity
//the TryRebindShadowedTypeName guard patches over; that guard stays
//for the value kinds still allowed to collide (enum members,
//functions in disjoint bare pools, namespaces).
void StatementResolveAccessor::RejectTypeNameVariable(
	const std::string &name, SyntaxNode &scope, const SnField &accessor,
	const ISourceLocation *pLoc)
{
	if (m_ExprResolver.NamesVisibleType(name, scope, accessor))
		m_Env.Log(CLL_Error, pLoc,
			"\"%s\" names a type; it cannot be used as a variable name.",
			name.c_str());
}

void StatementResolveAccessor::Access(SnStructDecl &sn)
{
	assert(m_pVisitor);
	for (auto &field : sn.Members())
	{
		//0.9.0 semantic restriction: struct field names may not reuse
		//visible type names.
		RejectTypeNameVariable(field.Name(), sn, sn, field.Location());
		field.Type()->Accept(*m_pVisitor);
	}
	sn.AddFlags(NF_Resolved);
}

void StatementResolveAccessor::Access(SnStructField &sn)
{
}

//Resolve super class reference using ExprResolver (not the
//StatementResolver visitor, which lacks Access(SnNameExpr&)), then
//resolve "implements I1, I2" names into SnInterfaceDecl* pointers.
void StatementResolveAccessor::ResolveClassBases(SnClassDecl &sn)
{
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
}

//A method that overrides a parent virtual method
//is also virtual (implicit virtual propagation).
//Check both name and parameter count to avoid false matches.
void StatementResolveAccessor::PropagateVirtualOverrides(SnClassDecl &sn)
{
	auto *pSuper = sn.SuperClass();
	if (!pSuper)
		return;
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

void StatementResolveAccessor::Access(SnClassDecl &sn)
{
	assert(m_pVisitor);
	ResolveClassBases(sn);
	PropagateVirtualOverrides(sn);
	for (auto &field : sn.Members())
	{
		if (field.Kind() == NK_ClassField)
		{
			//0.9.0 semantic restriction: class field names may not
			//reuse visible type names.
			RejectTypeNameVariable(field.Name(), sn, sn,
				field.Location());
			field.Accept(*m_pVisitor);
		}
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

void StatementResolveAccessor::Access(SnClassField &sn)
{
	assert(m_pVisitor);
	if (sn.Type())
		sn.Type()->Accept(*m_pVisitor);
}

//Phase 9d: walk the SuperClass chain of t (if any). Returns true if any
//ancestor class is named "Exception" (case-sensitive). Also returns true
//when t itself is "Exception".
bool StatementResolveAccessor::IsExceptionSubclass(SnField *t)
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

namespace
{

//The constant-initializer fold surface (0.9.10 D3.3) mirrors
//VmBackend::ExtractDefaultValue exactly: a string/null/bool/char
//literal, or a numeric literal and its unary negation. The mirror keeps
//declaration-time acceptance and emission-time folding in lockstep.
enum class ConstantFoldKind
{
	NotFoldable,
	Null,
	Bool,
	Char,
	Int32,
	Long,
	ULong,
	Float,
	Double,
	String,
};

ConstantFoldKind ClassifyConstantInitializer(SnExpression *pExpr)
{
	if (!pExpr)
		return ConstantFoldKind::NotFoldable;
	if (pExpr->Kind() == NK_LiteralExpr)
	{
		auto &lit = static_cast<SnLiteralExpr &>(*pExpr);
		if (lit.ContainFlags(NF_NullLiteral))
			return ConstantFoldKind::Null;
		if (lit.Value().Type() == RnString::Instance())
			return ConstantFoldKind::String;
		//Bool/char carriers are int32/uint32; TryGetConstantLiteral
		//covers only the numeric rows, so classify them here.
		if (lit.Value().Type()->Kind() == NK_Bool)
			return ConstantFoldKind::Bool;
		if (lit.Value().Type()->Kind() == NK_Char)
			return ConstantFoldKind::Char;
	}
	ConstLiteralValue value;
	if (!TryGetConstantLiteral(pExpr, value))
		return ConstantFoldKind::NotFoldable;
	switch (value.kind)
	{
		case NK_Int32:  return ConstantFoldKind::Int32;
		case NK_Long:   return ConstantFoldKind::Long;
		case NK_ULong:  return ConstantFoldKind::ULong;
		case NK_Float:  return ConstantFoldKind::Float;
		case NK_Double: return ConstantFoldKind::Double;
		default:        return ConstantFoldKind::NotFoldable;
	}
}

NodeKind FoldKindNodeKind(ConstantFoldKind kind)
{
	switch (kind)
	{
		case ConstantFoldKind::Bool:   return NK_Bool;
		case ConstantFoldKind::Char:   return NK_Char;
		case ConstantFoldKind::Int32:  return NK_Int32;
		case ConstantFoldKind::Long:   return NK_Long;
		case ConstantFoldKind::ULong:  return NK_ULong;
		case ConstantFoldKind::Float:  return NK_Float;
		case ConstantFoldKind::Double: return NK_Double;
		case ConstantFoldKind::String: return NK_String;
		default:                       return NK_Void;
	}
}

//0.9.10 D3.3: the declared type must be a compile-time scalar or string;
//object and array constants need shared runtime storage, which this
//version deliberately excludes.
bool IsConstantValueType(SnField *pType)
{
	if (!pType || pType->IsArrayType())
		return false;
	return pType->Kind() == NK_String
		|| ScalarPrimIndexOf(pType->Kind()) >= 0;
}

//String/number domain split of one constant: the declared string slot
//takes exactly a string literal, a scalar slot exactly a numeric one.
//True = rejected with its diagnostic.
bool RejectConstantDomainMismatch(BuildEnvironment &env,
	SnClassField &field, SnField &declared, SnField &source,
	ConstantFoldKind foldKind)
{
	if ((declared.Kind() == NK_String)
		== (foldKind == ConstantFoldKind::String))
		return false;
	env.Log(CLL_Error, field.Initializer()->Location(),
		"the initializer of constant \"%s\" has incompatible type "
		"\"%s\"; expected \"%s\".",
		field.Name().c_str(), source.ToString().c_str(),
		declared.ToString().c_str());
	return true;
}

//Assignment compatibility of a numeric constant: the normal cast table
//plus the constant-fit exemption for explicit narrowings, so e.g.
//`static const byte B = 5;` passes while `static const int A = 0.5;`
//keeps the explicit-conversion diagnostic. True = rejected.
bool RejectConstantNumericMismatch(BuildEnvironment &env,
	SnClassField &field, SnField &declared, SnField &source)
{
	auto castInfo = GetCastInfo(&source, &declared);
	if (castInfo.Kind() == TCK_Same || castInfo.Kind() == TCK_Auto)
		return false;
	ConstantFitOutcome fit;
	const int ri = ScalarPrimIndexOf(declared.Kind());
	if (ri >= 0 && castInfo.Kind() == TCK_Explicit)
		fit = TryConstantFit(*field.Initializer(), kScalarPrims[ri]);
	if (fit.verdict == CF_Fits)
		return false;
	if (fit.verdict == CF_OutOfRange)
		env.Log(CLL_Error, field.Initializer()->Location(),
			"the initializer of constant \"%s\": constant %s out of "
			"range for '%s'",
			field.Name().c_str(), fit.constantText,
			kScalarPrims[ri].name);
	else
		env.Log(CLL_Error, field.Initializer()->Location(),
			"the initializer of constant \"%s\" has incompatible type "
			"\"%s\"; expected \"%s\".",
			field.Name().c_str(), source.ToString().c_str(),
			declared.ToString().c_str());
	return true;
}

//0.9.10 D3.3: foldability and assignment compatibility of one constant
//initializer, in declaration order: declared-type gate, fold gate, then
//the value's domain and numeric fit.
void CheckInterfaceConstantInitializer(BuildEnvironment &env,
	SnClassField &field)
{
	SnField *pDeclared = field.EvalDataType();
	if (!pDeclared)
		return;	//the type expression already failed to resolve
	if (!IsConstantValueType(pDeclared))
	{
		env.Log(CLL_Error, field.Location(),
			"The interface constant \"%s\" must have a compile-time "
			"scalar or string type.",
			field.Name().c_str());
		return;
	}
	const ConstantFoldKind foldKind
		= ClassifyConstantInitializer(field.Initializer());
	if (foldKind == ConstantFoldKind::NotFoldable)
	{
		env.Log(CLL_Error,
			field.Initializer() ? field.Initializer()->Location()
				: field.Location(),
			"the initializer of constant \"%s\" must be a constant "
			"expression.",
			field.Name().c_str());
		return;
	}
	//null binds every scalar/string slot as its zero sentinel, the same
	//bridge ordinary declarations grant.
	if (foldKind == ConstantFoldKind::Null)
		return;
	auto *pSourceType = SnBuiltinDataType::InstanceOf(
		FoldKindNodeKind(foldKind));
	if (RejectConstantDomainMismatch(env, field, *pDeclared,
		*pSourceType, foldKind))
		return;
	if (pDeclared->Kind() == NK_String)
		return;
	RejectConstantNumericMismatch(env, field, *pDeclared, *pSourceType);
}

//0.9.10 D3.7: a constant and a method of the same name in one interface
//make the value form and the call form ambiguous. The generic duplicate
//checker also reports the name collision; this diagnostic names the
//constant/method ambiguity itself.
void CheckInterfaceConstantNameClash(BuildEnvironment &env,
	SnInterfaceDecl &interfaceDecl, SnField &constant)
{
	for (auto &member : interfaceDecl.Members())
	{
		if (&member == &constant || member.Kind() != NK_Function
			|| member.Name() != constant.Name())
			continue;
		env.Log(CLL_Error, constant.Location(),
			"The interface constant \"%s\" is ambiguous with the method "
			"of the same name in interface \"%s\".",
			constant.Name().c_str(), interfaceDecl.Name().c_str());
		return;
	}
}

//0.9.10 D3.3: interface member variables exist in exactly one form,
//`static const Type NAME = <folded literal>;`.
void CheckInterfaceConstantContract(BuildEnvironment &env,
	SnClassField &field)
{
	//NodeFlagsNonEmpty only yields these four flags; the exact legal set
	//is static+const with neither virtual nor native. The static/const
	//requirement is one diagnostic; the method-only modifiers are named
	//individually, mirroring the method contract below.
	if (!field.ContainFlags(NF_Static) || !field.ContainFlags(NF_Const))
	{
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" must be static const: "
			"interfaces declare constants only.",
			field.Name().c_str());
		return;
	}
	if (field.ContainFlags(NF_Native))
		env.Log(CLL_Error, field.Location(),
			"The interface constant \"%s\" cannot be native: constants "
			"are data, not implementation.",
			field.Name().c_str());
	if (field.ContainFlags(NF_Virtual))
		env.Log(CLL_Error, field.Location(),
			"The interface constant \"%s\" cannot be virtual: constants "
			"are values, not methods.",
			field.Name().c_str());
	CheckInterfaceConstantInitializer(env, field);
}

} //namespace

//Declaration contract of a single interface member (0.9.10): public by
//default (FA_Default already resolved to FA_Public by OnAddedToParent,
//so a non-public member carries an explicit private/protected modifier).
//Methods carry no const/native/virtual flag, and of the static/instance
//pair only the legal shapes pass — a static member carries a body, an
//instance member does not (D1). Fields exist only as `static const`
//constants (D3). Rejecting at the declaration replaces the old failure
//mode where a silently-private member surfaced only at call sites as a
//misleading "does not exist or is not accessible" error.
static void CheckInterfaceMemberContract(BuildEnvironment &env,
	SnField &field)
{
	if (field.AccessType() != FA_Public)
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" must be public.",
			field.Name().c_str());
	if (field.Kind() == NK_ClassField)
	{
		CheckInterfaceConstantContract(env,
			static_cast<SnClassField &>(field));
		return;
	}
	if (field.ContainFlags(NF_Const))
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" cannot be const: interfaces"
			" declare no data to freeze.",
			field.Name().c_str());
	//0.9.10 (D1): a static member is legal as an in-place shared
	//implementation; only the bodyless static and the bodied
	//instance forms break the contract now.
	auto *fn = field.Kind() == NK_Function
		? static_cast<SnFunction *>(&field) : nullptr;
	if (fn && fn->ContainFlags(NF_Static) && !fn->Body())
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" is static but has no body: "
			"static interface methods are implemented in place.",
			field.Name().c_str());
	if (fn && !fn->ContainFlags(NF_Static) && fn->Body())
		env.Log(CLL_Error, field.Location(),
			"The interface instance method \"%s\" cannot have a body: "
			"use a static method for shared implementation.",
			field.Name().c_str());
	if (field.ContainFlags(NF_Native))
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" cannot be native: interfaces"
			" declare signatures without implementation.",
			field.Name().c_str());
	if (field.ContainFlags(NF_Virtual))
		env.Log(CLL_Error, field.Location(),
			"The interface member \"%s\" cannot be virtual: interface"
			" methods dispatch by name already.",
			field.Name().c_str());
}

void StatementResolveAccessor::Access(SnInterfaceDecl &sn)
{
	//0.9.10 D5: an imported stub carries wire-rebuilt static members
	//that are bodyless by construction — the member contract is
	//enforced at the producer's declaration. Only the signatures
	//resolve here (call sites bind against them).
	if (!sn.ContainFlags(NF_Imported))
	{
		//Enforce the member contract at the single point every own
		//interface declaration passes through.
		for (auto &field : sn.Members())
		{
			CheckInterfaceMemberContract(m_Env, field);
			if (IsInterfaceConstantField(field))
				CheckInterfaceConstantNameClash(m_Env, sn, field);
		}
	}
	//Interface member signatures resolve like class methods; only the
	//static members carry bodies (0.9.10), walked like any function
	//body. No super-class chain to walk.
	for (auto &field : sn.Members())
	{
		if (field.Kind() == NK_Function)
			field.Accept(*m_pVisitor);
	}
	sn.AddFlags(NF_Resolved);
}

} //namespace nlang
