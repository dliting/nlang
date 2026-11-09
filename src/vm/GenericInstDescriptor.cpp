/*--- GenericInstDescriptor.cpp — 泛型实例化描述符实现：规范拼写拼接与
    按拼写的进程级intern池（类契约见同目录头文件）。
---*/
#include "GenericInstDescriptor.h"
#include <nlang/vm/CompiledModule.h>
#include <nlang/vm/CompiledModuleRecords.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

namespace nlang
{

namespace
{
//Canonical no-space join: "Base<Arg1,Arg2>". Distinct from the
//diagnostic render (ExprResolverGenerics.cpp joins with ", ") — the
//canonical form is the intern/serialization key, so it never carries
//presentation spaces. Arguments spell through CanonicalArgSpelling so
//nested instantiations stay distinguishable (the raw spelling field of
//a nested argument is only its bare template name).
std::string CanonicalSpelling(const std::string& templateName,
	const std::vector<TypeArgDesc>& typeArgs)
{
	std::string spelling = templateName;
	spelling += "<";
	for (size_t i = 0; i < typeArgs.size(); ++i)
	{
		if (i)
			spelling += ",";
		spelling += CanonicalArgSpelling(typeArgs[i]);
	}
	spelling += ">";
	return spelling;
}
}

//--- Load-side residency: canonical-spelling parser -----------------------
//Reversibility per Rust RFC 2603 — the structure defines the spelling,
//so the spelling rebuilds the structure. Every throw below names a
//corrupt image: producers only ever write CanonicalSpelling output.

namespace
{
//Split [begin, end) on top-level commas (depth counts <> pairs; the
//array bracket run never contains a comma at array-argument level —
//rank commas live inside the trailing [] group).
std::vector<std::pair<size_t, size_t>> SplitTopLevelArgs(
	const std::string& s, size_t begin, size_t end)
{
	std::vector<std::pair<size_t, size_t>> parts;
	int depth = 0;
	size_t partBegin = begin;
	for (size_t i = begin; i < end; ++i)
	{
		const char c = s[i];
		if (c == '<')
			++depth;
		else if (c == '>')
			--depth;
		else if (c == ',' && depth == 0)
		{
			parts.emplace_back(partBegin, i);
			partBegin = i + 1;
		}
	}
	parts.emplace_back(partBegin, end);
	return parts;
}

//Index of the first top-level '<' in [begin, end), npos when none.
size_t FindTopLevelLess(const std::string& s, size_t begin, size_t end)
{
	for (size_t i = begin; i < end; ++i)
		if (s[i] == '<')
			return i;
	return std::string::npos;
}

//Mutual recursion: the array arm's element parses through the general
//argument parser (an element may itself be a nested instantiation).
bool ParseArgRange(const std::string& s, size_t begin, size_t end,
	const CompiledModule& module, TypeArgDesc& out);

//Array argument arm: ends with ']' and the bracket group's inner run
//is commas only ("int[]", "int[,]", "Box<int>[][]"). Walk left over
//that run to the owning '['; the element is everything before it.
//Returns false when the shape is not an array argument.
bool ParseArrayArgRange(const std::string& s, size_t begin, size_t end,
	const CompiledModule& module, TypeArgDesc& out)
{
	if (s[end - 1] != ']')
		return false;
	size_t open = end - 1;
	uint8_t rank = 1;
	while (open > begin)
	{
		const char c = s[open - 1];
		if (c == '[')
		{
			--open;  //the element range excludes the owning bracket
			break;
		}
		if (c != ',')
			return false;  //inner not comma-only
		++rank;
		--open;
	}
	if (open == begin)
		return false;       //no owning '[' (empty element shape)
	TypeArgDesc elem;
	if (!ParseArgRange(s, begin, open, module, elem))
		return false;
	out.spelling = s.substr(begin, end - begin);
	out.resolvedKind = RTK_Array;
	out.rank = rank;
	out.nested.push_back(std::move(elem));
	return true;
}

//Leaf arm: scalar keyword, string, or a link key resolved against the
//linked module's tables (struct/class/enum; enums are int32 at runtime
//and the spelling keeps the enum name). An unresolved key stays
//RTK_Class with rowRef -1 — display-correct, dispatch degrades
//gracefully.
void ResolveLeafArg(const std::string& leaf, const CompiledModule& module,
	TypeArgDesc& out)
{
	out.spelling = leaf;
	out.rowRef = -1;
	for (size_t pi = 0; pi < kScalarPrimCount; ++pi)
	{
		if (leaf == kScalarPrims[pi].name)
		{
			out.resolvedKind = kScalarPrims[pi].rtk;
			return;
		}
	}
	if (leaf == "string")
	{
		out.resolvedKind = RTK_String;
		return;
	}
	if (int idx = module.FindStruct(leaf); idx >= 0)
	{
		out.resolvedKind = RTK_Struct;
		out.rowRef = idx;
	}
	else if (int idx = module.FindClass(leaf); idx >= 0)
	{
		out.resolvedKind = RTK_Class;
		out.rowRef = idx;
	}
	else if (int idx = module.FindEnum(leaf); idx >= 0)
	{
		out.resolvedKind = RTK_Int32;
		out.rowRef = idx;
	}
	else
		out.resolvedKind = RTK_Class;
}

//Parse one argument substring into a TypeArgDesc: array, nested
//instantiation (its own spelling field is the bare template name per
//the canonical contract), or leaf, in that order.
bool ParseArgRange(const std::string& s, size_t begin, size_t end,
	const CompiledModule& module, TypeArgDesc& out)
{
	if (begin >= end)
		return false;
	if (ParseArrayArgRange(s, begin, end, module, out))
		return true;
	//Nested instantiation: bare template name + top-level argument list.
	const size_t less = FindTopLevelLess(s, begin, end);
	if (less != std::string::npos)
	{
		if (s[end - 1] != '>')
			return false;
		out.spelling = s.substr(begin, less - begin);
		out.resolvedKind = RTK_Class;
		//The template's class row rides the same lookup the leaf arm
		//uses — a row-less argument cannot walk a cast super chain.
		if (int idx = module.FindClass(out.spelling); idx >= 0)
			out.rowRef = idx;
		for (const auto& [aBegin, aEnd] :
			SplitTopLevelArgs(s, less + 1, end - 1))
		{
			TypeArgDesc arg;
			if (!ParseArgRange(s, aBegin, aEnd, module, arg))
				return false;
			out.nested.push_back(std::move(arg));
		}
		return !out.nested.empty();
	}
	ResolveLeafArg(s.substr(begin, end - begin), module, out);
	return true;
}
} //namespace

GenericInstDescriptor* InternDescriptorFromSpelling(
	const std::string& spelling, const CompiledModule& module)
{
	const size_t less = FindTopLevelLess(spelling, 0, spelling.size());
	if (less == std::string::npos || less == 0
		|| spelling.back() != '>')
		throw std::runtime_error(
			"corrupt descriptor spelling: " + spelling);
	const std::string templateName = spelling.substr(0, less);
	std::vector<TypeArgDesc> args;
	for (const auto& [aBegin, aEnd] :
		SplitTopLevelArgs(spelling, less + 1, spelling.size() - 1))
	{
		TypeArgDesc arg;
		if (!ParseArgRange(spelling, aBegin, aEnd, module, arg))
			throw std::runtime_error(
				"corrupt descriptor spelling: " + spelling);
		args.push_back(std::move(arg));
	}
	if (args.empty())
		throw std::runtime_error(
			"corrupt descriptor spelling: " + spelling);
	//Reversibility check: the rebuilt tree must reproduce the key. A
	//mismatch means the parser and the producer disagree — refuse the
	//row instead of interning a descriptor under an alias.
	GenericInstDescriptor* desc =
		InternDescriptor(templateName, args);
	if (desc->Spelling() != spelling)
		throw std::runtime_error(
			"corrupt descriptor spelling: " + spelling);
	return desc;
}

GenericInstDescriptor::GenericInstDescriptor(std::string templateName,
	std::vector<TypeArgDesc> typeArgs)
	: m_templateName(std::move(templateName))
	, m_typeArgs(std::move(typeArgs))
	, m_spelling(CanonicalSpelling(m_templateName, m_typeArgs))
{
}

GenericInstDescriptor* InternDescriptor(const std::string& templateName,
	const std::vector<TypeArgDesc>& typeArgs)
{
	//Pool keyed by the canonical spelling — the descriptor is immutable,
	//so spelling equality IS descriptor equality. Function-local static:
	//initialized on first intern (thread-safe initialization), mutated
	//only on the compile and module-load paths, which the process
	//serializes (see the header's contract).
	static std::map<std::string,
		std::unique_ptr<GenericInstDescriptor>> pool;
	const auto [it, inserted] =
		pool.try_emplace(CanonicalSpelling(templateName, typeArgs));
	if (inserted)
	{
		it->second = std::make_unique<GenericInstDescriptor>(
			templateName, typeArgs);
	}
	return it->second.get();
}

} //namespace nlang
