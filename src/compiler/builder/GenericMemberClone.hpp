/*---
GenericMemberClone.hpp — one generic template member's signature
deep-copy, shared by the class and interface creation TUs.

The copy is the metadata surface only: return type and formal
parameters with their constant defaults (bodies and native bindings
stay with the template row). Constants clone their TYPE only — the
folded initializer stays with the template row, because compliance
reads signatures, never values.
---*/
#pragma once
#include "SnData.h"
#include "SnMisc.h"
#include <functional>
#include <vector>

namespace nlang
{

inline SnField* CloneGenericMember(SnField& src,
	const std::vector<SnField*>& typeArgs,
	const std::function<SnFieldExpr*(SnFieldExpr&)>& substitute,
	const std::function<SnExpression*(SnExpression*)>& cloneDefault)
{
	if (src.Kind() == NK_Function)
	{
		auto& fn = static_cast<SnFunction&>(src);
		SnFieldExpr* pRetCopy = fn.ReturnType()
			? substitute(*fn.ReturnType()) : nullptr;
		auto* pParams = new PtrList<SnFormalParam>();
		for (auto& param : fn.Params())
		{
			auto* pTypeCopy = param.Type()
				? substitute(*param.Type()) : nullptr;
			pParams->push_back(new SnFormalParam(param.Flags(), pTypeCopy,
				new std::string(param.Name()),
				cloneDefault(param.Value()), *param.Location()));
		}
		return new SnFunction(fn.AccessType(), fn.Flags(), pRetCopy,
			new std::string(fn.Name()), pParams, *fn.Location());
	}
	if (src.Kind() == NK_ClassField)
	{
		auto& field = static_cast<SnClassField&>(src);
		auto* pTypeCopy = field.Type()
			? substitute(*field.Type()) : nullptr;
		return new SnClassField(pTypeCopy, new std::string(field.Name()),
			field.AccessType(), *field.Location());
	}
	return nullptr;
}

} //namespace nlang
