/*-----------------------------------------------------------------------------
	builder/FieldInitializerChecks.hpp
	编译期默认值检查的共享常量折叠面：接口常量（0.9.10 D3.3）与字段
	初始化器（0.9.11 D4）共用的可折叠字面量分类、声明类型门与值域/
	数值适配拒绝。诊断主语参数化（"constant"/"field"），文案随声明
	形态。从 StatementResolverTypes.cpp 抽取（0.9.11 T3.1，零行为变化）。
-----------------------------------------------------------------------------*/
#pragma once
#include "ExprResolverCastFit.hpp"
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/CastInfo.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/runtime/RnTypes.h>

namespace nlang
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

inline ConstantFoldKind ClassifyConstantInitializer(SnExpression *pExpr)
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

inline NodeKind FoldKindNodeKind(ConstantFoldKind kind)
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
inline bool IsConstantValueType(SnField *pType)
{
	if (!pType || pType->IsArrayType())
		return false;
	return pType->Kind() == NK_String
		|| ScalarPrimIndexOf(pType->Kind()) >= 0;
}

//Fold gate shared by both declaration forms: the initializer must be a
//foldable literal (foldKind from ClassifyConstantInitializer, computed
//once by the caller). pInit may be null (the declaration location is
//used then). subject names the declaration form in the text.
//True = rejected.
inline bool RejectConstantFoldability(BuildEnvironment &env,
	SnField &field, SnExpression *pInit, ConstantFoldKind foldKind,
	const char *subject)
{
	if (foldKind != ConstantFoldKind::NotFoldable)
		return false;
	env.Log(CLL_Error, pInit ? pInit->Location() : field.Location(),
		"the initializer of %s \"%s\" must be a constant expression.",
		subject, field.Name().c_str());
	return true;
}

//String/number domain split of one constant: the declared string slot
//takes exactly a string literal, a scalar slot exactly a numeric one.
//True = rejected with its diagnostic.
inline bool RejectConstantDomainMismatch(BuildEnvironment &env,
	SnField &field, SnExpression *pInit, SnField &declared, SnField &source,
	ConstantFoldKind foldKind, const char *subject)
{
	if ((declared.Kind() == NK_String)
		== (foldKind == ConstantFoldKind::String))
		return false;
	env.Log(CLL_Error, pInit->Location(),
		"the initializer of %s \"%s\" has incompatible type "
		"\"%s\"; expected \"%s\".",
		subject, field.Name().c_str(), source.ToString().c_str(),
		declared.ToString().c_str());
	return true;
}

//Assignment compatibility of a numeric constant: the normal cast table
//plus the constant-fit exemption for explicit narrowings, so e.g.
//`static const byte B = 5;` passes while `static const int A = 0.5;`
//keeps the explicit-conversion diagnostic. True = rejected.
inline bool RejectConstantNumericMismatch(BuildEnvironment &env,
	SnField &field, SnExpression *pInit, SnField &declared, SnField &source,
	const char *subject)
{
	auto castInfo = GetCastInfo(&source, &declared);
	if (castInfo.Kind() == TCK_Same || castInfo.Kind() == TCK_Auto)
		return false;
	ConstantFitOutcome fit;
	const int ri = ScalarPrimIndexOf(declared.Kind());
	if (ri >= 0 && castInfo.Kind() == TCK_Explicit)
		fit = TryConstantFit(*pInit, kScalarPrims[ri]);
	if (fit.verdict == CF_Fits)
		return false;
	if (fit.verdict == CF_OutOfRange)
		env.Log(CLL_Error, pInit->Location(),
			"the initializer of %s \"%s\": constant %s out of "
			"range for '%s'",
			subject, field.Name().c_str(), fit.constantText,
			kScalarPrims[ri].name);
	else
		env.Log(CLL_Error, pInit->Location(),
			"the initializer of %s \"%s\" has incompatible type "
			"\"%s\"; expected \"%s\".",
			subject, field.Name().c_str(), source.ToString().c_str(),
			declared.ToString().c_str());
	return true;
}

//Fold/domain/fit surface shared by both declaration forms once the
//declared-type gate has passed: foldability, then the value's domain
//and numeric fit against the declared slot.
inline void CheckConstantValueSurface(BuildEnvironment &env,
	SnField &field, SnExpression *pInit, SnField &declared,
	const char *subject)
{
	const ConstantFoldKind foldKind
		= ClassifyConstantInitializer(pInit);
	if (RejectConstantFoldability(env, field, pInit, foldKind, subject))
		return;
	//null binds every scalar/string slot as its zero sentinel, the same
	//bridge ordinary declarations grant.
	if (foldKind == ConstantFoldKind::Null)
		return;
	auto *pSourceType = SnBuiltinDataType::InstanceOf(
		FoldKindNodeKind(foldKind));
	if (RejectConstantDomainMismatch(env, field, pInit, declared,
		*pSourceType, foldKind, subject))
		return;
	if (declared.Kind() == NK_String)
		return;
	RejectConstantNumericMismatch(env, field, pInit, declared,
		*pSourceType, subject);
}

//0.9.11 D4: a class field initializer is a compile-time default. Only
//scalar and string slots carry one (the gate names the field form);
//the fold/domain/fit surface is shared with interface constants. The
//injection that applies the default runs later in the pipeline.
inline void CheckFieldInitializer(BuildEnvironment &env,
	SnClassField &field)
{
	SnExpression *pInit = field.Initializer();
	if (!pInit)
		return;
	SnField *pDeclared = field.EvalDataType();
	if (!pDeclared)
		return;	//the type expression already failed to resolve
	if (!IsConstantValueType(pDeclared))
	{
		env.Log(CLL_Error, field.Location(),
			"field \"%s\" cannot have an initializer: only scalar "
			"and string fields support compile-time defaults",
			field.Name().c_str());
		return;
	}
	CheckConstantValueSurface(env, field, pInit, *pDeclared, "field");
}

} //namespace nlang
