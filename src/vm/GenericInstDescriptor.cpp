/*--- GenericInstDescriptor.cpp — 泛型实例化描述符实现：规范拼写拼接与
    按拼写的进程级intern池（类契约见同目录头文件）。
---*/
#include "GenericInstDescriptor.h"
#include <map>
#include <memory>
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

std::string CanonicalArgSpelling(const TypeArgDesc& arg)
{
	if (arg.nested.empty())
		return arg.spelling;
	std::string spelling = arg.spelling;
	spelling += "<";
	for (size_t i = 0; i < arg.nested.size(); ++i)
	{
		if (i)
			spelling += ",";
		spelling += CanonicalArgSpelling(arg.nested[i]);
	}
	spelling += ">";
	return spelling;
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
