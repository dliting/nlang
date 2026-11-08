/*---
    DescriptorSpelling.cpp — AST-side TypeArgDesc builder (header
    contract in this directory). One function per argument shape; the
    array arm's spelling is complete on its own, so CanonicalArgSpelling
    passes it through instead of recursing (see GenericInstDescriptor).
---*/
#include "backend/DescriptorSpelling.h"
#include "GenericInstDescriptor.h"
#include "VmBackend.h"
#include "builder/ModuleRegistry.h"
#include "nlang/compiler/BuildEnvironment.h"
#include "nlang/compiler/Logger.h"
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

//Class-typed argument, three shapes: a type-parameter placeholder
//(position-encoded, runtime-composable; the spelling is the parameter
//name — diagnostic surface only, canonical keys never contain it, the
//recipe arm replaces the argument at runtime), a nested instantiation
//(bare template name per the canonical contract, the argument tree
//rides `nested`), or an ordinary class/interface (link key; rows are
//per-module and remap at link time, so the runtime resolves by name,
//not row).
static TypeArgDesc ClassArgOf(const ModuleRegistry& reg,
	const SnClassDecl& cls)
{
	TypeArgDesc desc;
	if (cls.IsTypeParamPlaceholder())
	{
		const size_t ordinal =
			reg.DeclaredGenerics().PlaceholderIndex(&cls);
		desc.spelling = cls.Name();
		desc.resolvedKind = RTK_TypeParam;
		desc.rowRef = kTypeParamArgBase
			- static_cast<int32_t>(ordinal);
		return desc;
	}
	if (cls.IsGenericInstantiation())
	{
		desc.spelling = cls.BaseName();
		desc.resolvedKind = RTK_Class;
		desc.nested = BuildTypeArgsOf(reg, cls);
		return desc;
	}
	desc.spelling = LinkKeyOf(reg, cls);
	desc.resolvedKind = RTK_Class;
	return desc;
}

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
		return ClassArgOf(reg, static_cast<const SnClassDecl&>(arg));
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

namespace
{
//Append one u16 in the recipe's little-endian wire order.
void AppendU16(std::vector<uint8_t>& out, uint16_t v)
{
	out.push_back(static_cast<uint8_t>(v & 0xFF));
	out.push_back(static_cast<uint8_t>(v >> 8));
}

void AppendLenPrefixed(std::vector<uint8_t>& out, uint8_t tag,
	const std::string& bytes)
{
	out.push_back(tag);
	AppendU16(out, static_cast<uint16_t>(bytes.size()));
	out.insert(out.end(), bytes.begin(), bytes.end());
}

//One argument's encoding (see DescriptorEntry): a placeholder spells
//ARG_THIS with its declared-parameter ordinal; an array wraps its
//element's encoding with the rank; a nested instantiation embeds a
//length-prefixed child recipe (its spelling field is the bare template
//name); a concrete leaf spells ARG_CONST with the complete leaf form.
void AppendArgRecipe(std::vector<uint8_t>& out, const TypeArgDesc& arg)
{
	if (IsTypeParamArg(arg))
	{
		out.push_back(2);
		out.push_back(static_cast<uint8_t>(TypeParamOrdinalOf(arg)));
		return;
	}
	if (arg.resolvedKind == RTK_Array && !arg.nested.empty())
	{
		out.push_back(3);
		out.push_back(arg.rank);
		AppendArgRecipe(out, arg.nested[0]);
		return;
	}
	if (!arg.nested.empty())
	{
		//Child recipe: TEMPLATE_HEAD with the nested template's bare
		//name (the argument's spelling field), then its arguments.
		std::vector<uint8_t> child;
		child.push_back(0);
		AppendU16(child, static_cast<uint16_t>(arg.spelling.size()));
		child.insert(child.end(), arg.spelling.begin(), arg.spelling.end());
		for (const auto& nestedArg : arg.nested)
			AppendArgRecipe(child, nestedArg);
		out.push_back(4);
		AppendU16(out, static_cast<uint16_t>(child.size()));
		out.insert(out.end(), child.begin(), child.end());
		return;
	}
	AppendLenPrefixed(out, 1, arg.spelling);
}
} //namespace

uint16_t VmBackend::DescriptorIndexFor(SnClassDecl& inst)
{
	if (!inst.IsGenericInstantiation())
		return kNoInstDescriptor;
	const std::vector<TypeArgDesc> args = BuildTypeArgsOf(*m_pRegistry, inst);
	auto& table = m_compiledModule.descriptorTable;
	if (HasTypeParamArg(args))
	{
		//A static member has no receiver at run time, so the composition
		//would have nothing to read. The declaration is legal (statics
		//may reference T); the construction is not.
		if (m_currFunc && m_currFunc->func
			&& m_currFunc->func->isStatic)
		{
			m_pEnv->Log(CLL_Error, inst.Location(),
				"constructing a generic type with type-parameter "
				"arguments requires an instance context.");
			return kNoInstDescriptor;
		}
		std::vector<uint8_t> recipe;
		recipe.push_back(0);
		const std::string baseName = inst.BaseName();
		AppendU16(recipe, static_cast<uint16_t>(baseName.size()));
		recipe.insert(recipe.end(), baseName.begin(), baseName.end());
		for (const auto& arg : args)
			AppendArgRecipe(recipe, arg);
		for (size_t i = 0; i < table.size(); ++i)
			if (table[i].recipe == recipe)
				return static_cast<uint16_t>(i);
		table.push_back({"", std::move(recipe)});
		return static_cast<uint16_t>(table.size() - 1);
	}
	GenericInstDescriptor* desc = InternDescriptor(inst.BaseName(), args);
	for (size_t i = 0; i < table.size(); ++i)
		if (table[i].spelling == desc->Spelling())
			return static_cast<uint16_t>(i);
	table.push_back({desc->Spelling(), {}});
	return static_cast<uint16_t>(table.size() - 1);
}

} //namespace nlang
