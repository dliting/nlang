/*---
    TypeArgInference.h — 调用点类型实参推断（0.13.1a Task 2）：把模板行
    静态成员的形参签名与一次调用的实参已解析类型对齐，为每个类型参数
    占位符绑定一个实参类型字段，供创建（GetDeclaredGenericClassDecl）
    直接消费。Task 5补齐推断臂：数组、嵌套实例化、返回位（从赋值目标
    发布的期望类型），并携带具名诊断所需的冲突/未绑定细节。
---*/
#pragma once
#include <vector>

namespace nlang
{

class GenericInstanceRegistry;
class SnFunction;
class SnExpression;
class SnField;

//The outcome of one candidate's unification: Bound fills bindings;
//Unbound means a type parameter got no actual from this call (the
//call site names the parameter and points at the fix); Mismatched
//means shapes conflicted (the ordinary binding path's per-argument
//notes are the precise report — unless two formals disagreed on one
//placeholder, which is the named conflict case below).
enum class TypeArgInferenceResult
{
	Bound,
	Unbound,
	Mismatched,
};

//Named-diagnostic detail for a Mismatched outcome where two formals
//bound the SAME placeholder to DIFFERENT actuals (max(3, 7.0)) —
//exact-match unification, no implicit numeric promotion. Shape
//mismatches (List<T> formal vs int actual) leave this empty: the
//ordinary per-argument notes are the report there.
struct TypeArgConflict
{
	size_t ordinal = 0;
	const SnField* pFirst = nullptr;   //the earlier formal's binding
	const SnField* pSecond = nullptr;  //the actual that refused it
};

//One candidate's full unification outcome for the sweep and its
//diagnostics. bindings is indexed by placeholder ordinal (nullptr =
//unbound; drives the unbound diagnostic); bReturnOnlyUnbound marks an
//unbound ordinal whose placeholder the return-type tree mentions
//(drives the return-position hint). Fields are the registry's own
//entries, ready for GetDeclaredGenericClassDecl.
struct TypeArgInferenceOutcome
{
	TypeArgInferenceResult result = TypeArgInferenceResult::Mismatched;
	std::vector<SnField*> bindings;
	TypeArgConflict conflict;
	bool bReturnOnlyUnbound = false;
};

//Unify one template-row member's formal signature against the resolved
//actual types of a call. Every type-parameter placeholder in the
//signature binds to the actual type field at its position — a direct
//leaf formal binds the whole actual, an array/array or instantiation/
//instantiation formal recurses position-wise. Placeholders still
//unbound after the formals unify against pExpectedType (the assign-
//published expected type; null when the call has none) through the
//return-type tree. outcome.bindings on Bound is indexed by placeholder
//ordinal and sized to the template's type-parameter count.
TypeArgInferenceOutcome InferTypeArgsForCall(
    const GenericInstanceRegistry& generics, const SnFunction& tmplMember,
    const std::vector<SnExpression*>& actuals,
    const SnField* pExpectedType = nullptr);

} //namespace nlang
