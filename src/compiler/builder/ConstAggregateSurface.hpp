/*---
    ConstAggregateSurface.hpp — v0.12.4 D6：const聚合字段类型面检查（拆自StatementResolverTypes.cpp，source-size split，零行为变化）。
    合法面＝基元、string、枚举、func类型、类型参数占位、const聚合、元素合规的容器。
    数组无浅拷贝通道且下标元素可写，整体拒绝（List字段＋copy-on-read才是密封形态）；
    可变class/struct字段类型经共享句柄穿密封（`c.f.x = 1`），可变容器元素同理（`c.xs.get(0).x = 1`）；
    接口引用可由任意可变实现类支撑，与可变class字段同罪（全分支审查修复）。
---*/
#pragma once

#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/runtime/BuiltinGenericNames.h>

namespace nlang
{

//The element-type half: List/Dict type arguments must be immutable-
//faced, the same rule the field itself obeys.
inline void CheckConstContainerElements(BuildEnvironment &env,
	SnField &owner, const char *szKindWord, SnField &field,
	SnClassDecl &inst)
{
	for (SnField *pArg : inst.GenericTypeArgs())
	{
		if (pArg->IsArrayType())
		{
			env.Log(CLL_Error, field.Location(),
				"The field \"%s\" of const %s \"%s\" must hold "
				"immutable container elements (\"%s\" is a mutable "
				"array).",
				field.Name().c_str(), szKindWord,
				owner.Name().c_str(), pArg->ToString().c_str());
			return;
		}
		if (pArg->Kind() == NK_ClassDecl
			&& !static_cast<SnClassDecl *>(pArg)->IsConstAggregate())
		{
			env.Log(CLL_Error, field.Location(),
				"The field \"%s\" of const %s \"%s\" must hold "
				"immutable container elements (\"%s\" is mutable).",
				field.Name().c_str(), szKindWord,
				owner.Name().c_str(), pArg->Name().c_str());
			return;
		}
		if (pArg->Kind() == NK_StructDecl
			&& !static_cast<SnStructDecl *>(pArg)->IsConstAggregate())
		{
			env.Log(CLL_Error, field.Location(),
				"The field \"%s\" of const %s \"%s\" must hold "
				"immutable container elements (\"%s\" is mutable).",
				field.Name().c_str(), szKindWord,
				owner.Name().c_str(), pArg->Name().c_str());
			return;
		}
		//An interface reference can be backed by any mutable
		//implementor — the same shared-handle leak as above.
		if (pArg->Kind() == NK_InterfaceDecl)
		{
			env.Log(CLL_Error, field.Location(),
				"The field \"%s\" of const %s \"%s\" must hold "
				"immutable container elements (\"%s\" is an "
				"interface).",
				field.Name().c_str(), szKindWord,
				owner.Name().c_str(), pArg->Name().c_str());
			return;
		}
	}
}

//The direct-type half: everything except containers, resolved type in
//hand. Primitives, string and enums need no check (nothing to seal).
inline void CheckConstDirectFieldType(BuildEnvironment &env,
	SnField &owner, const char *szKindWord, SnField &field, SnField *pType)
{
	if (pType->Kind() == NK_StructDecl)
	{
		if (!static_cast<SnStructDecl *>(pType)->IsConstAggregate())
			env.Log(CLL_Error, field.Location(),
				"The field \"%s\" of const %s \"%s\" must be a const "
				"struct type (\"%s\" is mutable).",
				field.Name().c_str(), szKindWord, owner.Name().c_str(),
				pType->Name().c_str());
		return;
	}
	//An interface reference can be backed by any mutable implementor —
	//the same shared-handle leak a mutable class field has.
	if (pType->Kind() == NK_InterfaceDecl)
	{
		env.Log(CLL_Error, field.Location(),
			"The field \"%s\" of const %s \"%s\" cannot be an interface "
			"type (\"%s\"): an interface may have mutable implementors.",
			field.Name().c_str(), szKindWord, owner.Name().c_str(),
			pType->Name().c_str());
		return;
	}
	if (pType->Kind() != NK_ClassDecl)
		return;	//primitives, string, enums: nothing to seal
	auto *pClass = static_cast<SnClassDecl *>(pType);
	//T binds at instantiation (instantiation-time args are the mint's
	//surface, not the template's); func values are read-only handles.
	if (pClass->IsTypeParamPlaceholder() || pClass->IsFuncType())
		return;
	if (pClass->IsGenericInstantiation()
		&& (pClass->BaseName() == kBuiltinListTypeName
			|| pClass->BaseName() == kBuiltinDictTypeName))
	{
		CheckConstContainerElements(env, owner, szKindWord, field, *pClass);
		return;
	}
	if (!pClass->IsConstAggregate())
		env.Log(CLL_Error, field.Location(),
			"The field \"%s\" of const %s \"%s\" must be a const class "
			"type (\"%s\" is mutable).",
			field.Name().c_str(), szKindWord, owner.Name().c_str(),
			pClass->Name().c_str());
}

//v0.12.4 D6 entry point: the sealed field-type surface of a const
//aggregate, checked at declaration. See the file header for the rules.
inline void CheckConstAggregateFieldSurface(BuildEnvironment &env,
	SnField &owner, const char *szKindWord, SnField &field)
{
	if (field.IsArrayType())
	{
		env.Log(CLL_Error, field.Location(),
			"The const %s \"%s\" cannot hold an array field \"%s\": "
			"use a List<T> field instead.",
			szKindWord, owner.Name().c_str(), field.Name().c_str());
		return;
	}
	SnField *pType = field.EvalDataType();
	if (pType == nullptr)
		return;	//the type expression already failed to resolve
	CheckConstDirectFieldType(env, owner, szKindWord, field, pType);
}

} //namespace nlang
