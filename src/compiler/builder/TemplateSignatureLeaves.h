/*---
TemplateSignatureLeaves.h — the template-signature leaf contract,
enforced programmatically (assertions at the stub mint and the
substitution entry) instead of by convention comments or implementor
memory.

A template member's type expression is canonical iff every BARE
identifier leaf is a pre-bound builtin (resolved to an SnBuiltinDataType
— the SnIdentifierExpr NodeKind ctor family); a named reference arrives
wrapped in SnNameExpr (the parser shape; NamedLeafExpr on the stub
side). Composites recurse (generic, array) or pass (wrapped names,
qualified paths — those carry their own resolution and diagnostic
channels). A bare unresolved identifier is the divergent mint that once
cloned every non-T signature of a stub template as void — the contract
exists so that failure mode stops loudly at a checkpoint, never
silently.
---*/
#pragma once
#include "SnExpressions.h"
#include "SnData.h"
#include "SnMisc.h"

namespace nlang
{

//True when every bare identifier leaf in the type expression is a
//pre-bound builtin. Null (an absent return type) is legal.
inline bool TemplateSignatureLeavesAreCanonical(const SnFieldExpr *pType)
{
	if (!pType)
		return true;
	switch (pType->Kind())
	{
		case NK_IdentifierExpr:
		{
			//The contract itself: only the pre-bound builtin family
			//arrives bare.
			const auto &id = *static_cast<const SnIdentifierExpr *>(pType);
			return id.IsResolved() && id.Field() != nullptr
				&& dynamic_cast<const SnBuiltinDataType *>(id.Field())
					!= nullptr;
		}
		case NK_GenericTypeExpr:
		{
			const auto &gen = *static_cast<const SnGenericTypeExpr *>(pType);
			if (!TemplateSignatureLeavesAreCanonical(gen.Base()))
				return false;
			for (const SnFieldExpr *pArg : gen.TypeArgs())
				if (!TemplateSignatureLeavesAreCanonical(pArg))
					return false;
			return true;
		}
		case NK_ArrayTypeExpr:
			return TemplateSignatureLeavesAreCanonical(
				static_cast<const SnArrayTypeExpr *>(pType)
					->ElementType());
		default:
			//Wrapped names and qualified paths pass: this contract
			//rejects exactly the divergent bare mint and nothing else.
			return true;
	}
}

//Class-level form for the stub mint: every member signature (return
//types, formal types, field types) through the leaf contract.
inline bool TemplateClassSignaturesAreCanonical(const SnClassDecl &cls)
{
	for (const SnField &member : cls.Members())
	{
		if (member.Kind() == NK_Function)
		{
			const auto &fn = static_cast<const SnFunction &>(member);
			if (!TemplateSignatureLeavesAreCanonical(fn.ReturnType()))
				return false;
			for (const SnFormalParam &param : fn.Params())
				if (!TemplateSignatureLeavesAreCanonical(param.Type()))
					return false;
		}
		else if (member.Kind() == NK_ClassField)
		{
			if (!TemplateSignatureLeavesAreCanonical(
					static_cast<const SnClassField &>(member).Type()))
				return false;
		}
	}
	return true;
}

} //namespace nlang
