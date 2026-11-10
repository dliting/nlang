/*---
    TypeArgInference.h — 调用点类型实参推断（0.13.1a Task 2）：把模板行
    静态成员的形参签名与一次调用的实参已解析类型对齐，为每个类型参数
    占位符绑定一个实参类型字段，供铸造（GetDeclaredGenericClassDecl）
    直接消费。
---*/
#pragma once
#include <vector>

namespace nlang
{

class GenericInstanceRegistry;
class SnFunction;
class SnExpression;
class SnField;

//The outcome of one candidate's unification: Bound fills outTypeArgs;
//Unbound means a type parameter got no actual from this call (the
//zero-argument shape — the call site names the parameter and points at
//the explicit spelling); Mismatched means shapes conflicted (the
//ordinary binding path's per-argument notes are the precise report).
enum class TypeArgInferenceResult
{
	Bound,
	Unbound,
	Mismatched,
};

//Unify one template-row member's formal signature against the resolved
//actual types of a call. Every type-parameter placeholder in the
//signature binds to the actual type field at its position — a direct
//leaf formal binds the whole actual, an array/array or instantiation/
//instantiation formal recurses position-wise. outTypeArgs on Bound is
//indexed by placeholder ordinal and sized to the template's type-
//parameter count; its fields feed GetDeclaredGenericClassDecl directly.
TypeArgInferenceResult InferTypeArgsForCall(
    const GenericInstanceRegistry& generics, const SnFunction& tmplMember,
    const std::vector<SnExpression*>& actuals,
    std::vector<SnField*>& outTypeArgs);

} //namespace nlang
