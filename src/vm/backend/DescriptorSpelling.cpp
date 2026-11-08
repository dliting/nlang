/*---
    DescriptorSpelling.cpp — AST-side TypeArgDesc builder (header
    contract in this directory). One function per argument shape; the
    array arm's spelling is complete on its own, so CanonicalArgSpelling
    passes it through instead of recursing (see GenericInstDescriptor).
---*/
#include "backend/DescriptorSpelling.h"
#include "GenericInstDescriptor.h"
#include "builder/ModuleRegistry.h"
#include <nlang/vm/CompiledModuleRecords.h>
#include <nlang/compiler/GenericInstanceRegistry.h>
#include <nlang/compiler/SnArrayTypeToken.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnMisc.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <string>
#include <vector>

namespace nlang
{

namespace
{
//Array spelling: element spelling + one bracket pair whose inner comma
//count encodes the rank ("int[]", "int[,]"), matching SnArrayTypeToken's
//display form.
std::string ArraySpelling(const std::string& elemSpelling, uint8_t rank)
{
	std::string spelling = elemSpelling;
	spelling += "[";
	for (uint8_t i = 1; i < rank; ++i)
		spelling += ",";
	spelling += "]";
	return spelling;
}
} //namespace

static TypeArgDesc BuildArgOf(const ModuleRegistry& reg, const SnField& arg)
{
	TypeArgDesc desc;
	//Scalar family (int/float/byte../bool/char): canonical keyword name
	//and the RTK the dispatch paths read.
	if (int pi = ScalarPrimIndexOf(arg.Kind()); pi >= 0)
	{
		desc.spelling = kScalarPrims[pi].name;
		desc.resolvedKind = kScalarPrims[pi].rtk;
		return desc;
	}
	switch (arg.Kind())
	{
	case NK_String:
		desc.spelling = "string";
		desc.resolvedKind = RTK_String;
		return desc;
	case NK_StructDecl:
		desc.spelling = LinkKeyOf(reg, arg);
		desc.resolvedKind = RTK_Struct;
		return desc;
	//Enums are int32 at runtime (spec §4): the inline compare and the
	//as-dispatch are the int32 paths; the spelling keeps the enum name
	//for typeName.
	case NK_EnumDecl:
		desc.spelling = LinkKeyOf(reg, arg);
		desc.resolvedKind = RTK_Int32;
		return desc;
	case NK_ArrayTypeToken:
	{
		auto& token = static_cast<const SnArrayTypeToken&>(arg);
		TypeArgDesc elem = BuildArgOf(reg, *token.ElemTypeOf());
		desc.spelling = ArraySpelling(elem.spelling, token.Rank());
		desc.resolvedKind = RTK_Array;
		desc.rank = token.Rank();
		desc.nested.push_back(std::move(elem));
		return desc;
	}
	case NK_ClassDecl:
	{
		auto& cls = static_cast<const SnClassDecl&>(arg);
		//A type-parameter placeholder: position-encoded, runtime-
		//composable. The spelling is the parameter name — diagnostic
		//surface only; canonical keys never contain it (the recipe
		//arm replaces the argument at runtime).
		if (cls.IsTypeParamPlaceholder())
		{
			const size_t ordinal =
				reg.DeclaredGenerics().PlaceholderIndex(&arg);
			desc.spelling = arg.Name();
			desc.resolvedKind = RTK_TypeParam;
			desc.rowRef = kTypeParamArgBase
				- static_cast<int32_t>(ordinal);
			return desc;
		}
		//A nested instantiation: bare template name per the canonical
		//contract, the argument tree rides `nested`.
		if (cls.IsGenericInstantiation())
		{
			desc.spelling = cls.BaseName();
			desc.resolvedKind = RTK_Class;
			desc.nested = BuildTypeArgsOf(reg, cls);
			return desc;
		}
		//An ordinary class/interface: link key; rows are per-module and
		//remap at link time, so the runtime resolves by name, not row.
		desc.spelling = LinkKeyOf(reg, arg);
		desc.resolvedKind = RTK_Class;
		return desc;
	}
	default:
		//Unreachable through resolved instantiations (func arguments
		//are rejected at the use site); degrade to a non-sentinel
		//spelling so an intern key never collides with a real one.
		desc.spelling = "?";
		desc.resolvedKind = RTK_NonSerialized;
		return desc;
	}
}

std::vector<TypeArgDesc> BuildTypeArgsOf(const ModuleRegistry& reg,
	const SnClassDecl& inst)
{
	std::vector<TypeArgDesc> args;
	args.reserve(inst.GenericTypeArgs().size());
	for (const SnField* pArg : inst.GenericTypeArgs())
		args.push_back(BuildArgOf(reg, *pArg));
	return args;
}

bool HasTypeParamArg(const std::vector<TypeArgDesc>& args)
{
	for (const auto& arg : args)
	{
		if (IsTypeParamArg(arg))
			return true;
		//A nested instantiation containing a type parameter anywhere in
		//its subtree is composable too.
		if (!arg.nested.empty() && HasTypeParamArg(arg.nested))
			return true;
	}
	return false;
}

} //namespace nlang
