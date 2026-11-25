/*--- GenericInstDescriptor.cpp — 泛型实例化描述符实现：规范拼写拼接与
    按拼写的进程级intern池（类契约见同目录头文件）。
---*/
#include <nlang/core/GenericInstDescriptor.h>
#include <nlang/core/CompiledModule.h>
#include <nlang/core/CompiledModuleRecords.h>
#include <nlang/runtime/PrimitiveTypes.h>
#include <nlang/core/TypeArgSpelling.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

namespace nlang
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

//--- Load-side residency: canonical-spelling parser -----------------------
//Reversibility per Rust RFC 2603 — the structure defines the spelling,
//so the spelling rebuilds the structure. Every throw below names a
//corrupt image: producers only ever write CanonicalSpelling output.

namespace
{
//The func template is a keyword builtin with no row in any table — its
//handle kind never walks a super chain, so rowRef stays -1 for it (the
//only template keyed by a reserved word, so the bare name is unambiguous).
constexpr const char* kFuncTemplateKey = "func";

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

//0.13.8 item 8 (D8.3): a dotted template name's head must name a
//package the module actually knows — its own modulePath, a live import
//slot's target, or (on linked images, which keep no import records) a
//unit nlink merged in. Anything else is a corrupt image: the dotted
//strip in the bare-or-unique-suffix bridge would silently land the
//name on whatever erasure-bare row shares it, forging identity.
void ValidateDescriptorHead(const std::string& templateName,
	const CompiledModule& module)
{
	const size_t lastDot = templateName.rfind('.');
	if (lastDot == std::string::npos)
		return;
	const std::string head = templateName.substr(0, lastDot);
	if (head == module.modulePath)
		return;
	for (const std::string& path : module.mergedUnitPackagePaths)
		if (path == head)
			return;
	const std::vector<CompiledModule::SymbolImport>* importLists[] = {
		&module.functionImports, &module.classImports,
		&module.structImports, &module.enumImports,
		&module.staticSlotImports};
	for (const auto* pList : importLists)
		for (const CompiledModule::SymbolImport& imp : *pList)
			if (imp.modulePath == head)
				return;
	throw std::runtime_error(
		"corrupt descriptor spelling: unknown package head '"
		+ head + "' in '" + templateName + "'");
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

//Table-resolution tail of ResolveLeafArg: the link-key families
//(struct/class/enum/interface). Spellings carry the qualified link
//identity, so a class leaf resolves through the bare-or-unique-suffix
//bridge — plain-class spellings are exact table keys (module-qualified
//outside the prelude), while a qualified generic-template spelling
//lands on its erasure-bare row through the bridge's dotted-input strip
//(0.13.8 item 8: the row-key space keys templates by the bare
//BaseName). An unresolved key is a corrupt image and throws — the old
//silent rowRef -1 forged identity.
void ResolveLinkKeyLeaf(const std::string& leaf,
	const CompiledModule& module, TypeArgDesc& out)
{
	if (int idx = module.FindStruct(leaf); idx >= 0)
	{
		out.resolvedKind = RTK_Struct;
		out.rowRef = idx;
	}
	else if (int idx = module.FindClassBareOrUniqueSuffix(leaf); idx >= 0)
	{
		out.resolvedKind = RTK_Class;
		out.rowRef = idx;
	}
	else if (int idx = module.FindEnum(leaf); idx >= 0)
	{
		out.resolvedKind = RTK_Int32;
		out.rowRef = idx;
	}
	else if (module.FindInterface(leaf) >= 0)
	{
		//Admitted rowless: consumers are class-row scoped — an interface
		//row index would cross-table collide in ClassMatchesTarget
		//(descriptor arg kind model v2, roadmap 0.13.8). The exact hit
		//is the admission fact.
		out.resolvedKind = RTK_Class;
	}
	else
		throw std::runtime_error(
			"corrupt descriptor spelling: unresolved link key '"
			+ leaf + "'");
}

//Leaf arm: keyword-named leaves (the 12 scalars, `string`, `void`
// - func<void>'s return slot) from the shared vocabulary table, else
//a link key resolved through ResolveLinkKeyLeaf (struct/class/enum/
//interface; enums are int32 at runtime and the spelling keeps the
//enum name).
void ResolveLeafArg(const std::string& leaf, const CompiledModule& module,
	TypeArgDesc& out)
{
	out.spelling = leaf;
	out.rowRef = -1;
	//Keyword leaves (the 12 scalars, `string`, `void`) come from the shared
	//vocabulary table: no module table is involved, and the producer spells
	//the same rows, so a keyword can no longer be known on one side only.
	if (const TypeLeafKeyword* pLeaf = FindTypeLeafKeyword(leaf))
	{
		out.resolvedKind = pLeaf->rtk;
		return;
	}
	ResolveLinkKeyLeaf(leaf, module, out);
}

//Parse one argument substring into a TypeArgDesc: array, nested
//instantiation (its own spelling field is the nested template's
//descriptor spelling, 0.13.8 item 8), or leaf, in that order.
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
		//0.13.8 item 8: the nested template rides the same head gate and
		//must resolve (see ValidateDescriptorHead and ResolveLeafArg) —
		//classes via the bare-or-unique-suffix bridge, interfaces exact
		//(KeyOf spellings), func the keyword builtin rowless. A silent
		//miss forged identity. Non-class hits keep rowRef -1: consumers
		//are class-row scoped (descriptor arg kind model v2, roadmap).
		ValidateDescriptorHead(out.spelling, module);
		if (int classRow =
				module.FindClassBareOrUniqueSuffix(out.spelling);
			classRow >= 0)
			out.rowRef = classRow;
		else if (out.spelling != kFuncTemplateKey
			&& module.FindInterface(out.spelling) < 0)
			throw std::runtime_error(
				"corrupt descriptor spelling: unresolved template row '"
				+ out.spelling + "'");
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
	ValidateDescriptorHead(templateName, module);
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
	//Pool keyed by the canonical name — the descriptor is immutable,
	//so spelling equality IS descriptor equality. Function-local static:
	//initialized on first intern (thread-safe initialization), mutated
	//on the runtime paths only (see the header's contract for why the
	//compile path must never intern) — all serialized within the
	//process, so no pool lock.
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
