/*---
    DescriptorSpelling.cpp — 描述符表行生产：实例化静态行
    （DescriptorIndexFor）与typeName拼接记录行（CompositionIndexFor）。实参分类
    与规范拼写在编译器侧共享词汇（builder/TypeArgSpelling.*）；
    本文件只负责编码与表去重。
---*/
#include "VmBackend.h"
#include <nlang/core/TypeArgSpelling.h>
#include <nlang/core/GenericInstDescriptor.h>
#include "builder/ModuleRegistry.h"
#include <nlang/compiler/BuildEnvironment.h>
#include <nlang/compiler/Logger.h>
#include <nlang/core/CompiledModuleRecords.h>
#include <nlang/compiler/SnExpressions.h>
#include <nlang/compiler/SnMisc.h>
#include <string>
#include <vector>

namespace nlang
{

namespace
{
//Append one u16 in the composition record's little-endian wire order.
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
//length-prefixed child composition record (its spelling field is the
//template's descriptor spelling — 0.13.8 item 8); a concrete leaf spells
//ARG_CONST with the complete leaf form.
void AppendArgComposition(std::vector<uint8_t>& out, const TypeArgDesc& arg)
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
		AppendArgComposition(out, arg.nested[0]);
		return;
	}
	if (!arg.nested.empty())
	{
		//Child composition: TEMPLATE_HEAD with the nested template's
		//descriptor spelling (the argument's spelling field), then its
		//arguments.
		std::vector<uint8_t> child;
		child.push_back(0);
		AppendU16(child, static_cast<uint16_t>(arg.spelling.size()));
		child.insert(child.end(), arg.spelling.begin(), arg.spelling.end());
		for (const auto& nestedArg : arg.nested)
			AppendArgComposition(child, nestedArg);
		out.push_back(4);
		AppendU16(out, static_cast<uint16_t>(child.size()));
		out.insert(out.end(), child.begin(), child.end());
		return;
	}
	AppendLenPrefixed(out, 1, arg.spelling);
}
} //namespace

//0.13.1c Task 8: the shared row core. Two row families, one per argument
//shape, deduped by their exact bytes:
//- composition record row (empty spelling) when an argument carries a type parameter
//  — composes against the executing frame's descriptor cell at runtime
//  (instance, static and free frames alike carry one);
//- interned static row keyed by the complete spelling otherwise.
uint16_t VmBackend::DescriptorRowIndexFor(const std::string& rowName,
	const std::vector<TypeArgDesc>& args)
{
	auto& table = m_compiledModule.descriptorTable;
	if (HasTypeParamArg(args))
	{
		std::vector<uint8_t> composition;
		composition.push_back(0);
		AppendU16(composition, static_cast<uint16_t>(rowName.size()));
		composition.insert(composition.end(), rowName.begin(), rowName.end());
		for (const auto& arg : args)
			AppendArgComposition(composition, arg);
		for (size_t i = 0; i < table.size(); ++i)
			if (table[i].composition == composition)
				return static_cast<uint16_t>(i);
		table.push_back({"", std::move(composition)});
		return static_cast<uint16_t>(table.size() - 1);
	}
	//Static row: key the row by spelling WITHOUT interning. The intern
	//pool is runtime-only; a compile-phase intern would seed it with
	//TypeArgDesc rowRefs of the compiling module, and the linked run's
	//spelling-keyed lookups would then receive that stale entry and
	//miswalk casts/serialization through compile-module rows.
	const std::string spelling = CanonicalSpelling(rowName, args);
	for (size_t i = 0; i < table.size(); ++i)
		if (table[i].spelling == spelling)
			return static_cast<uint16_t>(i);
	table.push_back({spelling, {}});
	return static_cast<uint16_t>(table.size() - 1);
}

//0.13.8 item 8: the row name is the DESCRIPTOR spelling — the
//package-qualified link identity (DescriptorSpellingOf), not the bare
//erasure name LinkKeyOf keeps for row keys. This is where top-level
//descriptor rows become package-distinct.
uint16_t VmBackend::DescriptorIndexFor(SnClassDecl& inst)
{
	if (!inst.IsGenericInstantiation())
		return kNoInstDescriptor;
	const std::vector<TypeArgDesc> args = BuildTypeArgsOf(*m_pRegistry, inst);
	return DescriptorRowIndexFor(DescriptorSpellingOf(*m_pRegistry, inst),
		args);
}

//0.13.1c Task 8: the interface twin — a static call through a generic
//INTERFACE instantiation rides the interface's row (the created shell
//carries the same descriptor spelling/GenericTypeArgs identity the class
//creation does).
uint16_t VmBackend::DescriptorIndexFor(SnInterfaceDecl& inst)
{
	if (!inst.IsGenericInstantiation())
		return kNoInstDescriptor;
	const std::vector<TypeArgDesc> args = BuildTypeArgsOf(*m_pRegistry, inst);
	return DescriptorRowIndexFor(DescriptorSpellingOf(*m_pRegistry, inst),
		args);
}

//0.13.1a Task 4 (D4): a generic FREE-function call site's descriptor row
//— the inferred type arguments under the function's name, through the
//same two-family row core as DescriptorIndexFor (0.13.1c Task 8: the
//composition-record/comment rationale lives there).
uint16_t VmBackend::FreeCallDescriptorIndexFor(const std::string& funcName,
    const std::vector<SnField*>& typeArgs)
{
    std::vector<TypeArgDesc> args;
    args.reserve(typeArgs.size());
    for (const SnField* pArg : typeArgs)
        args.push_back(BuildTypeArgOf(*m_pRegistry, *pArg));
    return DescriptorRowIndexFor(funcName, args);
}

//v0.13.1: the element instantiation's descriptor row for an array
//allocation — 0xFFFF unless the element type is a generic instantiation
//(the erased arrayTypes row cannot tell Box<int>[] from Box<string>[];
//the record's stamped element descriptor does). Bare type-parameter
//elements never reach here (the resolver rejects arrays of T).
uint16_t VmBackend::ElemDescriptorIndexFor(SnField* pElemType)
{
	if (pElemType && pElemType->Kind() == NK_ClassDecl)
		return DescriptorIndexFor(
			static_cast<SnClassDecl&>(*pElemType));
	return kNoInstDescriptor;
}

//v0.13.1: composition record row for a typeName operand that contains a type
//parameter anywhere (the resolver folds concrete operands to a string
//instead — see Access(SnTypeNameExpr)). The row is one argument
//encoding: bare ARG_THIS, ARG_ARRAY (T[]), or an instantiation
//embedding one. Rows stay self-describing by leading tag — an argument
//encoding never starts with 0, so these never alias the instantiation
//rows and byte-identical rows still merge.
uint16_t VmBackend::CompositionIndexFor(const SnField& operand)
{
	const TypeArgDesc arg = BuildTypeArgOf(*m_pRegistry, operand);
	std::vector<uint8_t> composition;
	AppendArgComposition(composition, arg);
	auto& table = m_compiledModule.descriptorTable;
	for (size_t i = 0; i < table.size(); ++i)
		if (table[i].composition == composition)
			return static_cast<uint16_t>(i);
	table.push_back({"", std::move(composition)});
	return static_cast<uint16_t>(table.size() - 1);
}

//v0.13.1 D6: a placeholder type's declared parameter position — the
//ordinal the T operations (OP_Eq_T, OP_TypeArg_to_str) read from the
//receiver's instantiation descriptor. 0xFF (the no-ordinal sentinel,
//SnTypeNameExpr::kNoTypeParamOrdinal) when the type is not a
//placeholder; callers reach here only on resolver-admitted operands.
uint8_t VmBackend::TypeParamOrdinalOfType(const SnField& type)
{
	const TypeArgDesc arg = BuildTypeArgOf(*m_pRegistry, type);
	return IsTypeParamArg(arg)
		? static_cast<uint8_t>(TypeParamOrdinalOf(arg))
		: SnTypeNameExpr::kNoTypeParamOrdinal;
}

} //namespace nlang
